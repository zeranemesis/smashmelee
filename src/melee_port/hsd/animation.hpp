#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace meleeboard::hsd {

class Archive;

// Host-owned equivalents of HSD_AnimJoint/HSD_AObjDesc/HSD_FObjDesc.  DAT
// addresses are deliberately absent: bytecode and relationships are copied
// into host containers before the animation runtime sees them.
struct HostAnimationChannel {
    uint32_t source_offset = 0;
    int16_t start_frame = 0;
    uint8_t object_type = 0;
    uint8_t value_fraction = 0;
    uint8_t slope_fraction = 0;
    std::vector<uint8_t> bytecode;
};

struct HostAnimationObject {
    uint32_t flags = 0;
    float end_frame = 0.0F;
    uint32_t object_id = 0;
    std::vector<HostAnimationChannel> channels;
};

struct HostAnimationJoint {
    uint32_t source_offset = 0;
    int32_t child = -1;
    int32_t sibling = -1;
    uint32_t flags = 0;
    bool has_object = false;
    HostAnimationObject object;
};

class HostAnimation {
public:
    bool load(const Archive& archive, std::string_view symbol);

    const std::vector<HostAnimationJoint>& joints() const;
    const std::string& last_error() const;

private:
    std::vector<HostAnimationJoint> joints_;
    std::string last_error_;
};

} // namespace meleeboard::hsd
