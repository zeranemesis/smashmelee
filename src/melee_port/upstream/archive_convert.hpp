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

extern "C" {
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/pobj.h>
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
    explicit ArchiveConverter(const Archive& archive) : archive_(archive) {}

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

    std::size_t joint_count() const { return joints_.size(); }
    std::size_t display_object_count() const { return display_objects_.size(); }
    std::size_t primitive_count() const { return primitives_.size(); }

    // Every vertex array the converted primitives point at, in the order it
    // met them.  A renderer needs these to answer GXSetArray's extent and
    // byte-order arguments; see include/melee/port/dolphin_compat.h.
    const std::vector<VertexArray>& vertex_arrays() const
    {
        return vertex_arrays_;
    }

private:
    HSD_DObjDesc* convert_display_object(uint32_t offset);
    HSD_MObjDesc* convert_material_object(uint32_t offset);
    HSD_PObjDesc* convert_primitive(uint32_t offset);
    HSD_Material* convert_material(uint32_t offset);
    HSD_VtxDescList* convert_vertex_descriptors(uint32_t offset);
    const void* raw_data(uint32_t offset, uint32_t* extent);

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
    // Deques, because every address handed out has to stay put: the graph
    // points into this storage while it is still being built.
    std::deque<HSD_Joint> joints_;
    std::deque<HSD_DObjDesc> display_objects_;
    std::deque<HSD_MObjDesc> material_objects_;
    std::deque<HSD_PObjDesc> primitives_;
    std::deque<HSD_Material> materials_;
    std::deque<std::vector<HSD_VtxDescList>> vertex_descriptors_;
    std::deque<std::string> strings_;
    std::deque<StoredMatrix> matrices_;
    std::vector<VertexArray> vertex_arrays_;
    std::unordered_map<uint32_t, HSD_Joint*> joints_by_offset_;
    std::unordered_map<uint32_t, HSD_DObjDesc*> display_objects_by_offset_;
    std::unordered_map<uint32_t, HSD_MObjDesc*> material_objects_by_offset_;
    std::unordered_map<uint32_t, HSD_PObjDesc*> primitives_by_offset_;
    std::unordered_map<uint32_t, HSD_Material*> materials_by_offset_;
    std::vector<UnconvertedReference> unconverted_;
    std::string error_;
};

} // namespace meleeboard::hsd
