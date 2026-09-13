#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace meleeboard::hsd {

// Data-section offsets of a SceneDesc's four root arrays.  An absent array
// reads back as Archive::kNullOffset, never as offset zero.
struct SceneRoots {
    uint32_t models = UINT32_MAX;
    uint32_t cameras = UINT32_MAX;
    uint32_t lights = UINT32_MAX;
    uint32_t fogs = UINT32_MAX;
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
    std::vector<std::string> public_symbols() const;

    // Data offsets deliberately remain GameCube 32-bit offsets.  Native code
    // must resolve them through a host address model instead of casting them.
    std::optional<uint32_t> public_symbol_offset(std::string_view symbol) const;
    bool contains_data_range(uint32_t data_offset, uint32_t byte_count) const;
    std::optional<uint8_t> data_byte(uint32_t data_offset) const;
    std::optional<uint32_t> data_word(uint32_t data_offset) const;
    // The offset data_pointer() reports for a NULL field.  HSD stores NULL as
    // a zero word that the loader leaves out of the relocation table, while a
    // relocated zero addresses the first byte of the data section.  A host
    // that keeps offsets rather than addresses must not conflate the two, so
    // NULL gets a value no data section can contain.
    static constexpr uint32_t kNullOffset = UINT32_MAX;

    // Reads an on-disc pointer field as an offset in the HSD data section.
    // A zero offset is valid when the field is listed in the relocation table:
    // the original HSD loader adds the data-section base to every relocation
    // field, including one whose stored value is zero.  A zero word outside
    // the relocation table is a null pointer and reads back as kNullOffset.
    // Non-null unrelocated values are not safe host data pointers and are
    // rejected.
    std::optional<uint32_t> data_pointer(uint32_t field_offset) const;
    std::optional<float> data_float(uint32_t data_offset) const;

    // The bytes at `data_offset`, addressed directly, and how many of them
    // remain before the data section ends.
    //
    // This is the one place the archive hands out an address, and it is safe
    // for exactly one thing: raw data that is not a structure -- a vertex
    // array, a display list, a texture image, a string.  Those have no
    // pointers in them, so their size and meaning do not change on a 64-bit
    // host, and the GPU can read them where they lie.  Anything with a
    // pointer field has to be converted instead; see
    // src/melee_port/upstream/archive_convert.hpp.
    //
    // The bytes stay in the console's byte order.  A caller that hands them
    // to the host's GX has to say so -- which is what Aurora's GXSetArray
    // takes a little-endian flag for.
    struct DataSpan {
        const unsigned char* bytes = nullptr;
        uint32_t remaining = 0;
    };
    std::optional<DataSpan> data_span(uint32_t data_offset) const;
    std::optional<SceneRoots> scene_roots(std::string_view symbol) const;
    std::optional<uint32_t> scene_model_count(std::string_view symbol) const;
    std::optional<uint32_t> scene_joint_count(std::string_view symbol) const;
    std::optional<uint32_t> joint_tree_count(std::string_view symbol) const;

private:
    uint32_t public_count_ = 0;
    uint32_t data_size_ = 0;
    size_t public_table_offset_ = 0;
    size_t symbols_offset_ = 0;
    std::unordered_set<uint32_t> relocation_fields_;
    std::vector<unsigned char> bytes_;
};

} // namespace meleeboard::hsd
