#include "dat_builder.hpp"

#include <cstring>
#include <stdexcept>

namespace meleeboard::test {

namespace {

constexpr uint32_t kHeaderSize = 0x20;

void write_be_u32(std::vector<unsigned char>& bytes, size_t offset,
                  uint32_t value)
{
    bytes[offset] = static_cast<unsigned char>(value >> 24U);
    bytes[offset + 1] = static_cast<unsigned char>(value >> 16U);
    bytes[offset + 2] = static_cast<unsigned char>(value >> 8U);
    bytes[offset + 3] = static_cast<unsigned char>(value);
}

} // namespace

uint32_t DatBuilder::allocate(uint32_t size, uint32_t alignment)
{
    if (alignment == 0) {
        throw std::invalid_argument("DatBuilder::allocate: zero alignment");
    }
    const uint32_t padding = (alignment - (data_.size() % alignment)) % alignment;
    data_.resize(data_.size() + padding);
    const auto offset = static_cast<uint32_t>(data_.size());
    data_.resize(data_.size() + size);
    return offset;
}

void DatBuilder::u8(uint32_t offset, uint8_t value)
{
    if (offset >= data_.size()) {
        throw std::out_of_range("DatBuilder::u8: offset outside data section");
    }
    data_[offset] = value;
}

void DatBuilder::u16(uint32_t offset, uint16_t value)
{
    u8(offset, static_cast<uint8_t>(value >> 8U));
    u8(offset + 1, static_cast<uint8_t>(value));
}

void DatBuilder::u32(uint32_t offset, uint32_t value)
{
    u8(offset, static_cast<uint8_t>(value >> 24U));
    u8(offset + 1, static_cast<uint8_t>(value >> 16U));
    u8(offset + 2, static_cast<uint8_t>(value >> 8U));
    u8(offset + 3, static_cast<uint8_t>(value));
}

void DatBuilder::s16(uint32_t offset, int16_t value)
{
    u16(offset, static_cast<uint16_t>(value));
}

void DatBuilder::f32(uint32_t offset, float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    u32(offset, bits);
}

void DatBuilder::blob(uint32_t offset, const std::vector<uint8_t>& bytes)
{
    for (size_t index = 0; index < bytes.size(); ++index) {
        u8(offset + static_cast<uint32_t>(index), bytes[index]);
    }
}

void DatBuilder::pointer(uint32_t field, uint32_t target)
{
    u32(field, target);
    for (uint32_t existing : relocations_) {
        if (existing == field) {
            // The loader rejects a duplicated relocation record, so a test
            // that writes one twice is a bug in the fixture, not a case.
            throw std::invalid_argument(
                "DatBuilder::pointer: field already relocated");
        }
    }
    relocations_.push_back(field);
}

void DatBuilder::symbol(std::string_view name, uint32_t data_offset)
{
    symbols_.push_back({ std::string(name), data_offset });
}

uint32_t DatBuilder::data_size() const
{
    return static_cast<uint32_t>(data_.size());
}

std::vector<unsigned char> DatBuilder::build() const
{
    std::vector<unsigned char> strings;
    std::vector<uint32_t> string_offsets;
    string_offsets.reserve(symbols_.size());
    for (const Symbol& symbol : symbols_) {
        string_offsets.push_back(static_cast<uint32_t>(strings.size()));
        strings.insert(strings.end(), symbol.name.begin(), symbol.name.end());
        strings.push_back(0);
    }

    const auto data_bytes = static_cast<uint32_t>(data_.size());
    const auto relocation_bytes =
        static_cast<uint32_t>(relocations_.size() * 4);
    const auto public_bytes = static_cast<uint32_t>(symbols_.size() * 8);
    const size_t relocation_offset = kHeaderSize + data_bytes;
    const size_t public_offset = relocation_offset + relocation_bytes;
    const size_t strings_offset = public_offset + public_bytes;

    std::vector<unsigned char> bytes(strings_offset + strings.size());
    write_be_u32(bytes, 0x00, static_cast<uint32_t>(bytes.size()));
    write_be_u32(bytes, 0x04, data_bytes);
    write_be_u32(bytes, 0x08, static_cast<uint32_t>(relocations_.size()));
    write_be_u32(bytes, 0x0C, static_cast<uint32_t>(symbols_.size()));
    write_be_u32(bytes, 0x10, 0); // extern symbol count
    std::memcpy(bytes.data() + kHeaderSize, data_.data(), data_.size());
    for (size_t index = 0; index < relocations_.size(); ++index) {
        write_be_u32(bytes, relocation_offset + index * 4, relocations_[index]);
    }
    for (size_t index = 0; index < symbols_.size(); ++index) {
        write_be_u32(bytes, public_offset + index * 8,
                     symbols_[index].data_offset);
        write_be_u32(bytes, public_offset + index * 8 + 4,
                     string_offsets[index]);
    }
    if (!strings.empty()) {
        std::memcpy(bytes.data() + strings_offset, strings.data(),
                    strings.size());
    }
    return bytes;
}

} // namespace meleeboard::test
