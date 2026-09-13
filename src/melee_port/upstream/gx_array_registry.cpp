#include "gx_array_registry.hpp"

#include <vector>

#include "archive_convert.hpp"

namespace meleeboard::hsd {
namespace {

struct Entry {
    const void* base = nullptr;
    uint32_t extent = 0;
    bool little_endian = false;
};

// Small and linear on purpose.  A loaded archive has a handful of vertex
// arrays, GXSetArray is called once per array per draw, and a vector that
// stays in cache beats a hash table at these sizes.
std::vector<Entry>& entries()
{
    static std::vector<Entry> storage;
    return storage;
}

const Entry* find(const void* base)
{
    for (const Entry& entry : entries()) {
        if (entry.base == base) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

void register_vertex_array(const void* base, uint32_t extent,
                           bool little_endian)
{
    if (base == nullptr) {
        return;
    }
    for (Entry& entry : entries()) {
        if (entry.base == base) {
            entry.extent = extent;
            entry.little_endian = little_endian;
            return;
        }
    }
    entries().push_back(Entry{ base, extent, little_endian });
}

void register_vertex_arrays(const ArchiveConverter& converter)
{
    for (const ArchiveConverter::VertexArray& array :
         converter.vertex_arrays()) {
        // Straight out of the archive, so still in the console's byte order.
        register_vertex_array(array.base, array.extent,
                              /*little_endian=*/false);
    }
}

void forget_vertex_arrays() { entries().clear(); }

} // namespace meleeboard::hsd

// The two entry points include/melee/port/dolphin_compat.h routes upstream's
// GXSetArray calls through.  An array nobody registered answers zero, which
// is what the console's GX was told and what a backend that has not been
// given a length has to fall back on -- and it is visible in a recorded
// trace, which is how it was meant to be noticed.
extern "C" u32 melee_gx_array_extent(const void* base)
{
    const auto* entry = meleeboard::hsd::find(base);
    return entry != nullptr ? entry->extent : 0;
}

extern "C" bool melee_gx_array_is_little_endian(const void* base)
{
    const auto* entry = meleeboard::hsd::find(base);
    return entry != nullptr && entry->little_endian;
}
