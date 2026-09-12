#include "animation.hpp"

#include <melee/sysdolphin/baselib/archive.hpp>

#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace meleeboard::hsd {

namespace {

constexpr uint32_t kAnimJointSize = 0x14;
constexpr uint32_t kAObjDescSize = 0x10;
constexpr uint32_t kFObjDescSize = 0x14;
constexpr size_t kMaxAnimationJoints = 8192;
constexpr size_t kMaxAnimationChannels = 8192;

bool copy_bytecode(const Archive& archive, uint32_t offset, uint32_t length,
                   std::vector<uint8_t>& destination)
{
    if (!archive.contains_data_range(offset, length)) {
        return false;
    }
    destination.resize(length);
    for (uint32_t index = 0; index < length; ++index) {
        const auto byte = archive.data_byte(offset + index);
        if (!byte.has_value()) {
            return false;
        }
        destination[index] = *byte;
    }
    return true;
}

bool load_aobj(const Archive& archive, uint32_t offset, HostAnimationObject& object)
{
    if (!archive.contains_data_range(offset, kAObjDescSize)) {
        return false;
    }
    const auto flags = archive.data_word(offset);
    const auto end_frame = archive.data_float(offset + 0x04);
    const auto first_channel = archive.data_pointer(offset + 0x08);
    const auto object_id = archive.data_word(offset + 0x0C);
    if (!flags.has_value() || !end_frame.has_value() || !first_channel.has_value() ||
        !object_id.has_value() || !std::isfinite(*end_frame)) {
        return false;
    }
    object.flags = *flags;
    object.end_frame = *end_frame;
    object.object_id = *object_id;

    uint32_t channel_offset = *first_channel;
    std::unordered_set<uint32_t> visited;
    while (channel_offset != 0) {
        if (object.channels.size() >= kMaxAnimationChannels ||
            !visited.insert(channel_offset).second ||
            !archive.contains_data_range(channel_offset, kFObjDescSize)) {
            return false;
        }
        const auto next = archive.data_pointer(channel_offset);
        const auto length = archive.data_word(channel_offset + 0x04);
        const auto start = archive.data_float(channel_offset + 0x08);
        const auto type = archive.data_byte(channel_offset + 0x0C);
        const auto value_fraction = archive.data_byte(channel_offset + 0x0D);
        const auto slope_fraction = archive.data_byte(channel_offset + 0x0E);
        const auto bytecode = archive.data_pointer(channel_offset + 0x10);
        if (!next.has_value() || !length.has_value() || !start.has_value() ||
            !type.has_value() || !value_fraction.has_value() ||
            !slope_fraction.has_value() || !bytecode.has_value() ||
            !std::isfinite(*start) ||
            *start < static_cast<float>(std::numeric_limits<int16_t>::min()) ||
            *start > static_cast<float>(std::numeric_limits<int16_t>::max())) {
            return false;
        }
        HostAnimationChannel channel{};
        channel.source_offset = channel_offset;
        channel.start_frame = static_cast<int16_t>(*start);
        channel.object_type = *type;
        channel.value_fraction = *value_fraction;
        channel.slope_fraction = *slope_fraction;
        if (!copy_bytecode(archive, *bytecode, *length, channel.bytecode)) {
            return false;
        }
        object.channels.push_back(std::move(channel));
        channel_offset = *next;
    }
    return true;
}

} // namespace

bool HostAnimation::load(const Archive& archive, std::string_view symbol)
{
    joints_.clear();
    last_error_.clear();
    const auto root = archive.public_symbol_offset(symbol);
    if (!root.has_value()) {
        last_error_ = "animation symbol is missing";
        return false;
    }

    std::vector<uint32_t> pending{ *root };
    std::unordered_map<uint32_t, uint32_t> indices;
    while (!pending.empty()) {
        const uint32_t offset = pending.back();
        pending.pop_back();
        if (indices.contains(offset)) {
            continue;
        }
        if (joints_.size() >= kMaxAnimationJoints ||
            !archive.contains_data_range(offset, kAnimJointSize)) {
            last_error_ = "animation joint is malformed or exceeds host limit";
            joints_.clear();
            return false;
        }
        const auto child = archive.data_pointer(offset);
        const auto sibling = archive.data_pointer(offset + 0x04);
        const auto object = archive.data_pointer(offset + 0x08);
        const auto flags = archive.data_word(offset + 0x10);
        if (!child.has_value() || !sibling.has_value() || !object.has_value() ||
            !flags.has_value()) {
            last_error_ = "animation joint contains an unsafe pointer";
            joints_.clear();
            return false;
        }
        HostAnimationJoint joint{};
        joint.source_offset = offset;
        joint.flags = *flags;
        joint.has_object = *object != 0;
        if (joint.has_object && !load_aobj(archive, *object, joint.object)) {
            last_error_ = "animation object or FObj descriptor is malformed";
            joints_.clear();
            return false;
        }
        indices.emplace(offset, static_cast<uint32_t>(joints_.size()));
        joints_.push_back(std::move(joint));
        if (*child != 0) {
            pending.push_back(*child);
        }
        if (*sibling != 0) {
            pending.push_back(*sibling);
        }
    }

    for (HostAnimationJoint& joint : joints_) {
        const auto child = archive.data_pointer(joint.source_offset);
        const auto sibling = archive.data_pointer(joint.source_offset + 0x04);
        if (!child.has_value() || !sibling.has_value()) {
            last_error_ = "animation relationship is malformed";
            joints_.clear();
            return false;
        }
        if (*child != 0) {
            const auto iterator = indices.find(*child);
            if (iterator == indices.end()) {
                last_error_ = "animation child is outside hierarchy";
                joints_.clear();
                return false;
            }
            joint.child = static_cast<int32_t>(iterator->second);
        }
        if (*sibling != 0) {
            const auto iterator = indices.find(*sibling);
            if (iterator == indices.end()) {
                last_error_ = "animation sibling is outside hierarchy";
                joints_.clear();
                return false;
            }
            joint.sibling = static_cast<int32_t>(iterator->second);
        }
    }
    return true;
}

const std::vector<HostAnimationJoint>& HostAnimation::joints() const
{
    return joints_;
}

const std::string& HostAnimation::last_error() const
{
    return last_error_;
}

} // namespace meleeboard::hsd
