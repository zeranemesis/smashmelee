#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace meleeboard::hsd {

struct SceneRoots {
    uint32_t models = 0;
    uint32_t cameras = 0;
    uint32_t lights = 0;
    uint32_t fogs = 0;
};

// A read-only view of Melee's big-endian HSD DAT container.  Relocation is
// deliberately separate: GameCube pointers are 32-bit and need a native host
// address model before they can be made usable on a 64-bit process.
class Archive {
public:
    bool parse(std::vector<unsigned char> bytes);

    bool is_valid() const;
    size_t file_size() const;
    uint32_t data_size() const;
    uint32_t public_symbol_count() const;
    bool has_public_symbol(std::string_view symbol) const;

    // Data offsets deliberately remain GameCube 32-bit offsets.  Native code
    // must resolve them through a host address model instead of casting them.
    std::optional<uint32_t> public_symbol_offset(std::string_view symbol) const;
    bool contains_data_range(uint32_t data_offset, uint32_t byte_count) const;
    std::optional<uint32_t> data_word(uint32_t data_offset) const;
    std::optional<float> data_float(uint32_t data_offset) const;
    std::optional<SceneRoots> scene_roots(std::string_view symbol) const;
    std::optional<uint32_t> scene_model_count(std::string_view symbol) const;
    std::optional<uint32_t> scene_joint_count(std::string_view symbol) const;

private:
    uint32_t public_count_ = 0;
    uint32_t data_size_ = 0;
    size_t public_table_offset_ = 0;
    size_t symbols_offset_ = 0;
    std::vector<unsigned char> bytes_;
};

} // namespace meleeboard::hsd
