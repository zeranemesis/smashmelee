#include <melee/sysdolphin/baselib/archive.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace meleeboard::hsd {

namespace {

constexpr size_t kHeaderSize = 0x20;
constexpr size_t kRelocationEntrySize = 0x04;
constexpr size_t kSymbolEntrySize = 0x08;
constexpr size_t kDynamicModelDescSize = 0x10;
constexpr size_t kJointDescSize = 0x40;
constexpr uint32_t kMaxSceneModels = 256;
constexpr uint32_t kMaxSceneJoints = 8192;

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
    relocation_fields_.clear();
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
    const size_t relocation_table_offset = offset;
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

    std::unordered_set<uint32_t> relocation_fields;
    relocation_fields.reserve(relocation_count);
    for (uint32_t index = 0; index < relocation_count; ++index) {
        const size_t record = relocation_table_offset +
            static_cast<size_t>(index) * kRelocationEntrySize;
        const uint32_t field_offset = read_be_u32(bytes.data() + record);
        if (data_size < sizeof(uint32_t) ||
            field_offset > data_size - sizeof(uint32_t) ||
            !relocation_fields.insert(field_offset).second) {
            return false;
        }
    }

    bytes_ = std::move(bytes);
    public_count_ = public_count;
    data_size_ = data_size;
    public_table_offset_ = public_table_offset;
    symbols_offset_ = offset;
    relocation_fields_ = std::move(relocation_fields);
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
    return public_symbol_offset(symbol).has_value();
}

std::vector<std::string> Archive::public_symbols() const
{
    std::vector<std::string> result;
    if (!is_valid()) {
        return result;
    }
    result.reserve(public_count_);
    for (uint32_t index = 0; index < public_count_; ++index) {
        const size_t record = public_table_offset_ +
            static_cast<size_t>(index) * kSymbolEntrySize;
        const uint32_t symbol_offset = read_be_u32(bytes_.data() + record + 4);
        result.emplace_back(reinterpret_cast<const char*>(
            bytes_.data() + symbols_offset_ + symbol_offset));
    }
    return result;
}

