#ifdef MELEE_BOOTSTRAP

#include "hsd/host_runtime.hpp"
#include "hsd/animation.hpp"
#include "hsd/scene.hpp"
#include "hsd/scene_renderer.hpp"

#include <melee/port/disc_mount.hpp>
#include <melee/sysdolphin/baselib/archive.hpp>

#include "../port/ui/ui.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/lib/logging.hpp>
#include <dolphin/vi.h>
#include <port/main.h>
#include <port/settings.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

aurora::Module MeleeBootstrapLog("meleeboard::bootstrap");

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
    std::vector<unsigned char> menu_bytes;
    if (meleeboard::disc::read_file("MnMaAll.dat", menu_bytes)) {
        meleeboard::hsd::Archive menu_archive;
        meleeboard::hsd::HostAnimation menu_animation;
        if (menu_archive.parse(std::move(menu_bytes)) &&
            menu_animation.load(menu_archive, "MenMainBack_Top_animjoint")) {
            const auto model_joints =
                menu_archive.joint_tree_count("MenMainBack_Top_joint");
            MeleeBootstrapLog.info(
                "Validated MnMaAll.dat main-menu animation: {} animation joints, {} model joints",
                menu_animation.joints().size(), model_joints.value_or(0));
        } else {
            MeleeBootstrapLog.warn(
                "Could not materialize MnMaAll.dat's MenMainBack animation");
        }
    }
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
    if (!meleeboard::hsd::initialize_host_runtime()) {
        MeleeBootstrapLog.error("HSD host runtime self-test failed");
        meleeboard::disc::unmount();
        return 1;
    }
    MeleeBootstrapLog.info(
        "Mounted {}; materialized standScene with {} models, {} joints, {} draw objects, {} materials, and {} direct textures (M={:#x} C={:#x} L={:#x} F={:#x}); entering bootstrap loop",
        meleeboard::disc::mounted_path(), host_scene.model_roots().size(),
        host_scene.joints().size(), host_scene.draw_objects().size(),
        host_scene.materials().size(), host_scene.textures().size(),
        stand_scene->models, stand_scene->cameras, stand_scene->lights,
        stand_scene->fogs);
    meleeboard::hsd::MeleeSceneRenderer scene_renderer(host_scene);
    MeleeBootstrapLog.info(
        "standScene renderer: {} drawable objects, {} skipped objects, {} triangles",
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
            simulation_accumulator -= kSimulationStep;
            ++steps;
        }
        if (steps == kMaxCatchUpSteps && simulation_accumulator >= kSimulationStep) {
            // Drop stale time rather than running a simulation catch-up loop
            // disconnected from current controller state.
            simulation_accumulator = std::chrono::duration<double>::zero();
        }
        scene_renderer.render();
        aurora_end_frame();
    }

    meleeboard::hsd::shutdown_host_runtime();
    meleeboard::disc::unmount();
    return 0;
}

#endif
