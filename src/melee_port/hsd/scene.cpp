#include "scene.hpp"

#include <melee/sysdolphin/baselib/archive.hpp>

#include <unordered_map>
#include <vector>

namespace meleeboard::hsd {

namespace {

constexpr uint32_t kMaxSceneJoints = 8192;

bool read_transform(const Archive& archive, uint32_t offset,
                    std::array<float, 3>& destination)
{
    for (uint32_t index = 0; index < destination.size(); ++index) {
        const auto value = archive.data_float(offset + index * sizeof(float));
        if (!value.has_value()) {
            return false;
        }
        destination[index] = *value;
    }
    return true;
}

} // namespace

bool HostScene::load(const Archive& archive, std::string_view symbol)
{
    joints_.clear();
    model_roots_.clear();

    const auto scene = archive.scene_roots(symbol);
    const auto model_count = archive.scene_model_count(symbol);
    if (!scene.has_value() || !model_count.has_value()) {
        return false;
    }

    std::vector<uint32_t> pending;
    for (uint32_t index = 0; index < *model_count; ++index) {
        const auto model = archive.data_word(scene->models +
                                             index * sizeof(uint32_t));
        if (!model.has_value()) {
            return false;
        }
        const auto root = archive.data_word(*model);
        if (!root.has_value()) {
            return false;
        }
        model_roots_.push_back(*root);
        if (*root != 0) {
            pending.push_back(*root);
        }
    }

    std::unordered_map<uint32_t, uint32_t> joint_indices;
    while (!pending.empty()) {
        const uint32_t offset = pending.back();
        pending.pop_back();
        if (joint_indices.contains(offset)) {
            continue;
        }
        if (joints_.size() >= kMaxSceneJoints) {
            return false;
        }

        const auto flags = archive.data_word(offset + 0x04);
        const auto child = archive.data_word(offset + 0x08);
        const auto sibling = archive.data_word(offset + 0x0C);
        if (!flags.has_value() || !child.has_value() || !sibling.has_value()) {
            return false;
        }

        HostJoint joint{};
        joint.source_offset = offset;
        joint.flags = *flags;
        if (!read_transform(archive, offset + 0x14, joint.rotation) ||
            !read_transform(archive, offset + 0x20, joint.scale) ||
            !read_transform(archive, offset + 0x2C, joint.translation)) {
            return false;
        }

        joint_indices.emplace(offset, static_cast<uint32_t>(joints_.size()));
        joints_.push_back(joint);
        if (*child != 0) {
            pending.push_back(*child);
        }
        if (*sibling != 0) {
            pending.push_back(*sibling);
        }
    }

    for (HostJoint& joint : joints_) {
        const auto child = archive.data_word(joint.source_offset + 0x08);
        const auto sibling = archive.data_word(joint.source_offset + 0x0C);
        if (!child.has_value() || !sibling.has_value()) {
            return false;
        }
        if (*child != 0) {
            const auto iterator = joint_indices.find(*child);
            if (iterator == joint_indices.end()) {
                return false;
            }
            joint.child = static_cast<int32_t>(iterator->second);
        }
        if (*sibling != 0) {
            const auto iterator = joint_indices.find(*sibling);
            if (iterator == joint_indices.end()) {
                return false;
            }
            joint.sibling = static_cast<int32_t>(iterator->second);
        }
    }

    for (uint32_t& root : model_roots_) {
        if (root == 0) {
            root = UINT32_MAX;
            continue;
        }
        const auto iterator = joint_indices.find(root);
        if (iterator == joint_indices.end()) {
            return false;
        }
        root = iterator->second;
    }
    return true;
}

const std::vector<HostJoint>& HostScene::joints() const
{
    return joints_;
}

const std::vector<uint32_t>& HostScene::model_roots() const
{
    return model_roots_;
}

} // namespace meleeboard::hsd
