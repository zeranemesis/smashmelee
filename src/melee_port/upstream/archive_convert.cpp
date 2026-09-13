#include "archive_convert.hpp"

#include <cstring>
#include <utility>

// Why there is no HSD_Joint32b here.
//
// docs/PLAN.md expected this to follow the Mario Party 4 precedent in
// src/port/byteswap.cpp: a `<Name>32b` struct laid over the archive's bytes
// with big-endian fields, and a byteswap_<name>() that copies it into the
// host structure.  For Melee's containers that shape is not merely awkward,
// it is unable to express the data.
//
// An HSD pointer field is not self-describing.  The console's loader adds the
// data section's base to every field named in the archive's *relocation
// table*, and leaves every other field alone -- so a pointer field holding
// zero is the first byte of the data section when the table names it, and is
// NULL when the table does not.  The two cases are identical in the struct
// and differ only in a table stored elsewhere in the file.  A wire struct
// cannot tell them apart, which is exactly the defect this port already had
// and fixed once (see Archive::kNullOffset).
//
// So the conversion reads through Archive, which carries the relocation table
// and answers kNullOffset for a field the table does not name.  The bounds
// checks come along for free.

namespace meleeboard::hsd {
namespace {

// Field offsets within an on-disc HSD_Joint.  Upstream's own comments in
// sysdolphin/baselib/jobj.h are the source; the structure is 0x40 bytes on
// the console and larger here, which is the whole reason for this file.
constexpr uint32_t kJointClassName = 0x00;
constexpr uint32_t kJointFlags = 0x04;
constexpr uint32_t kJointChild = 0x08;
constexpr uint32_t kJointNext = 0x0C;
constexpr uint32_t kJointUnion = 0x10;
constexpr uint32_t kJointRotation = 0x14;
constexpr uint32_t kJointScale = 0x20;
constexpr uint32_t kJointPosition = 0x2C;
constexpr uint32_t kJointMatrix = 0x38;
constexpr uint32_t kJointRObjDesc = 0x3C;
constexpr uint32_t kJointSize = 0x40;

// What the union at +0x10 holds is decided by two of the joint's flags.
bool union_is_display_object(uint32_t flags)
{
    return (flags & (JOBJ_PTCL | JOBJ_SPLINE)) == 0;
}

const char* union_kind(uint32_t flags)
{
    if ((flags & JOBJ_PTCL) != 0) {
        return "HSD_SList (particle)";
    }
    if ((flags & JOBJ_SPLINE) != 0) {
        return "HSD_Spline";
    }
    return "HSD_DObjDesc";
}

// HSD_DObjDesc, 0x10 bytes on the console.
constexpr uint32_t kDObjClassName = 0x00;
constexpr uint32_t kDObjNext = 0x04;
constexpr uint32_t kDObjMObjDesc = 0x08;
constexpr uint32_t kDObjPObjDesc = 0x0C;
constexpr uint32_t kDObjSize = 0x10;

// HSD_MObjDesc, 0x18 bytes.
constexpr uint32_t kMObjClassName = 0x00;
constexpr uint32_t kMObjRenderMode = 0x04;
constexpr uint32_t kMObjTexDesc = 0x08;
constexpr uint32_t kMObjMaterial = 0x0C;
constexpr uint32_t kMObjRenderDesc = 0x10;
constexpr uint32_t kMObjPEDesc = 0x14;
constexpr uint32_t kMObjSize = 0x18;

// HSD_Material, 0x14 bytes: three GXColors then two floats.
constexpr uint32_t kMaterialAmbient = 0x00;
constexpr uint32_t kMaterialDiffuse = 0x04;
constexpr uint32_t kMaterialSpecular = 0x08;
constexpr uint32_t kMaterialAlpha = 0x0C;
constexpr uint32_t kMaterialShininess = 0x10;
constexpr uint32_t kMaterialSize = 0x14;

// HSD_PObjDesc, 0x18 bytes.  flags and n_display are two u16 in one word.
constexpr uint32_t kPObjClassName = 0x00;
constexpr uint32_t kPObjNext = 0x04;
constexpr uint32_t kPObjVerts = 0x08;
constexpr uint32_t kPObjFlags = 0x0C;
constexpr uint32_t kPObjDisplayCount = 0x0E;
constexpr uint32_t kPObjDisplay = 0x10;
constexpr uint32_t kPObjUnion = 0x14;
constexpr uint32_t kPObjSize = 0x18;

// HSD_VtxDescList, 0x18 bytes: four enum words, a fraction byte, a pad byte,
// a stride, and the array's address.  The list ends with a GX_VA_NULL entry.
constexpr uint32_t kVtxAttr = 0x00;
constexpr uint32_t kVtxAttrType = 0x04;
constexpr uint32_t kVtxCompCnt = 0x08;
constexpr uint32_t kVtxCompType = 0x0C;
constexpr uint32_t kVtxFrac = 0x10;
constexpr uint32_t kVtxStride = 0x12;
constexpr uint32_t kVtxVertex = 0x14;
constexpr uint32_t kVtxSize = 0x18;

// A vertex descriptor list of more entries than this is a corrupt archive
// rather than a large mesh: HSD has nine vertex attributes plus eight
// texture coordinates.
constexpr uint32_t kMaxVertexDescriptors = 64;

// The display list HSD hands to GXCallDisplayList is stored in 32-byte units.
constexpr uint32_t kDisplayListGranularity = 32;

} // namespace

bool ArchiveConverter::read_vector(uint32_t offset, Vec3& out)
{
    const auto x = archive_.data_float(offset + 0);
    const auto y = archive_.data_float(offset + 4);
    const auto z = archive_.data_float(offset + 8);
    if (!x.has_value() || !y.has_value() || !z.has_value()) {
        return false;
    }
    out.x = *x;
    out.y = *y;
    out.z = *z;
    return true;
}

bool ArchiveConverter::read_matrix(uint32_t offset, MtxPtr& out)
{
    Mtx matrix;
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            const auto value =
                archive_.data_float(offset + (row * 4 + column) * 4);
            if (!value.has_value()) {
                return false;
            }
            matrix[row][column] = *value;
        }
    }
    matrices_.emplace_back();
    std::memcpy(matrices_.back().value, matrix, sizeof matrix);
    out = matrices_.back().value;
    return true;
}

