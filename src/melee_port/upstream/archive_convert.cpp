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

// HSD_Spline, 0x18 bytes on the console.  sysdolphin/baselib/spline.h.
constexpr uint32_t kSplineType = 0x00;
constexpr uint32_t kSplineControlCount = 0x02;
constexpr uint32_t kSplineTension = 0x04;
constexpr uint32_t kSplineControlPoints = 0x08;
constexpr uint32_t kSplineTotalLength = 0x0C;
constexpr uint32_t kSplineSegmentLengths = 0x10;
constexpr uint32_t kSplineSegmentPolynomials = 0x14;
constexpr uint32_t kSplineSize = 0x18;

// HSD_SList, 8 bytes: a next pointer and one word beside it.
constexpr uint32_t kSListNext = 0x00;
constexpr uint32_t kSListData = 0x04;
constexpr uint32_t kSListSize = 0x08;

// Sanity limits.  A spline with more control points than this, or a particle
// list longer than this, is a misread structure rather than content: Melee's
// splines run to a few dozen points and its particle lists to a handful of
// entries.  Without a limit a corrupt `next` field is an endless walk.
constexpr uint32_t kMaxControlPoints = 4096;
constexpr uint32_t kMaxParticleNodes = 4096;

// How many control points splGetSplinePoint actually reads -- which is not
// numcv.
//
// numcv divides *parameter space*: the runtime computes u * (numcv - 1) and
// takes the integer part as a segment index.  How many points a segment needs
// is the curve's business, and each line below is read straight off
// splGetSplinePoint's own indexing, in both the u < 1 branch and the u == 1
// one:
//
//   type 0, linear    cv[idx], cv[idx+1], idx <= numcv-2     -> numcv
//   type 1, Bezier    cv[idx*3 .. idx*3+3]                   -> 3*numcv - 2
//   type 2, B-spline  cv[idx-1 .. idx+2]                     -> numcv + 2
//   type 3, cardinal  cv[idx .. idx+3]                       -> numcv + 2
//
// Converting only numcv of them would leave upstream reading past the end of
// the host array on a curve the console draws correctly -- the kind of bug
// that shows up as one wrong point at the end of a camera path.
uint32_t control_point_count(u8 type, uint32_t numcv)
{
    if (numcv < 2) {
        return numcv;
    }
    switch (type) {
    case 1:
        return 3 * numcv - 2;
    case 2:
    case 3:
        return numcv + 2;
    default:
        return numcv;
    }
}

