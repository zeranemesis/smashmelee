#pragma once

// Turning an HSD archive into host-sized structures.
//
// On the console, loading a DAT was almost free: HSD_ArchiveParse walked the
// relocation table and added the data section's base to every pointer field
// in place, and the blob *was* the object graph.  A 64-bit host cannot do
// that.  Every structure with a pointer in it is a different size, so the
// graph has to be built beside the blob rather than inside it.
//
// That is what this does, one structure kind at a time.  It is the mechanism
// phase 2 of docs/PLAN.md calls the *32b converters, and it does not take the
// shape the plan expected -- see the note on wire structs in the source.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <melee/port/dolphin_compat.h>
#include <melee/sysdolphin/baselib/archive.hpp>

#include "descriptor_arena.hpp"

extern "C" {
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/robj.h>
#include <sysdolphin/baselib/tobj.h>
}

namespace meleeboard::hsd {

// A pointer field the converter followed to something it cannot build yet.
// The graph it produces is complete except for these, and a renderer must
// not be run while any remain: a silently null display object draws nothing
// and looks exactly like a broken renderer.
struct UnconvertedReference {
    // Data-section offset of the structure that holds the field.
    uint32_t holder = 0;
    // Data-section offset the field points at.
    uint32_t target = 0;
    // What kind of structure lives there, in upstream's own spelling.
    const char* kind = "";
};

// Builds host HSD structures from one archive.  The archive must outlive the
// converter: strings are read out of it, and nothing is copied that does not
// have to be.
class ArchiveConverter {
public:
    // `joint_arena_bytes` sizes the block joint descriptors come from; see
    // descriptor_arena.hpp for why that block is one allocation and why its
    // size matters.  The default holds far more joints than an archive does.
    explicit ArchiveConverter(
        const Archive& archive,
        std::size_t joint_arena_bytes = DescriptorArena::kDefaultCapacity)
        : archive_(archive), joint_arena_(joint_arena_bytes)
    {
    }

    ArchiveConverter(const ArchiveConverter&) = delete;
    ArchiveConverter& operator=(const ArchiveConverter&) = delete;

    // Where a vertex array lives, and how much of it there is.  Vertex data
    // is not a structure -- no pointers, no size change on a 64-bit host --
    // so the converter leaves it in the archive and points at it.  It is
    // still in the console's byte order, which is why this is reported rather
    // than assumed: Aurora's GXSetArray wants to be told.
    struct VertexArray {
        const void* base = nullptr;
        uint32_t extent = 0;
    };

    // The joint at `offset`, with its children and siblings.  Null when the
    // archive holds no well-formed joint there, and error() says why.
    //
    // Converting the same offset twice returns the same pointer.  That is not
    // an optimization: HSD keys its ID table on a descriptor's address and
    // reference-counts by identity, so two host copies of one on-disc joint
    // would be two different objects to everything above.
    HSD_Joint* joint(uint32_t offset);

    const std::string& error() const { return error_; }

    // Every pointer this converter followed to a structure kind it does not
    // build yet, in the order it met them.
    const std::vector<UnconvertedReference>& unconverted() const
    {
        return unconverted_;
    }

    std::size_t joint_count() const { return joints_by_offset_.size(); }

    // Where the joint descriptors live.  Their addresses become ID-table
    // keys truncated to 32 bits, so they need storage whose low words are
    // distinct; see descriptor_arena.hpp.
    const DescriptorArena& joint_arena() const { return joint_arena_; }
    std::size_t display_object_count() const { return display_objects_.size(); }
    std::size_t primitive_count() const { return primitives_.size(); }
    std::size_t texture_count() const { return textures_.size(); }
    std::size_t envelope_count() const { return envelopes_.size(); }
    std::size_t reference_object_count() const
    {
        return reference_objects_.size();
    }

    // Every vertex array the converted primitives point at, in the order it
    // met them.  A renderer needs these to answer GXSetArray's extent and
    // byte-order arguments; see include/melee/port/dolphin_compat.h.
    const std::vector<VertexArray>& vertex_arrays() const
    {
        return vertex_arrays_;
    }

