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

// HSD_TObjDesc, 0x5C bytes.
constexpr uint32_t kTObjClassName = 0x00;
constexpr uint32_t kTObjNext = 0x04;
constexpr uint32_t kTObjId = 0x08;
constexpr uint32_t kTObjSrc = 0x0C;
constexpr uint32_t kTObjRotate = 0x10;
constexpr uint32_t kTObjScale = 0x1C;
constexpr uint32_t kTObjTranslate = 0x28;
constexpr uint32_t kTObjWrapS = 0x34;
constexpr uint32_t kTObjWrapT = 0x38;
constexpr uint32_t kTObjRepeatS = 0x3C;
constexpr uint32_t kTObjRepeatT = 0x3D;
constexpr uint32_t kTObjBlendFlags = 0x40;
constexpr uint32_t kTObjBlending = 0x44;
constexpr uint32_t kTObjMagFilt = 0x48;
constexpr uint32_t kTObjImageDesc = 0x4C;
constexpr uint32_t kTObjTlutDesc = 0x50;
constexpr uint32_t kTObjLod = 0x54;
constexpr uint32_t kTObjTev = 0x58;
constexpr uint32_t kTObjSize = 0x5C;

// HSD_ImageDesc, 0x18 bytes.  image_ptr addresses pixels, which are neither
// a structure nor host-endian -- Aurora's GX reads the console's own tiled
// formats, so they cross untouched.
constexpr uint32_t kImagePtr = 0x00;
constexpr uint32_t kImageWidth = 0x04;
constexpr uint32_t kImageHeight = 0x06;
constexpr uint32_t kImageFormat = 0x08;
constexpr uint32_t kImageMipmap = 0x0C;
constexpr uint32_t kImageMinLOD = 0x10;
constexpr uint32_t kImageMaxLOD = 0x14;
constexpr uint32_t kImageSize = 0x18;

// HSD_TlutDesc, 0x10 bytes.
constexpr uint32_t kTlutLut = 0x00;
constexpr uint32_t kTlutFmt = 0x04;
constexpr uint32_t kTlutName = 0x08;
constexpr uint32_t kTlutEntries = 0x0C;
constexpr uint32_t kTlutSize = 0x10;

// HSD_TexLODDesc, 0x10 bytes: GXBool is a byte on the console, so the two
// flags share a word with two bytes of padding.
constexpr uint32_t kLodMinFilt = 0x00;
constexpr uint32_t kLodBias = 0x04;
constexpr uint32_t kLodBiasClamp = 0x08;
constexpr uint32_t kLodEdgeEnable = 0x09;
constexpr uint32_t kLodMaxAnisotropy = 0x0C;
constexpr uint32_t kLodSize = 0x10;

// HSD_TObjTevDesc, 0x20 bytes: sixteen selector bytes, three GXColors, and a
// word of flags.  No pointers, so every field crosses by value.
constexpr uint32_t kTevSelectors = 0x00;
constexpr uint32_t kTevSelectorCount = 16;
constexpr uint32_t kTevKonst = 0x10;
constexpr uint32_t kTevTev0 = 0x14;
constexpr uint32_t kTevTev1 = 0x18;
constexpr uint32_t kTevActive = 0x1C;
constexpr uint32_t kTevSize = 0x20;

// A texture chain longer than this is a corrupt archive rather than a rich
// material: GX has eight texture maps.
constexpr uint32_t kMaxTextureChain = 16;

// A vertex descriptor list of more entries than this is a corrupt archive
// rather than a large mesh: HSD has nine vertex attributes plus eight
// texture coordinates.
constexpr uint32_t kMaxVertexDescriptors = 64;

// The display list HSD hands to GXCallDisplayList is stored in 32-byte units.
constexpr uint32_t kDisplayListGranularity = 32;

} // namespace

u16 ArchiveConverter::read_u16(uint32_t offset)
{
    const auto high = archive_.data_byte(offset);
    const auto low = archive_.data_byte(offset + 1);
    if (!high.has_value() || !low.has_value()) {
        return 0;
    }
    return static_cast<u16>((*high << 8) | *low);
}

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
        host.stride = read_u16(entry + kVtxStride);

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

    host.flags = read_u16(offset + kPObjFlags);
    host.n_display = read_u16(offset + kPObjDisplayCount);

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

HSD_TObjDesc* ArchiveConverter::texture(uint32_t offset)
{
    error_.clear();
    if (offset == Archive::kNullOffset) {
        return nullptr;
    }
    return convert_texture(offset);
}

