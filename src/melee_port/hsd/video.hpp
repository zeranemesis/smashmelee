#pragma once

#include <cstdint>

namespace meleeboard::hsd {

bool initialize_video();
void begin_video_frame();
void shutdown_video();
uint64_t video_frame_count();

} // namespace meleeboard::hsd
