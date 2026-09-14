#pragma once

// Builds synthetic HSD DAT containers in the on-disc GameCube layout
// (big-endian, 32-bit offsets, explicit relocation table).  Tests use it to
// exercise the archive/scene/animation decoders without shipping or requiring
// any Nintendo asset.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace meleeboard::test {

class DatBuilder {
public:
    // Reserves zero-filled space in the data section and returns its offset.
    uint32_t allocate(uint32_t size, uint32_t alignment = 4);

    void u8(uint32_t offset, uint8_t value);
    void u16(uint32_t offset, uint16_t value);
    void u32(uint32_t offset, uint32_t value);
    void s16(uint32_t offset, int16_t value);
    void f32(uint32_t offset, float value);
    void blob(uint32_t offset, const std::vector<uint8_t>& bytes);

    // Writes a GameCube pointer field and records it in the relocation table,
    // which is what makes a stored zero a valid data-section offset rather
    // than a null pointer.
    void pointer(uint32_t field, uint32_t target);

    // Exports `data_offset` under `name` in the public symbol table.
    void symbol(std::string_view name, uint32_t data_offset);

    uint32_t data_size() const;

    std::vector<unsigned char> build() const;

private:
    struct Symbol {
        std::string name;
        uint32_t data_offset = 0;
    };

    std::vector<uint8_t> data_;
    std::vector<uint32_t> relocations_;
    std::vector<Symbol> symbols_;
};

} // namespace meleeboard::test
