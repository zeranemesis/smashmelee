#pragma once

#include <cstdint>

namespace meleeboard::hsd {

bool initialize_host_runtime();
void tick_host_runtime();
void shutdown_host_runtime();
uint64_t rendered_frame_count();

} // namespace meleeboard::hsd
