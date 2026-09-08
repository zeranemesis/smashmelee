#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace meleeboard::hsd {

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

private:
    uint32_t public_count_ = 0;
    uint32_t data_size_ = 0;
    size_t public_table_offset_ = 0;
    size_t symbols_offset_ = 0;
    std::vector<unsigned char> bytes_;
};

} // namespace meleeboard::hsd
