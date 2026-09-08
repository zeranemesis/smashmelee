#pragma once

#include <array>
#include <cstdint>
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

struct HostDrawObject {
    uint32_t source_offset = 0;
    uint32_t material_description = 0;
    uint32_t primitive_description = 0;
    int32_t next = -1;
};

// A host-safe version of a SceneDesc: all relationships are vector indices,
// not addresses into the GameCube DAT image.
class HostScene {
public:
    bool load(const Archive& archive, std::string_view symbol);

    const std::vector<HostJoint>& joints() const;
    const std::vector<HostDrawObject>& draw_objects() const;
    const std::vector<uint32_t>& model_roots() const;

private:
    std::vector<HostJoint> joints_;
    std::vector<HostDrawObject> draw_objects_;
    std::vector<uint32_t> model_roots_;
};

} // namespace meleeboard::hsd
