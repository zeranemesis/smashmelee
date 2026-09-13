#ifdef MELEE_BOOTSTRAP

#include "hsd/host_runtime.hpp"
#include "hsd/animation.hpp"
#include "hsd/animation_player.hpp"
#include "hsd/scene.hpp"
#include "hsd/scene_renderer.hpp"

#include <melee/port/disc_mount.hpp>
#include <melee/sysdolphin/baselib/archive.hpp>

#include "../port/ui/ui.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/lib/logging.hpp>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <port/main.h>
#include <port/settings.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

aurora::Module MeleeBootstrapLog("meleeboard::bootstrap");

constexpr std::array<const char*, 5> kMainMenuNames = {
    "1-P Mode", "VS. Mode", "Trophies", "Options", "Data",
};
constexpr std::array<const char*, 5> kVsMenuNames = {
    "Melee", "Tournament Melee", "Special Melee", "Rules", "Name Entry",
};

enum class MenuPage : uint8_t {
    Main,
    Versus,
    CharacterSelect,
};

struct MainMenuInput {
    uint16_t previous_buttons = 0;
    int8_t previous_stick_direction = 0;
    uint8_t selection = 0;

    bool poll(bool& confirm, bool& back)
    {
        PADStatus pads[PAD_MAX_CONTROLLERS]{};
        PADRead(pads);
        uint16_t buttons = 0;
        int8_t stick_direction = 0;
        for (const PADStatus& pad : pads) {
            if (pad.err != PAD_ERR_NONE) continue;
            buttons |= pad.button;
            if (pad.stickY >= 40) stick_direction = 1;
            else if (pad.stickY <= -40) stick_direction = -1;
        }
        const uint16_t pressed = buttons & ~previous_buttons;
        const bool stick_pressed = stick_direction != 0 &&
            stick_direction != previous_stick_direction;
        previous_buttons = buttons;
        previous_stick_direction = stick_direction;
        confirm = (pressed & (PAD_BUTTON_A | PAD_BUTTON_START)) != 0;
        back = (pressed & PAD_BUTTON_B) != 0;

        int direction = 0;
        if ((pressed & PAD_BUTTON_UP) != 0 ||
            (stick_pressed && stick_direction > 0)) direction = -1;
        else if ((pressed & PAD_BUTTON_DOWN) != 0 ||
                 (stick_pressed && stick_direction < 0)) direction = 1;
        if (direction == 0) return false;
        selection = static_cast<uint8_t>(
            (selection + kMainMenuNames.size() + direction) %
            kMainMenuNames.size());
        return true;
    }
};

} // namespace

