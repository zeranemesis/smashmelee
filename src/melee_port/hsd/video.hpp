#pragma once

#include <cstdint>
#include <dolphin/gx.h>

namespace meleeboard::hsd {

bool initialize_video();
void begin_video_frame();
void shutdown_video();
uint64_t video_frame_count();
// The logical GameCube mode used to translate HSD CObj viewport/scissor
// coordinates to Aurora's GX surface.  Aurora owns the actual swapchain.
const GXRenderModeObj& logical_render_mode();

} // namespace meleeboard::hsd
