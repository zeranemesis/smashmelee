#pragma once

#include <cstdint>
#include <vector>

#include <melee/sysdolphin/baselib/aobj.h>

namespace meleeboard::hsd {

class HostAnimation;
struct HostJoint;

// Owns the native AObj/FObj instances used to play a materialized HSD
// animation. The caller supplies the explicit AnimJoint-to-HostJoint mapping;
// this avoids relying on GameCube addresses or traversal coincidences.
class HostAnimationPlayer {
public:
    ~HostAnimationPlayer();

    bool attach(const HostAnimation& animation, std::vector<HostJoint>& joints,
                const std::vector<uint32_t>& joint_mapping);
    void request(float frame);
    // Mirrors lb_80011E24 + HSD_JObjReqAnimAll: traversal_index is the
    // pre-order index used throughout Melee's menu code, not our storage
    // vector index. The selected joint and all of its descendants are reset.
    bool request_subtree(uint32_t traversal_index, float frame);
    void tick();
    void clear();

private:
    struct Playback {
        HSD_AObj* object = nullptr;
        HostJoint* joint = nullptr;
        std::vector<std::vector<uint8_t>> bytecode;
    };
    std::vector<Playback> playback_;
    std::vector<int32_t> playback_by_joint_;
    std::vector<int32_t> child_by_joint_;
    std::vector<int32_t> sibling_by_joint_;
    std::vector<uint32_t> preorder_joints_;
};

} // namespace meleeboard::hsd
