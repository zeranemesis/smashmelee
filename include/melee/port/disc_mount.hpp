#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace meleeboard::disc {

// Opens a supported GALE01 v1.02 image and its GameCube data partition.
bool mount(std::string_view image_path);
void unmount();

bool is_mounted();
std::string mounted_path();

// Paths are relative to the GameCube disc root, for example "opening.bnr".
bool has_file(std::string_view path);
bool read_file(std::string_view path, std::vector<unsigned char>& out);

} // namespace meleeboard::disc
