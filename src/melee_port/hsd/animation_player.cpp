#include "animation_player.hpp"

#include "animation.hpp"
#include "scene.hpp"

#include <melee/sysdolphin/baselib/aobj.h>

#include <cmath>

namespace meleeboard::hsd {

namespace {

constexpr uint32_t kJointHidden = 1U << 4;

void update_joint(void* object, uint32_t type, HSD_ObjData* value)
{
    auto* joint = static_cast<HostJoint*>(object);
    if (joint == nullptr || value == nullptr || !std::isfinite(value->fv)) {
        return;
    }
    switch (type) {
    case 1: joint->rotation[0] = value->fv; break;
    case 2: joint->rotation[1] = value->fv; break;
    case 3: joint->rotation[2] = value->fv; break;
    case 5: joint->translation[0] = value->fv; break;
    case 6: joint->translation[1] = value->fv; break;
    case 7: joint->translation[2] = value->fv; break;
    case 8: joint->scale[0] = std::fabs(value->fv) < 1e-3F ? 1e-3F : value->fv; break;
    case 9: joint->scale[1] = std::fabs(value->fv) < 1e-3F ? 1e-3F : value->fv; break;
    case 10: joint->scale[2] = std::fabs(value->fv) < 1e-3F ? 1e-3F : value->fv; break;
    case 11:
        if (value->fv > 0.5F) joint->flags &= ~kJointHidden;
        else joint->flags |= kJointHidden;
        break;
    default: break; // Path, branch, constraints, and callbacks need their HSD subsystems.
    }
}

} // namespace

HostAnimationPlayer::~HostAnimationPlayer() { clear(); }

bool HostAnimationPlayer::attach(const HostAnimation& animation,
                                 std::vector<HostJoint>& joints,
                                 const std::vector<uint32_t>& joint_mapping)
{
    clear();
    if (joint_mapping.size() != animation.joints().size()) return false;
    playback_by_joint_.assign(animation.joints().size(), -1);
    child_by_joint_.resize(animation.joints().size());
    sibling_by_joint_.resize(animation.joints().size());
    for (size_t index = 0; index < animation.joints().size(); ++index) {
        child_by_joint_[index] = animation.joints()[index].child;
        sibling_by_joint_[index] = animation.joints()[index].sibling;
    }
    if (!animation.joints().empty()) {
        std::vector<uint32_t> pending{ 0 };
        std::vector<bool> visited(animation.joints().size(), false);
        while (!pending.empty()) {
            const uint32_t index = pending.back();
            pending.pop_back();
            if (index >= animation.joints().size() || visited[index]) {
                clear();
                return false;
            }
            visited[index] = true;
            preorder_joints_.push_back(index);
            const int32_t sibling = sibling_by_joint_[index];
            const int32_t child = child_by_joint_[index];
            if (sibling >= 0) pending.push_back(static_cast<uint32_t>(sibling));
            if (child >= 0) pending.push_back(static_cast<uint32_t>(child));
        }
        if (preorder_joints_.size() != animation.joints().size()) {
            clear();
            return false;
        }
    }
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();
    for (size_t index = 0; index < animation.joints().size(); ++index) {
        const auto& source = animation.joints()[index];
        if (!source.has_object) continue;
        if (joint_mapping[index] >= joints.size()) { clear(); return false; }
        playback_.emplace_back();
        Playback& target = playback_.back();
        playback_by_joint_[index] =
            static_cast<int32_t>(playback_.size() - 1);
        target.joint = &joints[joint_mapping[index]];
        target.object = HSD_AObjAlloc();
        if (target.object == nullptr) { clear(); return false; }
        target.bytecode.reserve(source.object.channels.size());
        HSD_FObj* first = nullptr;
        HSD_FObj** tail = &first;
        for (const HostAnimationChannel& channel : source.object.channels) {
            target.bytecode.push_back(channel.bytecode);
            HSD_FObj* fobj = HSD_FObjAlloc();
            if (fobj == nullptr) { HSD_FObjRemoveAll(first); clear(); return false; }
            fobj->startframe = channel.start_frame;
            fobj->obj_type = channel.object_type;
            fobj->frac_value = channel.value_fraction;
            fobj->frac_slope = channel.slope_fraction;
            fobj->length = static_cast<uint32_t>(target.bytecode.back().size());
            fobj->ad_head = target.bytecode.back().data();
            *tail = fobj;
            tail = &fobj->next;
        }
        HSD_AObjSetFObj(target.object, first);
        HSD_AObjSetFlags(target.object, source.object.flags);
        HSD_AObjSetEndFrame(target.object, source.object.end_frame);
        HSD_AObjReqAnim(target.object, 0.0F);
    }
    return true;
}

void HostAnimationPlayer::request(float frame)
{
    for (Playback& entry : playback_) HSD_AObjReqAnim(entry.object, frame);
}

bool HostAnimationPlayer::request_subtree(uint32_t traversal_index, float frame)
{
    if (traversal_index >= preorder_joints_.size()) return false;
    std::vector<uint32_t> pending{ preorder_joints_[traversal_index] };
    std::vector<bool> visited(playback_by_joint_.size(), false);
    while (!pending.empty()) {
        const uint32_t index = pending.back();
        pending.pop_back();
        if (index >= visited.size() || visited[index]) return false;
        visited[index] = true;
        const int32_t playback = playback_by_joint_[index];
        if (playback >= 0) {
            HSD_AObjReqAnim(playback_[static_cast<size_t>(playback)].object, frame);
        }
        size_t sibling_count = 0;
        for (int32_t child = child_by_joint_[index]; child >= 0;
             child = sibling_by_joint_[static_cast<size_t>(child)]) {
            if (static_cast<size_t>(child) >= child_by_joint_.size() ||
                ++sibling_count > child_by_joint_.size()) return false;
            pending.push_back(static_cast<uint32_t>(child));
        }
    }
    return true;
}

void HostAnimationPlayer::tick()
{
    for (Playback& entry : playback_)
        HSD_AObjInterpretAnim(entry.object, entry.joint, update_joint);
}

void HostAnimationPlayer::clear()
{
    for (Playback& entry : playback_) HSD_AObjRemove(entry.object);
    playback_.clear();
    playback_by_joint_.clear();
    child_by_joint_.clear();
    sibling_by_joint_.clear();
    preorder_joints_.clear();
}

} // namespace meleeboard::hsd
