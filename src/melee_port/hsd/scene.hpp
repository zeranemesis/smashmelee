#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace meleeboard::hsd {

class Archive;

struct HostJoint {
    uint32_t source_offset = 0;
    uint32_t flags = 0;
    int32_t child = -1;
    int32_t sibling = -1;
    int32_t first_draw_object = -1;
    std::array<float, 3> rotation{};
    std::array<float, 3> scale{};
    std::array<float, 3> translation{};
};

// Host-safe camera data copied from HSD_CObjDesc/HSD_WObjDesc.  It intentionally
// stores values rather than GameCube pointers so it can be handed to Aurora.
struct HostCamera {
    uint16_t flags = 0;
    uint16_t projection_type = 0;
    std::array<int16_t, 4> viewport{};
    std::array<float, 3> eye{};
    std::array<float, 3> interest{};
    std::array<float, 3> up{ 0.0F, 1.0F, 0.0F };
    float near_plane = 0.1F;
    float far_plane = 1000.0F;
    std::array<float, 4> projection{};
};

// GX vertex-array setup copied from a HSD_VtxDescList record.  The vertex
// data remains in the archive until the host renderer uploads it.
struct HostVertexDescriptor {
    uint32_t attribute = 0;
    uint32_t attribute_type = 0;
    uint32_t component_count = 0;
    uint32_t component_type = 0;
    uint32_t vertex_data = 0;
    uint16_t stride = 0;
    uint8_t fraction = 0;
};

struct HostDrawObject {
    uint32_t source_offset = 0;
    uint32_t material_description = 0;
    uint32_t primitive_description = 0;
    uint32_t render_mode = 0;
    uint32_t texture_description = 0;
    uint32_t material = 0;
    uint32_t vertex_description = 0;
    uint32_t display_list = 0;
    uint16_t primitive_flags = 0;
    uint16_t display_list_count = 0;
    uint32_t primitive_batch_count = 0;
    uint32_t vertex_count = 0;
    uint32_t triangle_count = 0;
    bool position_stream_decoded = false;
    std::string position_decode_error;
    // Position-array indices arranged as triangles.  These remain indices into
    // the HSD position array until the host GPU upload step.
    std::vector<uint32_t> triangle_position_indices;
    // Normal-array indices follow the triangle position stream one-for-one.
    // UINT32_MAX denotes a primitive without an indexed normal attribute.
    std::vector<uint32_t> triangle_normal_indices;
    std::vector<uint32_t> triangle_texcoord_indices;
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> texcoords;
    std::vector<uint32_t> triangle_indices;
    std::vector<HostVertexDescriptor> vertex_descriptors;
    int32_t next = -1;
};

// A host-safe version of a SceneDesc: all relationships are vector indices,
// not addresses into the GameCube DAT image.
class HostScene {
public:
    bool load(const Archive& archive, std::string_view symbol);

    const std::vector<HostJoint>& joints() const;
    const std::vector<HostCamera>& cameras() const;
    const std::vector<HostDrawObject>& draw_objects() const;
    const std::vector<uint32_t>& model_roots() const;
    const std::string& last_error() const;

private:
    std::vector<HostJoint> joints_;
    std::vector<HostCamera> cameras_;
    std::vector<HostDrawObject> draw_objects_;
    std::vector<uint32_t> model_roots_;
    std::string last_error_;
};

} // namespace meleeboard::hsd