    // A texture on its own, for a caller holding an offset rather than a
    // material that names one -- a texture animation swapping images, or a
    // fixture assembling a model by hand.
    HSD_TObjDesc* texture(uint32_t offset);

private:
    HSD_DObjDesc* convert_display_object(uint32_t offset);
    HSD_MObjDesc* convert_material_object(uint32_t offset);
    HSD_PObjDesc* convert_primitive(uint32_t offset);
    HSD_Material* convert_material(uint32_t offset);
    HSD_VtxDescList* convert_vertex_descriptors(uint32_t offset);
    HSD_TObjDesc* convert_texture(uint32_t offset);
    HSD_ImageDesc* convert_image(uint32_t offset);
    HSD_TlutDesc* convert_palette(uint32_t offset);
    HSD_TexLODDesc* convert_texture_lod(uint32_t offset);
    HSD_TObjTevDesc* convert_texture_tev(uint32_t offset);
    HSD_PEDesc* convert_pixel_engine(uint32_t offset);
    HSD_Joint* convert_joint_graph(uint32_t offset);
    HSD_EnvelopeDesc** convert_envelope_table(uint32_t offset);
    HSD_EnvelopeDesc* convert_envelope_run(uint32_t offset);
    HSD_ShapeSetDesc* convert_shape_set(uint32_t offset);
    u8** convert_index_table(uint32_t offset, uint32_t count,
                             const char* what);
    HSD_RObjDesc* convert_reference_object(uint32_t offset);
    HSD_RvalueList* convert_rvalue_list(uint32_t offset);
    const void* raw_data(uint32_t offset, uint32_t* extent);

    // A big-endian u16, which the archive reader does not offer directly --
    // HSD stores several of them beside a byte rather than on a word
    // boundary, so they are read as two bytes.
    u16 read_u16(uint32_t offset);
    bool read_vector(uint32_t offset, Vec3& out);
    bool read_matrix(uint32_t offset, MtxPtr& out);
    char* read_string(uint32_t offset);
    void note_unconverted(uint32_t holder, uint32_t target, const char* kind);

    // Mtx is f32[3][4], and a raw array cannot be a container's element
    // type, so it travels in a struct.
    struct StoredMatrix {
        Mtx value;
    };

    const Archive& archive_;
    DescriptorArena joint_arena_;
    // Deques, because every address handed out has to stay put: the graph
    // points into this storage while it is still being built.
    std::deque<HSD_DObjDesc> display_objects_;
    std::deque<HSD_MObjDesc> material_objects_;
    std::deque<HSD_PObjDesc> primitives_;
    std::deque<HSD_Material> materials_;
    std::deque<HSD_TObjDesc> textures_;
    std::deque<HSD_ImageDesc> images_;
    std::deque<HSD_TlutDesc> palettes_;
    std::deque<HSD_TexLODDesc> texture_lods_;
    std::deque<HSD_TObjTevDesc> texture_tevs_;
    std::deque<HSD_PEDesc> pixel_engines_;
    std::deque<HSD_ShapeSetDesc> shape_sets_;
    // Each entry is one NULL-terminated run of envelope weights; the tables
    // hold pointers into them, NULL-terminated in turn.
    std::deque<std::vector<HSD_EnvelopeDesc>> envelopes_;
    std::deque<std::vector<HSD_EnvelopeDesc*>> envelope_tables_;
    std::deque<std::vector<u8*>> index_tables_;
    std::deque<HSD_RObjDesc> reference_objects_;
    std::deque<HSD_IKHintDesc> ik_hints_;
    std::deque<HSD_ExpDesc> expressions_;
    std::deque<HSD_ByteCodeExpDesc> bytecode_expressions_;
    std::deque<std::vector<HSD_RvalueList>> rvalue_lists_;
    std::deque<std::vector<HSD_VtxDescList>> vertex_descriptors_;
    std::deque<std::string> strings_;
    std::deque<StoredMatrix> matrices_;
    std::vector<VertexArray> vertex_arrays_;
    std::unordered_map<uint32_t, HSD_Joint*> joints_by_offset_;
    std::unordered_map<uint32_t, HSD_DObjDesc*> display_objects_by_offset_;
    std::unordered_map<uint32_t, HSD_MObjDesc*> material_objects_by_offset_;
    std::unordered_map<uint32_t, HSD_PObjDesc*> primitives_by_offset_;
    std::unordered_map<uint32_t, HSD_Material*> materials_by_offset_;
    std::unordered_map<uint32_t, HSD_TObjDesc*> textures_by_offset_;
    std::unordered_map<uint32_t, HSD_ImageDesc*> images_by_offset_;
    std::unordered_map<uint32_t, HSD_TlutDesc*> palettes_by_offset_;
    std::unordered_map<uint32_t, HSD_RObjDesc*> reference_objects_by_offset_;
    std::vector<UnconvertedReference> unconverted_;
    std::string error_;
};

} // namespace meleeboard::hsd