// What the union at +0x10 holds is decided by two of the joint's flags.
bool union_is_display_object(uint32_t flags)
{
    return (flags & (JOBJ_PTCL | JOBJ_SPLINE)) == 0;
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

// HSD_ShapeSetDesc, 0x1C bytes.  The two index tables hold nb_shape pointers
// each -- pobj.c clamps its shape id to nb_shape - 1 before indexing them.
constexpr uint32_t kShapeFlags = 0x00;
constexpr uint32_t kShapeCount = 0x02;
constexpr uint32_t kShapeVertexIndexCount = 0x04;
constexpr uint32_t kShapeVertexDesc = 0x08;
constexpr uint32_t kShapeVertexIndexList = 0x0C;
constexpr uint32_t kShapeNormalIndexCount = 0x10;
constexpr uint32_t kShapeNormalDesc = 0x14;
constexpr uint32_t kShapeNormalIndexList = 0x18;
constexpr uint32_t kShapeSetSize = 0x1C;

// HSD_EnvelopeDesc, eight bytes: a joint and a weight.  A run of them ends at
// the entry whose joint is NULL -- and "NULL" here means a pointer field the
// relocation table does not name, which is exactly the distinction a wire
// struct could not make.  A run terminated by a *relocated* zero would be a
// run whose last entry weights joint 0.
constexpr uint32_t kEnvelopeJoint = 0x00;
constexpr uint32_t kEnvelopeWeight = 0x04;
constexpr uint32_t kEnvelopeSize = 0x08;

// HSD_RObjDesc, 0x0C bytes: a chain pointer, flags, and a union whose meaning
// the top nibble of the flags decides.
constexpr uint32_t kRObjNext = 0x00;
constexpr uint32_t kRObjFlags = 0x04;
constexpr uint32_t kRObjUnion = 0x08;
constexpr uint32_t kRObjSize = 0x0C;

// HSD_IKHintDesc, HSD_ExpDesc, HSD_ByteCodeExpDesc and HSD_RvalueList are all
// eight bytes: two floats, two pointers, two pointers, and a word beside a
// pointer.  An rvalue list is an array ending at the entry whose joint is
// NULL -- the same relocation-dependent terminator as an envelope run.
constexpr uint32_t kIKHintBoneLength = 0x00;
constexpr uint32_t kIKHintRotateX = 0x04;
constexpr uint32_t kIKHintSize = 0x08;
constexpr uint32_t kExpFunc = 0x00;
constexpr uint32_t kExpRvalue = 0x04;
constexpr uint32_t kExpSize = 0x08;
constexpr uint32_t kBytecodeExpBytecode = 0x00;
constexpr uint32_t kBytecodeExpRvalue = 0x04;
constexpr uint32_t kRvalueFlags = 0x00;
constexpr uint32_t kRvalueJoint = 0x04;
constexpr uint32_t kRvalueSize = 0x08;

// A reference-object chain or rvalue list longer than this is a corrupt
// archive rather than an elaborate rig.
constexpr uint32_t kMaxReferenceChain = 64;
constexpr uint32_t kMaxRvalueList = 64;

// Bounds on the two tables, past which the archive is corrupt rather than
// elaborate.  GX addresses ten position matrices, and a vertex weighted by
// more than a few joints is not a thing Melee's rigs do.
constexpr uint32_t kMaxEnvelopeTable = 64;
constexpr uint32_t kMaxEnvelopeRun = 64;
constexpr uint32_t kMaxShapeCount = 1024;

// HSD_PEDesc, twelve bytes and every one of them a u8, so the structure is
// the same size on both sides -- which matters, because MObjLoad copies it
// with memcpy(sizeof(HSD_PEDesc)) rather than field by field.
constexpr uint32_t kPEDescSize = 12;

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

u8** ArchiveConverter::convert_index_table(uint32_t offset, uint32_t count,
                                           const char* what)
{
    // An array of pointers to index runs.  The runs are bytes, so they stay
    // in the archive; only the array of addresses has to be rebuilt at host
    // width.
    std::vector<u8*> table;
    table.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const auto entry = archive_.data_pointer(offset + index * 4);
        if (!entry.has_value()) {
            error_ = std::string(what) + " at offset " +
                     std::to_string(offset) + " runs off the data section";
            return nullptr;
        }
        if (*entry == Archive::kNullOffset) {
            table.push_back(nullptr);
            continue;
        }
        u8* run = const_cast<u8*>(static_cast<const u8*>(
            raw_data(*entry, nullptr)));
        if (run == nullptr) {
            error_ = std::string(what) + " at offset " +
                     std::to_string(offset) + " points outside the data "
                     "section";
            return nullptr;
        }
        table.push_back(run);
    }
    index_tables_.push_back(std::move(table));
    return index_tables_.back().data();
}

