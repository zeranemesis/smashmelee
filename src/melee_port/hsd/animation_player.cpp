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
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();
    for (size_t index = 0; index < animation.joints().size(); ++index) {
        const auto& source = animation.joints()[index];
        if (!source.has_object) continue;
        if (joint_mapping[index] >= joints.size()) { clear(); return false; }
        playback_.emplace_back();
        Playback& target = playback_.back();
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

void HostAnimationPlayer::tick()
{
    for (Playback& entry : playback_)
        HSD_AObjInterpretAnim(entry.object, entry.joint, update_joint);
}

void HostAnimationPlayer::clear()
{
    for (Playback& entry : playback_) HSD_AObjRemove(entry.object);
    playback_.clear();
}

} // namespace meleeboard::hsd
