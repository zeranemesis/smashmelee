#ifdef MELEE_BOOTSTRAP

#include "hsd/host_runtime.hpp"
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
        "Mounted {}; materialized standScene with {} models, {} joints, and {} draw objects (M={:#x} C={:#x} L={:#x} F={:#x}); entering bootstrap loop",
        meleeboard::disc::mounted_path(), host_scene.model_roots().size(),
        host_scene.joints().size(), host_scene.draw_objects().size(),
        stand_scene->models, stand_scene->cameras, stand_scene->lights,
        stand_scene->fogs);
    meleeboard::hsd::MeleeSceneRenderer scene_renderer(host_scene);
    MeleeBootstrapLog.info(
        "standScene renderer: {} drawable objects, {} skipped objects, {} triangles",
        scene_renderer.drawable_object_count(), scene_renderer.skipped_object_count(),
        scene_renderer.submitted_triangle_count());

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
        meleeboard::hsd::tick_host_runtime();
        scene_renderer.render();
        aurora_end_frame();
    }

    meleeboard::hsd::shutdown_host_runtime();
    meleeboard::disc::unmount();
    return 0;
}

#endif