extern "C" int game_main(void)
{
    const std::string disc_path = partyboard::getSettings().backend.isoPath.getValue();
    if (!meleeboard::disc::mount(disc_path)) {
        MeleeBootstrapLog.error("Could not mount the selected GALE01 v1.02 disc image");
        return 1;
    }
    std::vector<unsigned char> archive_bytes;
    if (!meleeboard::disc::read_file("GmRgStnd.dat", archive_bytes)) {
        MeleeBootstrapLog.error("Mounted disc is missing GmRgStnd.dat");
        meleeboard::disc::unmount();
        return 1;
    }
    meleeboard::hsd::Archive archive;
    if (!archive.parse(std::move(archive_bytes))) {
        MeleeBootstrapLog.error("Could not parse GmRgStnd.dat");
        meleeboard::disc::unmount();
        return 1;
    }
    const auto archive_symbols = archive.public_symbols();
    MeleeBootstrapLog.info("GmRgStnd.dat exports {} HSD symbols", archive_symbols.size());
    for (const std::string& symbol : archive_symbols) {
        if (symbol.find("anim_joint") != std::string::npos ||
            symbol.find("animjoint") != std::string::npos) {
            MeleeBootstrapLog.info("GmRgStnd.dat animation symbol: {}", symbol);
        }
    }

    // The reference main-menu flow loads this exact archive/symbol pair in
    // mnmain.c. Validate the native animation path against the user's disc,
    // but keep the standScene bootstrap usable if optional menu data is absent.
    meleeboard::hsd::HostScene menu_model;
    meleeboard::hsd::HostAnimation menu_animation;
    meleeboard::hsd::HostScene menu_panel;
    meleeboard::hsd::HostAnimation menu_panel_animation;
    meleeboard::hsd::HostScene menu_content;
    meleeboard::hsd::HostAnimation menu_content_animation;
    std::array<meleeboard::hsd::HostScene, 5> menu_cursors;
    meleeboard::hsd::HostAnimation menu_cursor_animation;
    meleeboard::hsd::HostScene character_select_model;
    meleeboard::hsd::HostAnimation character_select_animation;
    bool menu_ready = false;
    bool menu_panel_ready = false;
    bool menu_content_ready = false;
    bool menu_cursors_ready = false;
    std::vector<unsigned char> menu_bytes;
    if (meleeboard::disc::read_file("MnMaAll.dat", menu_bytes)) {
        meleeboard::hsd::Archive menu_archive;
        if (menu_archive.parse(std::move(menu_bytes)) &&
            menu_animation.load(menu_archive, "MenMainBack_Top_animjoint")) {
            const auto model_joints =
                menu_archive.joint_tree_count("MenMainBack_Top_joint");
            const bool model_loaded =
                menu_model.load_joint(menu_archive, "MenMainBack_Top_joint");
            const bool camera_loaded = model_loaded && menu_model.load_camera(
                menu_archive, "ScMenMain_cam_int1_camera");
            menu_ready = model_loaded && camera_loaded && model_joints.has_value() &&
                *model_joints == menu_animation.joints().size();
            MeleeBootstrapLog.info(
                "Validated MnMaAll.dat main-menu data: {} animation joints, {} model joints, {} draw objects ({})",
                menu_animation.joints().size(), model_joints.value_or(0),
                menu_model.draw_objects().size(),
                menu_ready ? "model/camera decoded" : menu_model.last_error());
            const auto panel_joints =
                menu_archive.joint_tree_count("MenMainPanel_Top_joint");
            menu_panel_ready = menu_panel_animation.load(
                    menu_archive, "MenMainPanel_Top_animjoint") &&
                menu_panel.load_joint(menu_archive, "MenMainPanel_Top_joint") &&
                menu_panel.load_camera(menu_archive, "ScMenMain_cam_int1_camera") &&
                panel_joints.has_value() &&
                *panel_joints == menu_panel_animation.joints().size();
            MeleeBootstrapLog.info(
                "MnMaAll.dat main-menu panel: {} animation joints, {} model joints, {} draw objects ({})",
                menu_panel_animation.joints().size(), panel_joints.value_or(0),
                menu_panel.draw_objects().size(),
                menu_panel_ready ? "panel decoded" : menu_panel.last_error());
            const auto content_joints =
                menu_archive.joint_tree_count("MenMainConTop_Top_joint");
            menu_content_ready = menu_content_animation.load(
                    menu_archive, "MenMainConTop_Top_animjoint") &&
                menu_content.load_joint(menu_archive, "MenMainConTop_Top_joint") &&
                menu_content.load_camera(menu_archive, "ScMenMain_cam_int1_camera") &&
                content_joints.has_value() &&
                *content_joints == menu_content_animation.joints().size();
            MeleeBootstrapLog.info(
                "MnMaAll.dat main-menu choices: {} animation joints, {} model joints, {} draw objects ({})",
                menu_content_animation.joints().size(), content_joints.value_or(0),
                menu_content.draw_objects().size(),
                menu_content_ready ? "choices decoded" : menu_content.last_error());
            const auto cursor_joints =
                menu_archive.joint_tree_count("MenMainCursor_Top_joint");
            menu_cursors_ready = menu_cursor_animation.load(
                    menu_archive, "MenMainCursor_Top_animjoint") &&
                cursor_joints.has_value() &&
                *cursor_joints == menu_cursor_animation.joints().size();
            for (auto& cursor : menu_cursors) {
                menu_cursors_ready = menu_cursors_ready &&
                    cursor.load_joint(menu_archive, "MenMainCursor_Top_joint") &&
                    cursor.load_camera(menu_archive, "ScMenMain_cam_int1_camera");
            }
            MeleeBootstrapLog.info(
                "MnMaAll.dat main-menu cursor: {} animation joints, {} model joints, {} draw objects ({})",
                menu_cursor_animation.joints().size(), cursor_joints.value_or(0),
                menu_cursors.front().draw_objects().size(),
                menu_cursors_ready ? "five instances decoded" :
                                     menu_cursors.front().last_error());
        } else {
            MeleeBootstrapLog.warn(
                "Could not materialize MnMaAll.dat's MenMainBack animation");
        }
    }
    bool character_select_ready = false;
    std::vector<unsigned char> character_select_bytes;
    if (meleeboard::disc::read_file("MnSlChr.usd", character_select_bytes)) {
        meleeboard::hsd::Archive character_select_archive;
        if (character_select_archive.parse(std::move(character_select_bytes))) {
            // MnSelectChrDataTable starts with CSSSceneModels (four pointers),
            // followed by CSSAnimSet entries (four pointers each).  VS uses
            // ANIM[3], exactly as mnCharSel_802640A0 does upstream.
            const auto table = character_select_archive.public_symbol_offset(
                "MnSelectChrDataTable");
            constexpr uint32_t kSceneModelsSize = 0x10;
            constexpr uint32_t kAnimSetSize = 0x10;
            constexpr uint32_t kVersusAnimSet = 3;
            if (table.has_value()) {
                const uint32_t animation_set = *table + kSceneModelsSize +
                    kVersusAnimSet * kAnimSetSize;
                const auto camera = character_select_archive.data_pointer(*table);
                const auto joint =
                    character_select_archive.data_pointer(animation_set);
                const auto animation =
                    character_select_archive.data_pointer(animation_set + 0x04);
                character_select_ready = camera.has_value() && joint.has_value() &&
                    animation.has_value() &&
                    character_select_model.load_joint_at(
                        character_select_archive, *joint) &&
                    character_select_model.load_camera_at(
                        character_select_archive, *camera) &&
                    character_select_animation.load_at(
                        character_select_archive, *animation) &&
                    character_select_model.joints().size() ==
                        character_select_animation.joints().size();
            }
        }
    }
    MeleeBootstrapLog.info(
        "MnSlChr.usd VS character-select root: {} animation joints, {} model joints, {} draw objects ({})",
        character_select_animation.joints().size(),
        character_select_model.joints().size(),
        character_select_model.draw_objects().size(),
        character_select_ready ? "model/camera decoded" :
                                 character_select_model.last_error());
    const auto stand_scene = archive.scene_roots("standScene");
    const auto model_count = archive.scene_model_count("standScene");
    const auto joint_count = archive.scene_joint_count("standScene");
    meleeboard::hsd::HostScene host_scene;
    if (!stand_scene.has_value() || !model_count.has_value() ||
        !joint_count.has_value() || !host_scene.load(archive, "standScene")) {
        MeleeBootstrapLog.error(
            "Could not decode GmRgStnd.dat's standScene graph ({})",
            host_scene.last_error());
        VISetWindowTitle(host_scene.last_error().c_str());
        meleeboard::disc::unmount();
        return 1;
    }
    MeleeBootstrapLog.debug("standScene materialization complete");
    if (!meleeboard::hsd::initialize_host_runtime()) {
        MeleeBootstrapLog.error("HSD host runtime self-test failed");
        meleeboard::disc::unmount();
        return 1;
    }
    MeleeBootstrapLog.debug("HSD host runtime self-test complete");
    meleeboard::hsd::HostAnimationPlayer menu_player;
    meleeboard::hsd::HostAnimationPlayer menu_panel_player;
    meleeboard::hsd::HostAnimationPlayer menu_content_player;
    std::array<meleeboard::hsd::HostAnimationPlayer, 5> menu_cursor_players;
    meleeboard::hsd::HostAnimationPlayer character_select_player;
    if (menu_ready) {
        std::vector<uint32_t> mapping(menu_animation.joints().size());
        for (uint32_t index = 0; index < mapping.size(); ++index) {
            mapping[index] = index;
        }
        menu_ready = menu_player.attach(menu_animation, menu_model.joints(), mapping);
        MeleeBootstrapLog.debug("MenMainBack animation attach: {}", menu_ready);
    }
    if (menu_panel_ready) {
        std::vector<uint32_t> mapping(menu_panel_animation.joints().size());
        for (uint32_t index = 0; index < mapping.size(); ++index) {
            mapping[index] = index;
        }
        menu_panel_ready = menu_panel_player.attach(
            menu_panel_animation, menu_panel.joints(), mapping);
        MeleeBootstrapLog.debug("MenMainPanel animation attach: {}", menu_panel_ready);
    }
    if (menu_content_ready) {
        std::vector<uint32_t> mapping(menu_content_animation.joints().size());
        for (uint32_t index = 0; index < mapping.size(); ++index) {
            mapping[index] = index;
        }
        menu_content_ready = menu_content_player.attach(
            menu_content_animation, menu_content.joints(), mapping);
        MeleeBootstrapLog.debug("MenMainConTop animation attach: {}", menu_content_ready);
        // mn_8022B3A0 applies selection zero's hover loop to traversal node 14.
        menu_content_ready = menu_content_ready &&
            menu_content_player.request_subtree(14, 0.0F);
        for (uint32_t option = 0; option < kMainMenuNames.size(); ++option) {
            menu_content_ready = menu_content_ready &&
                menu_content_player.request_joint(4 + option, 5.0F);
        }
    }
    if (menu_cursors_ready) {
        for (uint32_t option = 0; option < menu_cursors.size(); ++option) {
            std::vector<uint32_t> mapping(menu_cursor_animation.joints().size());
            for (uint32_t index = 0; index < mapping.size(); ++index) {
                mapping[index] = index;
            }
            menu_cursors_ready = menu_cursors_ready &&
                menu_cursor_players[option].attach(
                    menu_cursor_animation, menu_cursors[option].joints(), mapping);
        }
    }
    if (character_select_ready) {
        std::vector<uint32_t> mapping(character_select_animation.joints().size());
        for (uint32_t index = 0; index < mapping.size(); ++index) {
            mapping[index] = index;
        }
        character_select_ready = character_select_player.attach(
            character_select_animation, character_select_model.joints(), mapping);
        if (character_select_ready) {
            character_select_player.request(0.0F);
            character_select_player.tick();
        }
        MeleeBootstrapLog.debug("MnSlChr VS animation attach: {}",
                                character_select_ready);
    }
    MeleeBootstrapLog.info(
        "Mounted {}; materialized standScene with {} models, {} joints, {} draw objects, {} materials, and {} direct textures (M={:#x} C={:#x} L={:#x} F={:#x}); entering bootstrap loop",
        meleeboard::disc::mounted_path(), host_scene.model_roots().size(),
        host_scene.joints().size(), host_scene.draw_objects().size(),
        host_scene.materials().size(), host_scene.textures().size(),
        stand_scene->models, stand_scene->cameras, stand_scene->lights,
        stand_scene->fogs);
    meleeboard::hsd::HostScene& displayed_scene = menu_ready ? menu_model : host_scene;
    meleeboard::hsd::MeleeSceneRenderer scene_renderer(displayed_scene);
    std::unique_ptr<meleeboard::hsd::MeleeSceneRenderer> panel_renderer;
    std::unique_ptr<meleeboard::hsd::MeleeSceneRenderer> content_renderer;
    std::array<std::unique_ptr<meleeboard::hsd::MeleeSceneRenderer>, 5>
        cursor_renderers;
    std::unique_ptr<meleeboard::hsd::MeleeSceneRenderer>
        character_select_renderer;
    if (menu_ready && menu_panel_ready) {
        panel_renderer =
            std::make_unique<meleeboard::hsd::MeleeSceneRenderer>(menu_panel);
    }
    if (menu_ready && menu_content_ready) {
        content_renderer =
            std::make_unique<meleeboard::hsd::MeleeSceneRenderer>(menu_content);
        MeleeBootstrapLog.info(
            "MenMainConTop renderer: {} drawable objects, {} skipped objects, {} triangles",
            content_renderer->drawable_object_count(),
            content_renderer->skipped_object_count(),
            content_renderer->submitted_triangle_count());
    }
    if (menu_ready && menu_content_ready && menu_cursors_ready) {
        for (uint32_t option = 0; option < menu_cursors.size(); ++option) {
            cursor_renderers[option] =
                std::make_unique<meleeboard::hsd::MeleeSceneRenderer>(
                    menu_cursors[option]);
            cursor_renderers[option]->attach_roots_to(menu_content, 4 + option);
        }
        MeleeBootstrapLog.info(
            "MenMainCursor renderers: 5 instances, {} drawable objects each, {} triangles each",
            cursor_renderers.front()->drawable_object_count(),
            cursor_renderers.front()->submitted_triangle_count());
    }
    if (character_select_ready) {
        character_select_renderer =
            std::make_unique<meleeboard::hsd::MeleeSceneRenderer>(
                character_select_model);
        MeleeBootstrapLog.info(
            "MnSlChr VS renderer: {} drawable objects, {} skipped objects, {} triangles",
            character_select_renderer->drawable_object_count(),
            character_select_renderer->skipped_object_count(),
            character_select_renderer->submitted_triangle_count());
    }
    MeleeBootstrapLog.info(
        "{} renderer: {} drawable objects, {} skipped objects, {} triangles",
        menu_ready ? "MenMainBack" : "standScene",
        scene_renderer.drawable_object_count(), scene_renderer.skipped_object_count(),
        scene_renderer.submitted_triangle_count());

    // Melee's HSD/GObj scene work advances on the NTSC 60 Hz cadence, while
    // Aurora may present faster or slower depending on the host display.
    // Bound catch-up after a debugger pause so an overloaded machine does not
    // spend an unbounded frame simulating stale input.
    using Clock = std::chrono::steady_clock;
    constexpr std::chrono::duration<double> kSimulationStep{ 1.0 / 60.0 };
    constexpr auto kMaxElapsed = std::chrono::milliseconds(250);
    constexpr uint32_t kMaxCatchUpSteps = 4;
    auto previous_tick = Clock::now();
    std::chrono::duration<double> simulation_accumulator = kSimulationStep;
    MainMenuInput menu_input;
    MenuPage menu_page = MenuPage::Main;
    uint32_t hover_ticks = 0;
    PADInit();
    const auto configure_cursors = [&](uint8_t selected,
                                       float cursor_frame_base) {
        if (!menu_cursors_ready) return;
        for (uint32_t option = 0; option < menu_cursor_players.size(); ++option) {
            auto& player = menu_cursor_players[option];
            const bool hovered = option == selected;
            player.request_joint(2, hovered ? 50.0F : 0.0F);
            player.request_subtree(3, hovered ? 1.0F : 0.0F);
            player.request_joint(
                3, cursor_frame_base + static_cast<float>(option * 2));
            player.request_subtree(4, hovered ? 50.0F : 49.0F);
            player.request_joint(9, 0.0F);
            player.set_subtree_hidden(9, !hovered);
            player.set_subtree_hidden(11, !hovered);
            // HSD_JObjAnim/HSD_JObjAnimAll are immediate in mn_8022B3A0.
            // Apply the requested cursor pose once; continuously advancing
            // every attached AObj would animate dormant branches that the
            // original scene process leaves stopped.
            player.tick();
        }
    };
    const auto apply_menu_selection = [&](float hover_start,
                                          float cursor_frame_base) {
        hover_ticks = 0;
        menu_content_player.request_subtree(14, hover_start);
        configure_cursors(menu_input.selection, cursor_frame_base);
    };
    configure_cursors(menu_input.selection, 0.0F);

    while (PartyBoard_IsRunning) {
        const AuroraEvent* event = aurora_update();
        while (event != nullptr && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) {
                PartyBoard_IsRunning = false;
                break;
            }
            if (event->type == AURORA_SDL_EVENT) {
                partyboard::ui::handle_event(event->sdl);
            }
            ++event;
        }

        if (!PartyBoard_IsRunning) {
            break;
        }
        if (!aurora_begin_frame()) {
            continue;
        }
        const auto now = Clock::now();
        simulation_accumulator += std::min(
            now - previous_tick,
            std::chrono::duration_cast<Clock::duration>(kMaxElapsed));
        previous_tick = now;
        uint32_t steps = 0;
        while (simulation_accumulator >= kSimulationStep &&
               steps < kMaxCatchUpSteps) {
            meleeboard::hsd::tick_host_runtime();
            if (menu_ready && menu_page != MenuPage::CharacterSelect) {
                menu_player.tick();
            }
            if (menu_panel_ready && menu_page != MenuPage::CharacterSelect) {
                menu_panel_player.tick();
            }
            if (menu_content_ready) {
                bool confirm = false;
                bool back = false;
                const bool moved = menu_input.poll(confirm, back);
                if (menu_page == MenuPage::CharacterSelect) {
                    if (back) {
                        menu_page = MenuPage::Versus;
                        menu_input.selection = 0;
                        apply_menu_selection(700.0F, 40.0F);
                        VISetWindowTitle("Melee native port - Melee");
                        MeleeBootstrapLog.info(
                            "Returned from native character select to VS submenu");
                    }
                } else {
                    const auto& names = menu_page == MenuPage::Main ?
                        kMainMenuNames : kVsMenuNames;
                    const float hover_base = menu_page == MenuPage::Main ?
                        0.0F : 700.0F;
                    const float cursor_frame_base = menu_page == MenuPage::Main ?
                        0.0F : 40.0F;
                    if (moved) {
                        const float start_frame = hover_base +
                            static_cast<float>(menu_input.selection) * 50.0F;
                        apply_menu_selection(start_frame, cursor_frame_base);
                        const std::string title =
                            std::string("Melee native port - ") +
                            names[menu_input.selection];
                        VISetWindowTitle(title.c_str());
                        MeleeBootstrapLog.info("Menu selection: {} ({})",
                                               names[menu_input.selection],
                                               menu_input.selection);
                    }
                    if (confirm) {
                        if (menu_page == MenuPage::Main &&
                            menu_input.selection == 1) {
                            // mn_8022DB10 enters MENU_KIND_VS with selection zero.
                            menu_page = MenuPage::Versus;
                            menu_input.selection = 0;
                            apply_menu_selection(700.0F, 40.0F);
                            VISetWindowTitle("Melee native port - Melee");
                            MeleeBootstrapLog.info(
                                "Entered native VS submenu (MENU_KIND_VS)");
                        } else if (menu_page == MenuPage::Versus &&
                                   menu_input.selection == 0) {
                            if (character_select_ready) {
                                menu_page = MenuPage::CharacterSelect;
                                VISetWindowTitle(
                                    "Melee native port - Character Select");
                                MeleeBootstrapLog.info(
                                    "Entered native GM_VS character-select visual");
                            } else {
                                MeleeBootstrapLog.warn(
                                    "VS Melee confirmed, but MnSlChr.usd is not renderable");
                            }
                        } else {
                            MeleeBootstrapLog.info(
                                "Menu confirm requested for {}; native transition not ported yet",
                                names[menu_input.selection]);
                        }
                    } else if (back && menu_page == MenuPage::Versus) {
                        // mn_8022D594 returns to the main page with VS selected.
                        menu_page = MenuPage::Main;
                        menu_input.selection = 1;
                        apply_menu_selection(50.0F, 0.0F);
                        VISetWindowTitle("Melee native port - VS. Mode");
                        MeleeBootstrapLog.info("Returned to native main menu");
                    }
                }
                ++hover_ticks;
                if (menu_page != MenuPage::CharacterSelect &&
                    (hover_ticks == 50 ||
                     (hover_ticks > 50 && (hover_ticks - 50) % 30 == 0))) {
                    const float active_hover_base =
                        menu_page == MenuPage::Main ? 0.0F : 700.0F;
                    const float loop_frame = active_hover_base +
                        static_cast<float>(menu_input.selection) * 50.0F + 20.0F;
                    menu_content_player.request_subtree(14, loop_frame);
                }
                if (menu_page != MenuPage::CharacterSelect) {
                    menu_content_player.tick();
                }
            }
            simulation_accumulator -= kSimulationStep;
            ++steps;
        }
        if (steps == kMaxCatchUpSteps && simulation_accumulator >= kSimulationStep) {
            // Drop stale time rather than running a simulation catch-up loop
            // disconnected from current controller state.
            simulation_accumulator = std::chrono::duration<double>::zero();
        }
        if (menu_page == MenuPage::CharacterSelect &&
            character_select_renderer != nullptr) {
            character_select_renderer->render();
        } else {
            scene_renderer.render();
            if (panel_renderer != nullptr) {
                panel_renderer->render();
            }
            if (content_renderer != nullptr) {
                content_renderer->render();
            }
            for (const auto& renderer : cursor_renderers) {
                if (renderer != nullptr) renderer->render();
            }
        }
        aurora_end_frame();
    }

    meleeboard::hsd::shutdown_host_runtime();
    meleeboard::disc::unmount();
    return 0;
}

#endif
