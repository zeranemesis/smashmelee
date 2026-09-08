#include "scene.hpp"

#include <melee/sysdolphin/baselib/archive.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace meleeboard::hsd {

namespace {

constexpr uint32_t kMaxSceneJoints = 8192;
constexpr uint32_t kMaxSceneDrawObjects = 32768;
constexpr uint32_t kMaxVertexDescriptors = 32;
constexpr uint32_t kVertexDescriptorSize = 0x18;
constexpr uint8_t kGxOpcodeMask = 0xF8;
constexpr uint8_t kGxQuads = 0x80;
constexpr uint8_t kGxTriangles = 0x90;
constexpr uint8_t kGxTriangleStrip = 0x98;
constexpr uint8_t kGxTriangleFan = 0xA0;
constexpr uint8_t kGxLines = 0xA8;
constexpr uint8_t kGxLineStrip = 0xB0;
constexpr uint8_t kGxPoints = 0xB8;
constexpr uint32_t kGxDirect = 1;
constexpr uint32_t kGxIndex8 = 2;
constexpr uint32_t kGxIndex16 = 3;

bool parse_display_list(const Archive& archive, HostDrawObject& object)
{
    uint32_t bytes_per_vertex = 0;
    for (const HostVertexDescriptor& descriptor : object.vertex_descriptors) {
        switch (descriptor.attribute_type) {
        case 0:
            break;
        case kGxIndex8:
            ++bytes_per_vertex;
            break;
        case kGxIndex16:
            bytes_per_vertex += 2;
            break;
        case kGxDirect:
            // Matrix indices are the only direct fields emitted by the HSD
            // model lists handled here; each occupies one byte in GX FIFO.
            if (descriptor.attribute > 8) {
                return false;
            }
            ++bytes_per_vertex;
            break;
        default:
            return false;
        }
    }

    const uint32_t display_bytes =
        static_cast<uint32_t>(object.display_list_count) << 5;
    uint32_t cursor = 0;
    while (cursor < display_bytes) {
        const auto command = archive.data_byte(object.display_list + cursor++);
        if (!command.has_value()) {
            return false;
        }
        if (*command == 0) {
            continue;
        }
        if (display_bytes - cursor < 2) {
            return false;
        }
        const auto high = archive.data_byte(object.display_list + cursor++);
        const auto low = archive.data_byte(object.display_list + cursor++);
        if (!high.has_value() || !low.has_value()) {
            return false;
        }
        const uint32_t vertices =
            (static_cast<uint32_t>(*high) << 8) | *low;
        if (bytes_per_vertex != 0 &&
            vertices > (display_bytes - cursor) / bytes_per_vertex) {
            return false;
        }
        const uint8_t primitive = *command & kGxOpcodeMask;
        switch (primitive) {
        case kGxQuads:
            if (vertices % 4 != 0) {
                return false;
            }
            object.triangle_count += (vertices / 4) * 2;
            break;
        case kGxTriangles:
            if (vertices % 3 != 0) {
                return false;
            }
            object.triangle_count += vertices / 3;
            break;
        case kGxTriangleStrip:
        case kGxTriangleFan:
            object.triangle_count += vertices >= 3 ? vertices - 2 : 0;
            break;
        case kGxLines:
        case kGxLineStrip:
        case kGxPoints:
            break;
        default:
            return false;
        }
        object.vertex_count += vertices;
        ++object.primitive_batch_count;
        cursor += vertices * bytes_per_vertex;
    }
    return true;
}

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

                const uint32_t display_bytes =
                    static_cast<uint32_t>(object.display_list_count) << 5;
                if ((display_bytes != 0 && object.display_list == 0) ||
                    !archive.contains_data_range(object.display_list,
                                                 display_bytes)) {
                    return false;
                }

                bool descriptors_terminated = object.vertex_description == 0;
                for (uint32_t index = 0;
                     object.vertex_description != 0 &&
                     index < kMaxVertexDescriptors; ++index) {
                    const uint32_t descriptor = object.vertex_description +
                        index * kVertexDescriptorSize;
                    const auto attribute = archive.data_word(descriptor);
                    if (!attribute.has_value()) {
                        return false;
                    }
                    if (*attribute == 0) {
                        descriptors_terminated = true;
                        break;
                    }
                    const auto attribute_type =
                        archive.data_word(descriptor + 0x04);
                    const auto component_count =
                        archive.data_word(descriptor + 0x08);
                    const auto component_type =
                        archive.data_word(descriptor + 0x0C);
                    const auto fraction_and_stride =
                        archive.data_word(descriptor + 0x10);
                    const auto vertex_data =
                        archive.data_word(descriptor + 0x14);
                    if (!attribute_type.has_value() ||
                        !component_count.has_value() ||
                        !component_type.has_value() ||
                        !fraction_and_stride.has_value() ||
                        !vertex_data.has_value()) {
                        return false;
                    }
                    object.vertex_descriptors.push_back({
                        .attribute = *attribute,
                        .attribute_type = *attribute_type,
                        .component_count = *component_count,
                        .component_type = *component_type,
                        .vertex_data = *vertex_data,
                        .stride = static_cast<uint16_t>(*fraction_and_stride),
                        .fraction = static_cast<uint8_t>(
                            *fraction_and_stride >> 24),
                    });
                }
                if (!descriptors_terminated) {
                    return false;
                }
                if (!parse_display_list(archive, object)) {
                    return false;
                }
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