char* ArchiveConverter::read_string(uint32_t offset)
{
    std::string text;
    for (uint32_t index = 0;; ++index) {
        const auto byte = archive_.data_byte(offset + index);
        if (!byte.has_value()) {
            // An unterminated string runs off the data section.  Treat it as
            // absent rather than returning half of it.
            return nullptr;
        }
        if (*byte == 0) {
            break;
        }
        text.push_back(static_cast<char>(*byte));
    }
    strings_.push_back(std::move(text));
    return strings_.back().data();
}

const void* ArchiveConverter::raw_data(uint32_t offset, uint32_t* extent)
{
    const auto span = archive_.data_span(offset);
    if (!span.has_value()) {
        return nullptr;
    }
    if (extent != nullptr) {
        *extent = span->remaining;
    }
    return span->bytes;
}

HSD_Material* ArchiveConverter::convert_material(uint32_t offset)
{
    const auto seen = materials_by_offset_.find(offset);
    if (seen != materials_by_offset_.end()) {
        return seen->second;
    }
    if (!archive_.contains_data_range(offset, kMaterialSize)) {
        error_ = "material at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    // Three GXColors, each four bytes in the order r, g, b, a -- byte fields,
    // so they cross unchanged.
    const auto colour = [this, offset](uint32_t field, GXColor& out) {
        out.r = *archive_.data_byte(offset + field + 0);
        out.g = *archive_.data_byte(offset + field + 1);
        out.b = *archive_.data_byte(offset + field + 2);
        out.a = *archive_.data_byte(offset + field + 3);
    };

    materials_.emplace_back();
    HSD_Material& host = materials_.back();
    std::memset(&host, 0, sizeof host);
    colour(kMaterialAmbient, host.ambient);
    colour(kMaterialDiffuse, host.diffuse);
    colour(kMaterialSpecular, host.specular);
    host.alpha = *archive_.data_float(offset + kMaterialAlpha);
    host.shininess = *archive_.data_float(offset + kMaterialShininess);

    materials_by_offset_.emplace(offset, &host);
    return &host;
}

HSD_VtxDescList* ArchiveConverter::convert_vertex_descriptors(uint32_t offset)
{
    // The list is an array terminated by a GX_VA_NULL attribute, not a linked
    // list, so it converts as a block.  Every entry grows here: four enums
    // that are int-sized either way, and a pointer that is not.
    std::vector<HSD_VtxDescList> entries;
    for (uint32_t index = 0; index <= kMaxVertexDescriptors; ++index) {
        const uint32_t entry = offset + index * kVtxSize;
        if (!archive_.contains_data_range(entry, kVtxSize)) {
            error_ = "vertex descriptor list at offset " +
                     std::to_string(offset) +
                     " runs off the end of the data section";
            return nullptr;
        }

        HSD_VtxDescList host{};
        std::memset(&host, 0, sizeof host);
        host.attr = static_cast<GXAttr>(*archive_.data_word(entry + kVtxAttr));
        if (host.attr == GX_VA_NULL) {
            entries.push_back(host);
            vertex_descriptors_.push_back(std::move(entries));
            return vertex_descriptors_.back().data();
        }

        host.attr_type =
            static_cast<GXAttrType>(*archive_.data_word(entry + kVtxAttrType));
        host.comp_cnt =
            static_cast<GXCompCnt>(*archive_.data_word(entry + kVtxCompCnt));
        host.comp_type =
            static_cast<GXCompType>(*archive_.data_word(entry + kVtxCompType));
        host.frac = *archive_.data_byte(entry + kVtxFrac);
        host.stride =
            static_cast<u16>((*archive_.data_byte(entry + kVtxStride) << 8) |
                             *archive_.data_byte(entry + kVtxStride + 1));

        const auto vertex = archive_.data_pointer(entry + kVtxVertex);
        if (vertex.has_value() && *vertex != Archive::kNullOffset) {
            uint32_t extent = 0;
            host.vertex = const_cast<void*>(raw_data(*vertex, &extent));
            if (host.vertex == nullptr) {
                error_ = "vertex array at offset " + std::to_string(*vertex) +
                         " lies outside the archive's data section";
                return nullptr;
            }
            vertex_arrays_.push_back(VertexArray{ host.vertex, extent });
        }
        entries.push_back(host);
    }

    error_ = "vertex descriptor list at offset " + std::to_string(offset) +
             " has no GX_VA_NULL terminator";
    return nullptr;
}

HSD_PObjDesc* ArchiveConverter::convert_primitive(uint32_t offset)
{
    const auto seen = primitives_by_offset_.find(offset);
    if (seen != primitives_by_offset_.end()) {
        return seen->second;
    }
    if (!archive_.contains_data_range(offset, kPObjSize)) {
        error_ = "primitive at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    primitives_.emplace_back();
    HSD_PObjDesc& host = primitives_.back();
    std::memset(&host, 0, sizeof host);
    primitives_by_offset_.emplace(offset, &host);

    host.flags = static_cast<u16>((*archive_.data_byte(offset + kPObjFlags)
                                   << 8) |
                                  *archive_.data_byte(offset + kPObjFlags + 1));
    host.n_display =
        static_cast<u16>((*archive_.data_byte(offset + kPObjDisplayCount) << 8) |
                         *archive_.data_byte(offset + kPObjDisplayCount + 1));

    const auto class_name = archive_.data_pointer(offset + kPObjClassName);
    if (class_name.has_value() && *class_name != Archive::kNullOffset) {
        host.class_name = read_string(*class_name);
    }

    const auto verts = archive_.data_pointer(offset + kPObjVerts);
    if (verts.has_value() && *verts != Archive::kNullOffset) {
        host.verts = convert_vertex_descriptors(*verts);
        if (host.verts == nullptr) {
            return nullptr;
        }
    }

    // The display list is GX commands -- bytes, with no pointers in them --
    // so it stays in the archive and is addressed where it lies.  n_display
    // counts 32-byte units, which is how HSD stores it.
    const auto display = archive_.data_pointer(offset + kPObjDisplay);
    if (display.has_value() && *display != Archive::kNullOffset) {
        const uint32_t length = host.n_display * kDisplayListGranularity;
        if (!archive_.contains_data_range(*display, length)) {
            error_ = "primitive at offset " + std::to_string(offset) +
                     " has a display list running off the data section";
            return nullptr;
        }
        host.display =
            const_cast<u8*>(static_cast<const u8*>(raw_data(*display, nullptr)));
    }

    // The union at +0x14 is a joint, a shape set or an envelope table
    // depending on the flags -- none of them converted yet.
    const auto union_field = archive_.data_pointer(offset + kPObjUnion);
    if (union_field.has_value() && *union_field != Archive::kNullOffset) {
        note_unconverted(offset, *union_field, "HSD_PObjDesc::u");
    }

    const auto next = archive_.data_pointer(offset + kPObjNext);
    if (next.has_value() && *next != Archive::kNullOffset) {
        HSD_PObjDesc* following = convert_primitive(*next);
        if (following == nullptr) {
            return nullptr;
        }
        host.next = following;
    }

    return &host;
}

HSD_MObjDesc* ArchiveConverter::convert_material_object(uint32_t offset)
{
    const auto seen = material_objects_by_offset_.find(offset);
    if (seen != material_objects_by_offset_.end()) {
        return seen->second;
    }
    if (!archive_.contains_data_range(offset, kMObjSize)) {
        error_ = "material object at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    material_objects_.emplace_back();
    HSD_MObjDesc& host = material_objects_.back();
    std::memset(&host, 0, sizeof host);
    material_objects_by_offset_.emplace(offset, &host);

    host.rendermode = *archive_.data_word(offset + kMObjRenderMode);

    const auto class_name = archive_.data_pointer(offset + kMObjClassName);
    if (class_name.has_value() && *class_name != Archive::kNullOffset) {
        host.class_name = read_string(*class_name);
    }

    const auto material = archive_.data_pointer(offset + kMObjMaterial);
    if (material.has_value() && *material != Archive::kNullOffset) {
        host.mat = convert_material(*material);
        if (host.mat == nullptr) {
            return nullptr;
        }
    }

    // Textures, the TEV render descriptor and the pixel-engine descriptor are
    // the next three converters.  A material that names one is still usable
    // without it -- it draws untextured -- so they are recorded, not refused.
    const auto texdesc = archive_.data_pointer(offset + kMObjTexDesc);
    if (texdesc.has_value() && *texdesc != Archive::kNullOffset) {
        note_unconverted(offset, *texdesc, "HSD_TObjDesc");
    }
    const auto renderdesc = archive_.data_pointer(offset + kMObjRenderDesc);
    if (renderdesc.has_value() && *renderdesc != Archive::kNullOffset) {
        note_unconverted(offset, *renderdesc, "HSD_RenderDesc");
    }
    const auto pedesc = archive_.data_pointer(offset + kMObjPEDesc);
    if (pedesc.has_value() && *pedesc != Archive::kNullOffset) {
        note_unconverted(offset, *pedesc, "HSD_PEDesc");
    }

    return &host;
}

HSD_DObjDesc* ArchiveConverter::convert_display_object(uint32_t offset)
{
    const auto seen = display_objects_by_offset_.find(offset);
    if (seen != display_objects_by_offset_.end()) {
        return seen->second;
    }
    if (!archive_.contains_data_range(offset, kDObjSize)) {
        error_ = "display object at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    // Registered before its fields are filled in, and the reference stays
    // valid however much the deques grow: a chain that points back at this
    // object has to find the address that was handed out, not a second copy.
    display_objects_.emplace_back();
    HSD_DObjDesc& host = display_objects_.back();
    std::memset(&host, 0, sizeof host);
    display_objects_by_offset_.emplace(offset, &host);

    const auto class_name = archive_.data_pointer(offset + kDObjClassName);
    if (class_name.has_value() && *class_name != Archive::kNullOffset) {
        host.class_name = read_string(*class_name);
    }

    const auto mobjdesc = archive_.data_pointer(offset + kDObjMObjDesc);
    if (mobjdesc.has_value() && *mobjdesc != Archive::kNullOffset) {
        HSD_MObjDesc* material = convert_material_object(*mobjdesc);
        if (material == nullptr) {
            return nullptr;
        }
        host.mobjdesc = material;
    }

    const auto pobjdesc = archive_.data_pointer(offset + kDObjPObjDesc);
    if (pobjdesc.has_value() && *pobjdesc != Archive::kNullOffset) {
        HSD_PObjDesc* primitive = convert_primitive(*pobjdesc);
        if (primitive == nullptr) {
            return nullptr;
        }
        host.pobjdesc = primitive;
    }

    const auto next = archive_.data_pointer(offset + kDObjNext);
    if (next.has_value() && *next != Archive::kNullOffset) {
        HSD_DObjDesc* following = convert_display_object(*next);
        if (following == nullptr) {
            return nullptr;
        }
        host.next = following;
    }

    return &host;
}

void ArchiveConverter::note_unconverted(uint32_t holder, uint32_t target,
                                        const char* kind)
{
    for (const UnconvertedReference& seen : unconverted_) {
        if (seen.holder == holder && seen.target == target) {
            return;
        }
    }
    unconverted_.push_back(UnconvertedReference{ holder, target, kind });
}

HSD_Joint* ArchiveConverter::joint(uint32_t offset)
{
    error_.clear();
    if (offset == Archive::kNullOffset) {
        return nullptr;
    }

    // Two passes over a worklist rather than recursion: a joint tree is a
    // graph with a sibling chain that can be long, and an archive that is
    // wrong can point a joint at itself.  The offset map ends both.
    std::vector<uint32_t> pending{ offset };
    std::vector<uint32_t> converted;

    while (!pending.empty()) {
        const uint32_t current = pending.back();
        pending.pop_back();
        if (joints_by_offset_.count(current) != 0) {
            continue;
        }
        if (!archive_.contains_data_range(current, kJointSize)) {
            error_ = "joint at offset " + std::to_string(current) +
                     " lies outside the archive's data section";
            return nullptr;
        }

        const auto flags = archive_.data_word(current + kJointFlags);
        const auto child = archive_.data_pointer(current + kJointChild);
        const auto next = archive_.data_pointer(current + kJointNext);
        const auto class_name = archive_.data_pointer(current + kJointClassName);
        if (!flags.has_value() || !child.has_value() || !next.has_value() ||
            !class_name.has_value()) {
            error_ = "joint at offset " + std::to_string(current) +
                     " has a field that is neither a null nor a relocated "
                     "offset";
            return nullptr;
        }

        joints_.emplace_back();
        HSD_Joint& host = joints_.back();
        std::memset(&host, 0, sizeof host);
        host.flags = *flags;
        // Registered before its fields are filled in, and the reference
        // stays valid however much the deques grow, so anything the
        // conversion reaches that points back here finds this joint.
        joints_by_offset_.emplace(current, &host);

        if (*class_name != Archive::kNullOffset) {
            host.class_name = read_string(*class_name);
            if (host.class_name == nullptr) {
                error_ = "joint at offset " + std::to_string(current) +
                         " names a class that runs off the data section";
                return nullptr;
            }
        }

        if (!read_vector(current + kJointRotation, host.rotation) ||
            !read_vector(current + kJointScale, host.scale) ||
            !read_vector(current + kJointPosition, host.position)) {
            error_ = "joint at offset " + std::to_string(current) +
                     " has a transform outside the data section";
            return nullptr;
        }

        const auto matrix = archive_.data_pointer(current + kJointMatrix);
        if (matrix.has_value() && *matrix != Archive::kNullOffset) {
            if (!read_matrix(*matrix, host.mtx)) {
                error_ = "joint at offset " + std::to_string(current) +
                         " carries a matrix outside the data section";
                return nullptr;
            }
        }

        // A joint's union is a display object, a spline, or a particle list,
        // and which one the flags decide.  The display object is built; the
        // other two are recorded rather than refused, because the rest of the
        // graph is correct and a caller can see exactly what is missing.
        const auto union_field = archive_.data_pointer(current + kJointUnion);
        if (union_field.has_value() && *union_field != Archive::kNullOffset) {
            if (union_is_display_object(*flags)) {
                HSD_DObjDesc* display = convert_display_object(*union_field);
                if (display == nullptr) {
                    return nullptr;
                }
                host.u.dobjdesc = display;
            } else {
                note_unconverted(current, *union_field, union_kind(*flags));
            }
        }
        const auto robjdesc = archive_.data_pointer(current + kJointRObjDesc);
        if (robjdesc.has_value() && *robjdesc != Archive::kNullOffset) {
            note_unconverted(current, *robjdesc, "HSD_RObjDesc");
        }

        converted.push_back(current);
        if (*child != Archive::kNullOffset) {
            pending.push_back(*child);
        }
        if (*next != Archive::kNullOffset) {
            pending.push_back(*next);
        }
    }

    // Second pass: now that every joint has an address, link them.
    for (uint32_t current : converted) {
        HSD_Joint* host = joints_by_offset_.at(current);
        const uint32_t child = *archive_.data_pointer(current + kJointChild);
        const uint32_t next = *archive_.data_pointer(current + kJointNext);
        if (child != Archive::kNullOffset) {
            host->child = joints_by_offset_.at(child);
        }
        if (next != Archive::kNullOffset) {
            host->next = joints_by_offset_.at(next);
        }
    }

    return joints_by_offset_.at(offset);
}

} // namespace meleeboard::hsd
