#include "video.hpp"

#include <dolphin/gx.h>

namespace meleeboard::hsd {

namespace {

GXRenderModeObj gRenderMode{};
bool gVideoInitialized = false;
uint64_t gRenderedFrames = 0;

} // namespace

bool initialize_video()
{
    // HSD's default Melee path starts from the NTSC double-field render mode.
    // Aurora owns the host swapchain, so this describes the logical GX surface
    // only; it must not allocate or copy a GameCube XFB on the PC host.
    gRenderMode = GXNtsc480IntDf;
    if (gRenderMode.fbWidth == 0 || gRenderMode.efbHeight == 0 ||
        gRenderMode.xfbHeight == 0) {
        return false;
    }
    gRenderedFrames = 0;
    gVideoInitialized = true;
    return true;
}

void begin_video_frame()
{
    if (!gVideoInitialized) {
        return;
    }

    GXSetViewport(0.0F, 0.0F, static_cast<f32>(gRenderMode.fbWidth),
                  static_cast<f32>(gRenderMode.efbHeight), 0.0F, 1.0F);
    GXSetScissor(0, 0, gRenderMode.fbWidth, gRenderMode.efbHeight);
    GXSetCullMode(GX_CULL_BACK);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    ++gRenderedFrames;
}

void shutdown_video()
{
    gVideoInitialized = false;
    gRenderedFrames = 0;
}

uint64_t video_frame_count()
{
    return gRenderedFrames;
}

const GXRenderModeObj& logical_render_mode()
{
    return gRenderMode;
}

} // namespace meleeboard::hsd
