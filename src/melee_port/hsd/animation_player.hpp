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
    void tick();
    void clear();

private:
    struct Playback {
        HSD_AObj* object = nullptr;
        HostJoint* joint = nullptr;
        std::vector<std::vector<uint8_t>> bytecode;
    };
    std::vector<Playback> playback_;
};

} // namespace meleeboard::hsd
