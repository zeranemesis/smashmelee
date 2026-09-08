#include "scene.hpp"

#include <melee/sysdolphin/baselib/archive.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace meleeboard::hsd {

namespace {

constexpr uint32_t kMaxSceneJoints = 8192;
constexpr uint32_t kMaxSceneDrawObjects = 32768;

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
    draw_objects_.clear();
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

    for (HostJoint& joint : joints_) {
        const auto first = archive.data_word(joint.source_offset + 0x10);
        if (!first.has_value()) {
            return false;
        }

        uint32_t description = *first;
        std::unordered_set<uint32_t> chain;
        int32_t previous_draw_object = -1;
        while (description != 0) {
            if (!chain.insert(description).second ||
                draw_objects_.size() >= kMaxSceneDrawObjects) {
                return false;
            }
            const auto next = archive.data_word(description + 0x04);
            const auto material = archive.data_word(description + 0x08);
            const auto primitive = archive.data_word(description + 0x0C);
            if (!next.has_value() || !material.has_value() ||
                !primitive.has_value()) {
                return false;
            }

            HostDrawObject object{};
            object.source_offset = description;
            object.material_description = *material;
            object.primitive_description = *primitive;

            if (*material != 0) {
                const auto render_mode = archive.data_word(*material + 0x04);
                const auto texture_description =
                    archive.data_word(*material + 0x08);
                const auto material_data = archive.data_word(*material + 0x0C);
                if (!render_mode.has_value() ||
                    !texture_description.has_value() ||
                    !material_data.has_value()) {
                    return false;
                }
                object.render_mode = *render_mode;
                object.texture_description = *texture_description;
                object.material = *material_data;
            }

            if (*primitive != 0) {
                const auto vertex_description =
                    archive.data_word(*primitive + 0x08);
                const auto flags_and_display_count =
                    archive.data_word(*primitive + 0x0C);
                const auto display_list = archive.data_word(*primitive + 0x10);
                if (!vertex_description.has_value() ||
                    !flags_and_display_count.has_value() ||
                    !display_list.has_value()) {
                    return false;
                }
                object.vertex_description = *vertex_description;
                object.primitive_flags =
                    static_cast<uint16_t>(*flags_and_display_count >> 16);
                object.display_list_count =
                    static_cast<uint16_t>(*flags_and_display_count);
                object.display_list = *display_list;
            }
            const int32_t object_index =
                static_cast<int32_t>(draw_objects_.size());
            draw_objects_.push_back(object);
            if (previous_draw_object < 0) {
                joint.first_draw_object = object_index;
            } else {
                draw_objects_[static_cast<size_t>(previous_draw_object)].next =
                    object_index;
            }
            previous_draw_object = object_index;
            description = *next;
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

const std::vector<HostDrawObject>& HostScene::draw_objects() const
{
    return draw_objects_;
}

const std::vector<uint32_t>& HostScene::model_roots() const
{
    return model_roots_;
}

} // namespace meleeboard::hsd
