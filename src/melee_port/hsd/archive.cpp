#include <melee/sysdolphin/baselib/archive.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace meleeboard::hsd {

namespace {

constexpr size_t kHeaderSize = 0x20;
constexpr size_t kRelocationEntrySize = 0x04;
constexpr size_t kSymbolEntrySize = 0x08;

uint32_t read_be_u32(const unsigned char* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
}

bool advance(size_t& offset, uint32_t count, size_t entry_size, size_t limit)
{
    if (count > (std::numeric_limits<size_t>::max() - offset) / entry_size) {
        return false;
    }
    const size_t bytes = static_cast<size_t>(count) * entry_size;
    if (bytes > limit - offset) {
        return false;
    }
    offset += bytes;
    return true;
}

} // namespace

bool Archive::parse(std::vector<unsigned char> bytes)
{
    public_count_ = 0;
    data_size_ = 0;
    public_table_offset_ = 0;
    symbols_offset_ = 0;
    bytes_.clear();

    if (bytes.size() < kHeaderSize) {
        return false;
    }

    const uint32_t file_size = read_be_u32(bytes.data());
    const uint32_t data_size = read_be_u32(bytes.data() + 0x04);
    const uint32_t relocation_count = read_be_u32(bytes.data() + 0x08);
    const uint32_t public_count = read_be_u32(bytes.data() + 0x0C);
    const uint32_t extern_count = read_be_u32(bytes.data() + 0x10);
    if (file_size != bytes.size() || data_size > bytes.size() - kHeaderSize) {
        return false;
    }

    size_t offset = kHeaderSize + data_size;
    if (!advance(offset, relocation_count, kRelocationEntrySize, bytes.size())) {
        return false;
    }
    const size_t public_table_offset = offset;
    if (!advance(offset, public_count, kSymbolEntrySize, bytes.size()) ||
        !advance(offset, extern_count, kSymbolEntrySize, bytes.size())) {
        return false;
    }

    // Validate the public records before accepting the archive.  In particular,
    // each string must remain inside the symbol table and be NUL terminated.
    for (uint32_t index = 0; index < public_count; ++index) {
        const size_t record = public_table_offset +
            static_cast<size_t>(index) * kSymbolEntrySize;
        const uint32_t data_offset = read_be_u32(bytes.data() + record);
        const uint32_t symbol_offset = read_be_u32(bytes.data() + record + 4);
        if (data_offset >= data_size || symbol_offset > bytes.size() - offset) {
            return false;
        }
        const auto symbol_begin = bytes.begin() + offset + symbol_offset;
        if (std::find(symbol_begin, bytes.end(), 0) == bytes.end()) {
            return false;
        }
    }

    bytes_ = std::move(bytes);
    public_count_ = public_count;
    data_size_ = data_size;
    public_table_offset_ = public_table_offset;
    symbols_offset_ = offset;
    return true;
}

bool Archive::is_valid() const
{
    return !bytes_.empty();
}

size_t Archive::file_size() const
{
    return bytes_.size();
}

uint32_t Archive::data_size() const
{
    return data_size_;
}

uint32_t Archive::public_symbol_count() const
{
    return public_count_;
}

bool Archive::has_public_symbol(std::string_view symbol) const
{
    if (!is_valid()) {
        return false;
    }

    for (uint32_t index = 0; index < public_count_; ++index) {
        const size_t record = public_table_offset_ +
            static_cast<size_t>(index) * kSymbolEntrySize;
        const uint32_t symbol_offset = read_be_u32(bytes_.data() + record + 4);
        const char* name = reinterpret_cast<const char*>(
            bytes_.data() + symbols_offset_ + symbol_offset);
        if (std::string_view(name) == symbol) {
            return true;
        }
    }
    return false;
}

} // namespace meleeboard::hsd