HSD_ShapeSetDesc* ArchiveConverter::convert_shape_set(uint32_t offset)
{
    if (!archive_.contains_data_range(offset, kShapeSetSize)) {
        error_ = "shape set at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    shape_sets_.emplace_back();
    HSD_ShapeSetDesc& host = shape_sets_.back();
    std::memset(&host, 0, sizeof host);

    host.flags = read_u16(offset + kShapeFlags);
    host.nb_shape = read_u16(offset + kShapeCount);
    host.nb_vertex_index =
        static_cast<s32>(*archive_.data_word(offset + kShapeVertexIndexCount));
    host.nb_normal_index =
        static_cast<s32>(*archive_.data_word(offset + kShapeNormalIndexCount));

    if (host.nb_shape > kMaxShapeCount) {
        error_ = "shape set at offset " + std::to_string(offset) +
                 " claims more shapes than an archive can hold";
        return nullptr;
    }

    const auto vertex_desc = archive_.data_pointer(offset + kShapeVertexDesc);
    if (vertex_desc.has_value() && *vertex_desc != Archive::kNullOffset) {
        host.vertex_desc = convert_vertex_descriptors(*vertex_desc);
        if (host.vertex_desc == nullptr) {
            return nullptr;
        }
    }
    const auto normal_desc = archive_.data_pointer(offset + kShapeNormalDesc);
    if (normal_desc.has_value() && *normal_desc != Archive::kNullOffset) {
        host.normal_desc = convert_vertex_descriptors(*normal_desc);
        if (host.normal_desc == nullptr) {
            return nullptr;
        }
    }

    const auto vertex_list =
        archive_.data_pointer(offset + kShapeVertexIndexList);
    if (vertex_list.has_value() && *vertex_list != Archive::kNullOffset) {
        host.vertex_idx_list = convert_index_table(*vertex_list, host.nb_shape,
                                                   "shape vertex index table");
        if (host.vertex_idx_list == nullptr) {
            return nullptr;
        }
    }
    const auto normal_list =
        archive_.data_pointer(offset + kShapeNormalIndexList);
    if (normal_list.has_value() && *normal_list != Archive::kNullOffset) {
        host.normal_idx_list = convert_index_table(*normal_list, host.nb_shape,
                                                   "shape normal index table");
        if (host.normal_idx_list == nullptr) {
            return nullptr;
        }
    }

    return &host;
}

HSD_EnvelopeDesc* ArchiveConverter::convert_envelope_run(uint32_t offset)
{
    // A run of {joint, weight} pairs ending at the entry whose joint is NULL.
    // NULL means a pointer field the relocation table does not name: a run
    // ended by a *relocated* zero is a run whose last entry weights the joint
    // at data-section offset zero, and the two look identical in the bytes.
    std::vector<HSD_EnvelopeDesc> run;
    for (uint32_t index = 0; index <= kMaxEnvelopeRun; ++index) {
        const uint32_t entry = offset + index * kEnvelopeSize;
        if (!archive_.contains_data_range(entry, kEnvelopeSize)) {
            error_ = "envelope run at offset " + std::to_string(offset) +
                     " runs off the end of the data section";
            return nullptr;
        }
        const auto joint_offset =
            archive_.data_pointer(entry + kEnvelopeJoint);
        if (!joint_offset.has_value()) {
            error_ = "envelope run at offset " + std::to_string(offset) +
                     " has a joint field that is neither null nor relocated";
            return nullptr;
        }

        HSD_EnvelopeDesc host{};
        std::memset(&host, 0, sizeof host);
        if (*joint_offset == Archive::kNullOffset) {
            run.push_back(host);
            envelopes_.push_back(std::move(run));
            return envelopes_.back().data();
        }

        host.joint = convert_joint_graph(*joint_offset);
        if (host.joint == nullptr) {
            return nullptr;
        }
        host.weight = *archive_.data_float(entry + kEnvelopeWeight);
        run.push_back(host);
    }

    error_ = "envelope run at offset " + std::to_string(offset) +
             " has no terminating entry";
    return nullptr;
}

HSD_EnvelopeDesc** ArchiveConverter::convert_envelope_table(uint32_t offset)
{
    // A NULL-terminated array of pointers, one per position-matrix slot.
    std::vector<HSD_EnvelopeDesc*> table;
    for (uint32_t index = 0; index <= kMaxEnvelopeTable; ++index) {
        const auto entry = archive_.data_pointer(offset + index * 4);
        if (!entry.has_value()) {
            error_ = "envelope table at offset " + std::to_string(offset) +
                     " runs off the end of the data section";
            return nullptr;
        }
        if (*entry == Archive::kNullOffset) {
            table.push_back(nullptr);
            envelope_tables_.push_back(std::move(table));
            return envelope_tables_.back().data();
        }
        HSD_EnvelopeDesc* run = convert_envelope_run(*entry);
        if (run == nullptr) {
            return nullptr;
        }
        table.push_back(run);
    }

    error_ = "envelope table at offset " + std::to_string(offset) +
             " has no terminating entry";
    return nullptr;
}

HSD_RvalueList* ArchiveConverter::convert_rvalue_list(uint32_t offset)
{
    std::vector<HSD_RvalueList> list;
    for (uint32_t index = 0; index <= kMaxRvalueList; ++index) {
        const uint32_t entry = offset + index * kRvalueSize;
        if (!archive_.contains_data_range(entry, kRvalueSize)) {
            error_ = "rvalue list at offset " + std::to_string(offset) +
                     " runs off the end of the data section";
            return nullptr;
        }
        const auto joint_offset = archive_.data_pointer(entry + kRvalueJoint);
        if (!joint_offset.has_value()) {
            error_ = "rvalue list at offset " + std::to_string(offset) +
                     " has a joint field that is neither null nor relocated";
            return nullptr;
        }

        HSD_RvalueList host{};
        std::memset(&host, 0, sizeof host);
        host.flags = *archive_.data_word(entry + kRvalueFlags);
        if (*joint_offset == Archive::kNullOffset) {
            // loadRvalue stops here, so the terminator's flags never matter.
            host.joint = nullptr;
            list.push_back(host);
            rvalue_lists_.push_back(std::move(list));
            return rvalue_lists_.back().data();
        }
        host.joint = convert_joint_graph(*joint_offset);
        if (host.joint == nullptr) {
            return nullptr;
        }
        list.push_back(host);
    }

    error_ = "rvalue list at offset " + std::to_string(offset) +
             " has no terminating entry";
    return nullptr;
}

HSD_RObjDesc* ArchiveConverter::convert_reference_object(uint32_t offset)
{
    const auto seen = reference_objects_by_offset_.find(offset);
    if (seen != reference_objects_by_offset_.end()) {
        return seen->second;
    }
    if (!archive_.contains_data_range(offset, kRObjSize)) {
        error_ = "reference object at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }
    if (reference_objects_.size() > kMaxReferenceChain) {
        error_ = "reference object chain is longer than an archive can hold";
        return nullptr;
    }

    reference_objects_.emplace_back();
    HSD_RObjDesc& host = reference_objects_.back();
    std::memset(&host, 0, sizeof host);
    reference_objects_by_offset_.emplace(offset, &host);

    host.flags = *archive_.data_word(offset + kRObjFlags);

    // The top nibble of the flags says what the union holds.  Upstream's own
    // switch in HSD_RObjLoadDesc is the specification, and it panics on a
    // value it does not know, so an unknown type is reported rather than
    // guessed at here.
    switch (host.flags & ROBJ_TYPE_MASK) {
    case REFTYPE_LIMIT:
        // A float by value, not a pointer.
        host.u.limit = *archive_.data_float(offset + kRObjUnion);
        break;

    case REFTYPE_JOBJ: {
        const auto target = archive_.data_pointer(offset + kRObjUnion);
        if (target.has_value() && *target != Archive::kNullOffset) {
            host.u.joint = convert_joint_graph(*target);
            if (host.u.joint == nullptr) {
                return nullptr;
            }
        }
        break;
    }

    case REFTYPE_IKHINT: {
        const auto target = archive_.data_pointer(offset + kRObjUnion);
        if (target.has_value() && *target != Archive::kNullOffset) {
            if (!archive_.contains_data_range(*target, kIKHintSize)) {
                error_ = "IK hint at offset " + std::to_string(*target) +
                         " lies outside the archive's data section";
                return nullptr;
            }
            ik_hints_.emplace_back();
            HSD_IKHintDesc& hint = ik_hints_.back();
            hint.bone_length =
                *archive_.data_float(*target + kIKHintBoneLength);
            hint.rotate_x = *archive_.data_float(*target + kIKHintRotateX);
            host.u.ik_hint = &hint;
        }
        break;
    }

    case REFTYPE_BYTECODE: {
        const auto target = archive_.data_pointer(offset + kRObjUnion);
        if (target.has_value() && *target != Archive::kNullOffset) {
            if (!archive_.contains_data_range(*target, kExpSize)) {
                error_ = "bytecode expression at offset " +
                         std::to_string(*target) +
                         " lies outside the archive's data section";
                return nullptr;
            }
            bytecode_expressions_.emplace_back();
            HSD_ByteCodeExpDesc& exp = bytecode_expressions_.back();
            std::memset(&exp, 0, sizeof exp);
            const auto bytecode =
                archive_.data_pointer(*target + kBytecodeExpBytecode);
            if (bytecode.has_value() && *bytecode != Archive::kNullOffset) {
                exp.bytecode = const_cast<u8*>(
                    static_cast<const u8*>(raw_data(*bytecode, nullptr)));
            }
            const auto rvalue =
                archive_.data_pointer(*target + kBytecodeExpRvalue);
            if (rvalue.has_value() && *rvalue != Archive::kNullOffset) {
                exp.rvalue = convert_rvalue_list(*rvalue);
                if (exp.rvalue == nullptr) {
                    return nullptr;
                }
            }
            host.u.bcexp = &exp;
        }
        break;
    }

    case REFTYPE_EXP: {
        const auto target = archive_.data_pointer(offset + kRObjUnion);
        if (target.has_value() && *target != Archive::kNullOffset) {
            if (!archive_.contains_data_range(*target, kExpSize)) {
                error_ = "expression at offset " + std::to_string(*target) +
                         " lies outside the archive's data section";
                return nullptr;
            }
            expressions_.emplace_back();
            HSD_ExpDesc& exp = expressions_.back();
            std::memset(&exp, 0, sizeof exp);

            // func stays NULL, deliberately.  The field holds the address of
            // a function in the console's executable -- there is no host
            // value that means the same thing, and a truncated one would be
            // a jump into nothing.  Upstream already handles NULL here:
            // expLoadDesc substitutes dummy_func.  So the safe answer is also
            // upstream's own answer, which is why this is a null rather than
            // a refusal.
            const auto rvalue = archive_.data_pointer(*target + kExpRvalue);
            if (rvalue.has_value() && *rvalue != Archive::kNullOffset) {
                exp.rvalue = convert_rvalue_list(*rvalue);
                if (exp.rvalue == nullptr) {
                    return nullptr;
                }
            }
            host.u.exp = &exp;
        }
        break;
    }

    default: {
        const auto target = archive_.data_pointer(offset + kRObjUnion);
        if (target.has_value() && *target != Archive::kNullOffset) {
            note_unconverted(offset, *target, "HSD_RObjDesc::u (unknown type)");
        }
        break;
    }
    }

    const auto next = archive_.data_pointer(offset + kRObjNext);
    if (next.has_value() && *next != Archive::kNullOffset) {
        HSD_RObjDesc* following = convert_reference_object(*next);
        if (following == nullptr) {
            return nullptr;
        }
        host.next = following;
    }

    return &host;
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

    // The union at +0x14 is a joint, a shape set or an envelope table, and
    // pobj_type() -- two bits of the flags -- says which.  All three are
    // built; a fourth value of those two bits is not a thing upstream's own
    // switch handles, so it is reported rather than guessed at.
    const auto union_field = archive_.data_pointer(offset + kPObjUnion);
    if (union_field.has_value() && *union_field != Archive::kNullOffset) {
        switch (host.flags & 0x3000) {
        case POBJ_SKIN:
            host.u.joint = convert_joint_graph(*union_field);
            if (host.u.joint == nullptr) {
                return nullptr;
            }
            break;
        case POBJ_SHAPEANIM:
            host.u.shape_set = convert_shape_set(*union_field);
            if (host.u.shape_set == nullptr) {
                return nullptr;
            }
            break;
        case POBJ_ENVELOPE:
            host.u.envelope_p = convert_envelope_table(*union_field);
            if (host.u.envelope_p == nullptr) {
                return nullptr;
            }
            break;
        default:
            note_unconverted(offset, *union_field, "HSD_PObjDesc::u");
            break;
        }
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

HSD_Spline* ArchiveConverter::convert_spline(uint32_t offset)
{
    const auto seen = splines_by_offset_.find(offset);
    if (seen != splines_by_offset_.end()) {
        return seen->second;
    }
    if (!archive_.contains_data_range(offset, kSplineSize)) {
        error_ = "spline at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    splines_.emplace_back();
    HSD_Spline& host = splines_.back();
    std::memset(&host, 0, sizeof host);
    splines_by_offset_.emplace(offset, &host);

    host.type = *archive_.data_byte(offset + kSplineType);
    const s16 numcv = static_cast<s16>(read_u16(offset + kSplineControlCount));
    if (numcv < 0 || static_cast<uint32_t>(numcv) > kMaxControlPoints) {
        error_ = "spline at offset " + std::to_string(offset) +
                 " claims " + std::to_string(numcv) +
                 " control points, which is not a curve";
        return nullptr;
    }
    host.numcv = numcv;
    host.tension = *archive_.data_float(offset + kSplineTension);
    host.totalLength = *archive_.data_float(offset + kSplineTotalLength);

    // The three arrays below are precomputed on disc.  Nothing in the
    // decompilation fills segLength or segPoly -- spline.c only ever reads
    // them -- so the exporter wrote them, and converting them is the whole
    // job.
    const auto control = archive_.data_pointer(offset + kSplineControlPoints);
    if (control.has_value() && *control != Archive::kNullOffset) {
        const uint32_t count =
            control_point_count(host.type, static_cast<uint32_t>(numcv));
        if (!archive_.contains_data_range(*control, count * 12u)) {
            error_ = "spline at offset " + std::to_string(offset) +
                     " has control points running off the data section";
            return nullptr;
        }
        control_points_.emplace_back(count);
        std::vector<Vec3>& points = control_points_.back();
        for (uint32_t index = 0; index < count; ++index) {
            if (!read_vector(*control + index * 12u, points[index])) {
                error_ = "spline at offset " + std::to_string(offset) +
                         " has a control point outside the data section";
                return nullptr;
            }
        }
        host.cv = points.data();
    }

    // One entry per control parameter: splArcLengthGetParameter walks
    // segLength[idx + 1] with idx running to numcv - 2, so the last index it
    // reads is numcv - 1.
    const auto lengths = archive_.data_pointer(offset + kSplineSegmentLengths);
    if (lengths.has_value() && *lengths != Archive::kNullOffset) {
        const uint32_t count = static_cast<uint32_t>(numcv);
        if (!archive_.contains_data_range(*lengths, count * 4u)) {
            error_ = "spline at offset " + std::to_string(offset) +
                     " has segment lengths running off the data section";
            return nullptr;
        }
        segment_lengths_.emplace_back(count);
        std::vector<f32>& values = segment_lengths_.back();
        for (uint32_t index = 0; index < count; ++index) {
            values[index] = *archive_.data_float(*lengths + index * 4u);
        }
        host.segLength = values.data();
    }

    // Five coefficients per segment, and there are numcv - 1 segments.  Only
    // the curved types have them; a linear spline leaves the field NULL and
    // splArcLengthGetParameter's case 0 never looks.
    const auto polynomials =
        archive_.data_pointer(offset + kSplineSegmentPolynomials);
    if (polynomials.has_value() && *polynomials != Archive::kNullOffset) {
        const uint32_t rows =
            numcv > 1 ? static_cast<uint32_t>(numcv) - 1u : 0u;
        if (!archive_.contains_data_range(*polynomials, rows * 5u * 4u)) {
            error_ = "spline at offset " + std::to_string(offset) +
                     " has segment polynomials running off the data section";
            return nullptr;
        }
        segment_polynomials_.emplace_back(rows);
        std::vector<std::array<f32, 5>>& coefficients =
            segment_polynomials_.back();
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < 5; ++column) {
                coefficients[row][column] =
                    *archive_.data_float(*polynomials + (row * 5u + column) * 4u);
            }
        }
        // std::array<f32, 5> is five floats and nothing else, so an array of
        // them is exactly the f32[][5] upstream indexes.
        host.segPoly = reinterpret_cast<f32(*)[5]>(coefficients.data());
    }

    return &host;
}

// A joint's particle list, whose payload is not a pointer.
//
// HSD_SList is a next pointer beside a `void* data`, and everywhere else in
// HSD that field really is a pointer.  Here it is not: HSD_JObjLoadJoint does
// `*(u32*) &slist->data |= 0x80000000`, and HSD_JObjDisp reads a six-bit bank
// out of the bottom of it and a 24-bit offset above that.  The word is a
// packed integer that the archive's relocation table therefore does not name,
// which is why it is read with data_word and not data_pointer -- asking for a
// pointer would correctly refuse it.
//
// Storing it in the low half of a host pointer is what makes upstream's
// `*(u32*) &slist->data` reach the right bits, and that holds on a
// little-endian host.  The port has no big-endian target; if it ever gets
// one, this is the line that breaks, loudly, because the flag would land in
// the unused high word.
HSD_SList* ArchiveConverter::convert_particle_list(uint32_t offset)
{
    HSD_SList* head = nullptr;
    HSD_SList* tail = nullptr;
    uint32_t current = offset;

    for (uint32_t walked = 0; current != Archive::kNullOffset; ++walked) {
        if (walked >= kMaxParticleNodes) {
            error_ = "particle list at offset " + std::to_string(offset) +
                     " does not end";
            return nullptr;
        }

        // A node already converted -- the lists of two joints can share a
        // tail -- is linked to rather than copied, so identity is preserved
        // the way it is for every other structure here.
        const auto seen = particle_lists_by_offset_.find(current);
        if (seen != particle_lists_by_offset_.end()) {
            if (tail != nullptr) {
                tail->next = seen->second;
                return head;
            }
            return seen->second;
        }

        if (!archive_.contains_data_range(current, kSListSize)) {
            error_ = "particle list node at offset " + std::to_string(current) +
                     " lies outside the archive's data section";
            return nullptr;
        }

        particle_nodes_.emplace_back();
        HSD_SList& node = particle_nodes_.back();
        node.next = nullptr;
        node.data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
            *archive_.data_word(current + kSListData)));
        particle_lists_by_offset_.emplace(current, &node);

        if (tail != nullptr) {
            tail->next = &node;
        } else {
            head = &node;
        }
        tail = &node;

        const auto next = archive_.data_pointer(current + kSListNext);
        if (!next.has_value()) {
            error_ = "particle list node at offset " + std::to_string(current) +
                     " has a next field that is not a relocated pointer";
            return nullptr;
        }
        current = *next;
    }

    return head;
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

HSD_PEDesc* ArchiveConverter::convert_pixel_engine(uint32_t offset)
{
    if (!archive_.contains_data_range(offset, kPEDescSize)) {
        error_ = "pixel-engine descriptor at offset " + std::to_string(offset) +
                 " lies outside the archive's data section";
        return nullptr;
    }

    pixel_engines_.emplace_back();
    HSD_PEDesc& host = pixel_engines_.back();
    std::memset(&host, 0, sizeof host);
    static_assert(sizeof(HSD_PEDesc) == kPEDescSize,
                  "HSD_PEDesc is all u8 and must not change size on a host");

    u8* fields = &host.flags;
    for (uint32_t index = 0; index < kPEDescSize; ++index) {
        fields[index] = *archive_.data_byte(offset + index);
    }
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

    const auto pedesc = archive_.data_pointer(offset + kMObjPEDesc);
    if (pedesc.has_value() && *pedesc != Archive::kNullOffset) {
        host.pedesc = convert_pixel_engine(*pedesc);
        if (host.pedesc == nullptr) {
            return nullptr;
        }
    }

    // renderdesc is deliberately left null and deliberately not reported.
    // The field appears exactly once in the whole upstream tree -- its own
    // declaration in mobj.h -- so nothing reads it, and listing it as
    // unconverted would put something in that list that can never be cleared.
    // A caller is told the list has to be empty before it renders; that only
    // holds if everything in it is real work.

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
    return convert_joint_graph(offset);
}

// Re-entrant on purpose: a skinned primitive's envelope weights name joints,
// so this is called from inside its own display-object conversion.  Every
// joint is registered before its fields are filled in, so a nested call finds
// an entry rather than converting a second copy or recurring forever.
HSD_Joint* ArchiveConverter::convert_joint_graph(uint32_t offset)
{
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

        HSD_Joint* allocated = joint_arena_.allocate_one<HSD_Joint>();
        if (allocated == nullptr) {
            error_ = "more joints than the descriptor arena holds (" +
                     std::to_string(joint_arena_.capacity()) + " bytes)";
            return nullptr;
        }
        HSD_Joint& host = *allocated;
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

        // A joint's union is a display object, a spline, or a particle
        // list, and the flags decide which.  All three are built.
        const auto union_field = archive_.data_pointer(current + kJointUnion);
        if (union_field.has_value() && *union_field != Archive::kNullOffset) {
            // The order is jobj.c's own: HSD_JObjLoadJoint tests spline
            // before particle, so a joint that somehow carries both flags
            // resolves the way the console resolves it.
            if (union_is_display_object(*flags)) {
                HSD_DObjDesc* display = convert_display_object(*union_field);
                if (display == nullptr) {
                    return nullptr;
                }
                host.u.dobjdesc = display;
            } else if ((*flags & JOBJ_SPLINE) != 0) {
                HSD_Spline* spline = convert_spline(*union_field);
                if (spline == nullptr) {
                    return nullptr;
                }
                host.u.spline = spline;
            } else {
                HSD_SList* particles = convert_particle_list(*union_field);
                if (particles == nullptr) {
                    return nullptr;
                }
                host.u.ptcl = particles;
            }
        }
        const auto robjdesc = archive_.data_pointer(current + kJointRObjDesc);
        if (robjdesc.has_value() && *robjdesc != Archive::kNullOffset) {
            host.robjdesc = convert_reference_object(*robjdesc);
            if (host.robjdesc == nullptr) {
                return nullptr;
            }
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