std::optional<uint32_t> Archive::public_symbol_offset(
    std::string_view symbol) const
{
    if (!is_valid()) {
        return std::nullopt;
    }

    for (uint32_t index = 0; index < public_count_; ++index) {
        const size_t record = public_table_offset_ +
            static_cast<size_t>(index) * kSymbolEntrySize;
        const uint32_t data_offset = read_be_u32(bytes_.data() + record);
        const uint32_t symbol_offset = read_be_u32(bytes_.data() + record + 4);
        const char* name = reinterpret_cast<const char*>(
            bytes_.data() + symbols_offset_ + symbol_offset);
        if (std::string_view(name) == symbol) {
            return data_offset;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> Archive::data_word(uint32_t data_offset) const
{
    if (!contains_data_range(data_offset, sizeof(uint32_t))) {
        return std::nullopt;
    }
    return read_be_u32(bytes_.data() + kHeaderSize + data_offset);
}

std::optional<uint32_t> Archive::data_pointer(uint32_t field_offset) const
{
    const auto pointer = data_word(field_offset);
    if (!pointer.has_value()) {
        return std::nullopt;
    }
    if (relocation_fields_.contains(field_offset)) {
        return pointer;
    }
    if (*pointer == 0) {
        return uint32_t{ 0 };
    }
    return std::nullopt;
}

bool Archive::contains_data_range(uint32_t data_offset, uint32_t byte_count) const
{
    return is_valid() && data_offset <= data_size_ &&
        byte_count <= data_size_ - data_offset;
}

std::optional<uint8_t> Archive::data_byte(uint32_t data_offset) const
{
    if (!contains_data_range(data_offset, 1)) {
        return std::nullopt;
    }
    return bytes_[kHeaderSize + data_offset];
}

std::optional<float> Archive::data_float(uint32_t data_offset) const
{
    const auto bits = data_word(data_offset);
    if (!bits.has_value()) {
        return std::nullopt;
    }
    float value = 0;
    static_assert(sizeof(value) == sizeof(*bits));
    std::memcpy(&value, &*bits, sizeof(value));
    return value;
}

std::optional<SceneRoots> Archive::scene_roots(std::string_view symbol) const
{
    const auto root = public_symbol_offset(symbol);
    if (!root.has_value() || data_size_ < 4 * sizeof(uint32_t) ||
        *root > data_size_ - 4 * sizeof(uint32_t)) {
        return std::nullopt;
    }

    SceneRoots scene{};
    const auto models = data_pointer(*root);
    const auto cameras = data_pointer(*root + 4);
    const auto lights = data_pointer(*root + 8);
    const auto fogs = data_pointer(*root + 12);
    if (!models || !cameras || !lights || !fogs) {
        return std::nullopt;
    }
    scene.models = *models;
    scene.cameras = *cameras;
    scene.lights = *lights;
    scene.fogs = *fogs;

    const auto valid_data_pointer = [this](uint32_t pointer) {
        return pointer == 0 || pointer < data_size_;
    };
    if (!valid_data_pointer(scene.models) || !valid_data_pointer(scene.cameras) ||
        !valid_data_pointer(scene.lights) || !valid_data_pointer(scene.fogs)) {
        return std::nullopt;
    }
    return scene;
}

std::optional<uint32_t> Archive::scene_model_count(
    std::string_view symbol) const
{
    const auto scene = scene_roots(symbol);
    if (!scene.has_value()) {
        return std::nullopt;
    }
    if (scene->models == 0) {
        return 0;
    }

    for (uint32_t index = 0; index < kMaxSceneModels; ++index) {
        if (data_size_ < sizeof(uint32_t) ||
            scene->models > data_size_ - sizeof(uint32_t) ||
            index > (data_size_ - sizeof(uint32_t) - scene->models) /
                    sizeof(uint32_t)) {
            return std::nullopt;
        }
        const uint32_t array_offset = scene->models +
            index * sizeof(uint32_t);
        const auto model = data_pointer(array_offset);
        if (!model.has_value()) {
            return std::nullopt;
        }
        if (*model == 0) {
            return index;
        }
        if (data_size_ < kDynamicModelDescSize ||
            *model > data_size_ - kDynamicModelDescSize) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> Archive::scene_joint_count(std::string_view symbol) const
{
    const auto scene = scene_roots(symbol);
    const auto model_count = scene_model_count(symbol);
    if (!scene.has_value() || !model_count.has_value()) {
        return std::nullopt;
    }

    std::vector<uint32_t> pending;
    for (uint32_t index = 0; index < *model_count; ++index) {
        const auto model = data_pointer(scene->models + index * sizeof(uint32_t));
        if (!model.has_value() || data_size_ < kDynamicModelDescSize ||
            *model > data_size_ - kDynamicModelDescSize) {
            return std::nullopt;
        }
        const auto joint = data_pointer(*model);
        if (!joint.has_value()) {
            return std::nullopt;
        }
        if (*joint != 0) {
            pending.push_back(*joint);
        }
    }

    std::unordered_set<uint32_t> visited;
    while (!pending.empty()) {
        const uint32_t joint = pending.back();
        pending.pop_back();
        if (!visited.insert(joint).second) {
            continue;
        }
        if (visited.size() > kMaxSceneJoints || data_size_ < kJointDescSize ||
            joint > data_size_ - kJointDescSize) {
            return std::nullopt;
        }

        const auto child = data_pointer(joint + 0x08);
        const auto next = data_pointer(joint + 0x0C);
        if (!child.has_value() || !next.has_value()) {
            return std::nullopt;
        }
        if (*child != 0) {
            pending.push_back(*child);
        }
        if (*next != 0) {
            pending.push_back(*next);
        }
    }
    return static_cast<uint32_t>(visited.size());
}

std::optional<uint32_t> Archive::joint_tree_count(std::string_view symbol) const
{
    const auto root = public_symbol_offset(symbol);
    if (!root.has_value()) {
        return std::nullopt;
    }
    std::vector<uint32_t> pending{ *root };
    std::unordered_set<uint32_t> visited;
    while (!pending.empty()) {
        const uint32_t joint = pending.back();
        pending.pop_back();
        if (!visited.insert(joint).second) {
            continue;
        }
        if (visited.size() > kMaxSceneJoints || data_size_ < kJointDescSize ||
            joint > data_size_ - kJointDescSize) {
            return std::nullopt;
        }
        const auto child = data_pointer(joint + 0x08);
        const auto sibling = data_pointer(joint + 0x0C);
        if (!child.has_value() || !sibling.has_value()) {
            return std::nullopt;
        }
        if (*child != 0) pending.push_back(*child);
        if (*sibling != 0) pending.push_back(*sibling);
    }
    return static_cast<uint32_t>(visited.size());
}

} // namespace meleeboard::hsd
