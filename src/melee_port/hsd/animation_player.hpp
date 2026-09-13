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
    HostAnimationPlayer() = default;
    HostAnimationPlayer(const HostAnimationPlayer&) = delete;
    HostAnimationPlayer& operator=(const HostAnimationPlayer&) = delete;
    ~HostAnimationPlayer();

    bool attach(const HostAnimation& animation, std::vector<HostJoint>& joints,
                const std::vector<uint32_t>& joint_mapping);
    void request(float frame);
    bool request_joint(uint32_t traversal_index, float frame);
    // Mirrors lb_80011E24 + HSD_JObjReqAnimAll: traversal_index is the
    // pre-order index used throughout Melee's menu code, not our storage
    // vector index. The selected joint and all of its descendants are reset.
    bool request_subtree(uint32_t traversal_index, float frame);
    bool set_subtree_hidden(uint32_t traversal_index, bool hidden);
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
    std::vector<HostJoint*> joints_by_animation_;
    std::vector<uint32_t> base_flags_by_animation_;
};

} // namespace meleeboard::hsd