HSD_ImageDesc* ArchiveConverter::convert_image(uint32_t offset)
{
    const auto seen = images_by_offset_.find(offset);
    if (seen != images_by_offset_.end()) {
        return seen->second;
    }
    if (!archive_.contains_data_range(offset, kImageSize)) {
        error_ = "image at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    images_.emplace_back();
    HSD_ImageDesc& host = images_.back();
    std::memset(&host, 0, sizeof host);
    images_by_offset_.emplace(offset, &host);

    host.width = read_u16(offset + kImageWidth);
    host.height = read_u16(offset + kImageHeight);
    host.format =
        static_cast<GXTexFmt>(*archive_.data_word(offset + kImageFormat));
    host.mipmap = *archive_.data_word(offset + kImageMipmap);
    host.minLOD = *archive_.data_float(offset + kImageMinLOD);
    host.maxLOD = *archive_.data_float(offset + kImageMaxLOD);

    // Pixels are not a structure and are not host-endian either: Aurora's GX
    // reads the console's own tiled formats, so they stay where they lie.
    const auto pixels = archive_.data_pointer(offset + kImagePtr);
    if (pixels.has_value() && *pixels != Archive::kNullOffset) {
        host.image_ptr = const_cast<void*>(raw_data(*pixels, nullptr));
        if (host.image_ptr == nullptr) {
            error_ = "image at offset " + std::to_string(offset) +
                     " points at pixels outside the data section";
            return nullptr;
        }
    }

    return &host;
}

HSD_TlutDesc* ArchiveConverter::convert_palette(uint32_t offset)
{
    const auto seen = palettes_by_offset_.find(offset);
    if (seen != palettes_by_offset_.end()) {
        return seen->second;
    }
    if (!archive_.contains_data_range(offset, kTlutSize)) {
        error_ = "palette at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    palettes_.emplace_back();
    HSD_TlutDesc& host = palettes_.back();
    std::memset(&host, 0, sizeof host);
    palettes_by_offset_.emplace(offset, &host);

    host.fmt = static_cast<GXTlutFmt>(*archive_.data_word(offset + kTlutFmt));
    host.tlut_name = *archive_.data_word(offset + kTlutName);
    host.n_entries = read_u16(offset + kTlutEntries);

    const auto lut = archive_.data_pointer(offset + kTlutLut);
    if (lut.has_value() && *lut != Archive::kNullOffset) {
        host.lut = const_cast<void*>(raw_data(*lut, nullptr));
        if (host.lut == nullptr) {
            error_ = "palette at offset " + std::to_string(offset) +
                     " points at entries outside the data section";
            return nullptr;
        }
    }

    return &host;
}

HSD_TexLODDesc* ArchiveConverter::convert_texture_lod(uint32_t offset)
{
    if (!archive_.contains_data_range(offset, kLodSize)) {
        error_ = "texture LOD descriptor at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    texture_lods_.emplace_back();
    HSD_TexLODDesc& host = texture_lods_.back();
    std::memset(&host, 0, sizeof host);
    host.minFilt =
        static_cast<GXTexFilter>(*archive_.data_word(offset + kLodMinFilt));
    host.LODBias = *archive_.data_float(offset + kLodBias);
    host.bias_clamp = *archive_.data_byte(offset + kLodBiasClamp);
    host.edgeLODEnable = *archive_.data_byte(offset + kLodEdgeEnable);
    host.max_anisotropy = static_cast<GXAnisotropy>(
        *archive_.data_word(offset + kLodMaxAnisotropy));
    return &host;
}

HSD_TObjTevDesc* ArchiveConverter::convert_texture_tev(uint32_t offset)
{
    if (!archive_.contains_data_range(offset, kTevSize)) {
        error_ = "texture TEV descriptor at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    texture_tevs_.emplace_back();
    HSD_TObjTevDesc& host = texture_tevs_.back();
    std::memset(&host, 0, sizeof host);

    // Sixteen selector bytes in declaration order, which is also their order
    // in the structure on both sides -- they are bytes, so nothing moves.
    u8* selectors = &host.color_op;
    for (uint32_t index = 0; index < kTevSelectorCount; ++index) {
        selectors[index] =
            *archive_.data_byte(offset + kTevSelectors + index);
    }

    const auto colour = [this, offset](uint32_t field, GXColor& out) {
        out.r = *archive_.data_byte(offset + field + 0);
        out.g = *archive_.data_byte(offset + field + 1);
        out.b = *archive_.data_byte(offset + field + 2);
        out.a = *archive_.data_byte(offset + field + 3);
    };
    colour(kTevKonst, host.konst);
    colour(kTevTev0, host.tev0);
    colour(kTevTev1, host.tev1);
    host.active = *archive_.data_word(offset + kTevActive);
    return &host;
}

HSD_TObjDesc* ArchiveConverter::convert_texture(uint32_t offset)
{
    const auto seen = textures_by_offset_.find(offset);
    if (seen != textures_by_offset_.end()) {
        return seen->second;
    }
    if (!archive_.contains_data_range(offset, kTObjSize)) {
        error_ = "texture at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    textures_.emplace_back();
    HSD_TObjDesc& host = textures_.back();
    std::memset(&host, 0, sizeof host);
    textures_by_offset_.emplace(offset, &host);

    host.id = static_cast<GXTexMapID>(*archive_.data_word(offset + kTObjId));
    host.src = static_cast<GXTexGenSrc>(*archive_.data_word(offset + kTObjSrc));
    host.wrap_s =
        static_cast<GXTexWrapMode>(*archive_.data_word(offset + kTObjWrapS));
    host.wrap_t =
        static_cast<GXTexWrapMode>(*archive_.data_word(offset + kTObjWrapT));
    host.repeat_s = *archive_.data_byte(offset + kTObjRepeatS);
    host.repeat_t = *archive_.data_byte(offset + kTObjRepeatT);
    host.blend_flags = *archive_.data_word(offset + kTObjBlendFlags);
    host.blending = *archive_.data_float(offset + kTObjBlending);
    host.magFilt =
        static_cast<GXTexFilter>(*archive_.data_word(offset + kTObjMagFilt));

    if (!read_vector(offset + kTObjRotate, host.rotate) ||
        !read_vector(offset + kTObjScale, host.scale) ||
        !read_vector(offset + kTObjTranslate, host.translate)) {
        error_ = "texture at offset " + std::to_string(offset) +
                 " has a transform outside the data section";
        return nullptr;
    }

    const auto class_name = archive_.data_pointer(offset + kTObjClassName);
    if (class_name.has_value() && *class_name != Archive::kNullOffset) {
        host.class_name = read_string(*class_name);
    }

    const auto imagedesc = archive_.data_pointer(offset + kTObjImageDesc);
    if (imagedesc.has_value() && *imagedesc != Archive::kNullOffset) {
        host.imagedesc = convert_image(*imagedesc);
        if (host.imagedesc == nullptr) {
            return nullptr;
        }
    }

    const auto tlutdesc = archive_.data_pointer(offset + kTObjTlutDesc);
    if (tlutdesc.has_value() && *tlutdesc != Archive::kNullOffset) {
        host.tlutdesc = convert_palette(*tlutdesc);
        if (host.tlutdesc == nullptr) {
            return nullptr;
        }
    }

    const auto lod = archive_.data_pointer(offset + kTObjLod);
    if (lod.has_value() && *lod != Archive::kNullOffset) {
        host.lod = convert_texture_lod(*lod);
        if (host.lod == nullptr) {
            return nullptr;
        }
    }

    const auto tev = archive_.data_pointer(offset + kTObjTev);
    if (tev.has_value() && *tev != Archive::kNullOffset) {
        host.tev = convert_texture_tev(*tev);
        if (host.tev == nullptr) {
            return nullptr;
        }
    }

    const auto next = archive_.data_pointer(offset + kTObjNext);
    if (next.has_value() && *next != Archive::kNullOffset) {
        if (textures_.size() > kMaxTextureChain) {
            error_ = "texture chain from offset " + std::to_string(offset) +
                     " is longer than GX has texture maps";
            return nullptr;
        }
        HSD_TObjDesc* following = convert_texture(*next);
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

    const auto texdesc = archive_.data_pointer(offset + kMObjTexDesc);
    if (texdesc.has_value() && *texdesc != Archive::kNullOffset) {
        host.texdesc = convert_texture(*texdesc);
        if (host.texdesc == nullptr) {
            return nullptr;
        }
    }

    // The TEV render descriptor and the pixel-engine descriptor are the next
    // two converters.  A material that names one is still usable without it,
    // so they are recorded rather than refused.
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
