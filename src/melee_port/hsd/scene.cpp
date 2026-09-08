#include "scene.hpp"

#include <melee/sysdolphin/baselib/archive.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace meleeboard::hsd {

namespace {

constexpr uint32_t kMaxSceneJoints = 8192;
constexpr uint32_t kMaxSceneDrawObjects = 32768;
constexpr uint32_t kMaxVertexDescriptors = 32;
constexpr uint32_t kVertexDescriptorSize = 0x18;
constexpr uint32_t kGxVertexAttributeNull = 0xFF;
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
    uint32_t position_index_offset = UINT32_MAX;
    uint32_t position_index_width = 0;
    for (const HostVertexDescriptor& descriptor : object.vertex_descriptors) {
        uint32_t attribute_bytes = 0;
        switch (descriptor.attribute_type) {
        case 0:
            break;
        case kGxIndex8:
            attribute_bytes = 1;
            break;
        case kGxIndex16:
            attribute_bytes = 2;
            break;
        case kGxDirect:
            // Matrix indices are the only direct fields emitted by the HSD
            // model lists handled here; each occupies one byte in GX FIFO.
            if (descriptor.attribute > 8) {
                return false;
            }
            attribute_bytes = 1;
            break;
        default:
            return false;
        }
        if (descriptor.attribute == 9) { // GX_VA_POS
            if (descriptor.attribute_type != kGxIndex8 &&
                descriptor.attribute_type != kGxIndex16) {
                return false;
            }
            position_index_offset = bytes_per_vertex;
            position_index_width = attribute_bytes;
        }
        bytes_per_vertex += attribute_bytes;
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
        if (vertices != 0 && position_index_offset == UINT32_MAX) {
            return false;
        }

        std::vector<uint32_t> position_indices;
        position_indices.reserve(vertices);
        for (uint32_t vertex = 0; vertex < vertices; ++vertex) {
            const uint32_t position_offset = cursor +
                vertex * bytes_per_vertex + position_index_offset;
            const auto first = archive.data_byte(object.display_list +
                                                 position_offset);
            if (!first.has_value()) {
                return false;
            }
            uint32_t position_index = *first;
            if (position_index_width == 2) {
                const auto second = archive.data_byte(object.display_list +
                                                      position_offset + 1);
                if (!second.has_value()) {
                    return false;
                }
                position_index = (position_index << 8) | *second;
            }
            position_indices.push_back(position_index);
        }
        const auto append_triangle = [&object, &position_indices](uint32_t a,
                                                                   uint32_t b,
                                                                   uint32_t c) {
            object.triangle_position_indices.push_back(position_indices[a]);
            object.triangle_position_indices.push_back(position_indices[b]);
            object.triangle_position_indices.push_back(position_indices[c]);
        };
        const uint8_t primitive = *command & kGxOpcodeMask;
        switch (primitive) {
        case kGxQuads:
            if (vertices % 4 != 0) {
                return false;
            }
            object.triangle_count += (vertices / 4) * 2;
            for (uint32_t index = 0; index < vertices; index += 4) {
                append_triangle(index, index + 1, index + 2);
                append_triangle(index, index + 2, index + 3);
            }
            break;
        case kGxTriangles:
            if (vertices % 3 != 0) {
                return false;
            }
            object.triangle_count += vertices / 3;
            for (uint32_t index = 0; index < vertices; index += 3) {
                append_triangle(index, index + 1, index + 2);
            }
            break;
        case kGxTriangleStrip:
            object.triangle_count += vertices >= 3 ? vertices - 2 : 0;
            for (uint32_t index = 2; index < vertices; ++index) {
                if (index % 2 == 0) {
                    append_triangle(index - 2, index - 1, index);
                } else {
                    append_triangle(index - 1, index - 2, index);
                }
            }
            break;
        case kGxTriangleFan:
            object.triangle_count += vertices >= 3 ? vertices - 2 : 0;
            for (uint32_t index = 2; index < vertices; ++index) {
                append_triangle(0, index - 1, index);
            }
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

bool read_position(const Archive& archive,
                   const HostVertexDescriptor& descriptor, uint32_t index,
                   std::array<float, 3>& position, std::string& error)
{
    if (descriptor.attribute != 9) {
        error = "position descriptor has a non-position attribute";
        return false;
    }
    if (descriptor.vertex_data == 0) {
        error = "position descriptor has no vertex array";
        return false;
    }
    if (descriptor.component_count > 1) {
        error = "position descriptor has an unknown component count";
        return false;
    }

    uint32_t component_size = 0;
    switch (descriptor.component_type) {
    case 0: // GX_U8
    case 1: // GX_S8
        component_size = 1;
        break;
    case 2: // GX_U16
    case 3: // GX_S16
        component_size = 2;
        break;
    case 4: // GX_F32
        component_size = 4;
        break;
    default:
        error = "position descriptor has an unsupported component type";
        return false;
    }
    const uint32_t component_count = descriptor.component_count == 0 ? 2 : 3;
    if (descriptor.stride < component_count * component_size) {
        error = "position descriptor stride is shorter than its components";
        return false;
    }
    const uint64_t offset = static_cast<uint64_t>(descriptor.vertex_data) +
        static_cast<uint64_t>(index) * descriptor.stride;
    const uint64_t bytes = component_count * component_size;
    if (offset > std::numeric_limits<uint32_t>::max() ||
        !archive.contains_data_range(static_cast<uint32_t>(offset),
                                     static_cast<uint32_t>(bytes))) {
        error = "position index points outside the HSD vertex array";
        return false;
    }

    position = { 0.0F, 0.0F, 0.0F };
    for (uint32_t component = 0; component < component_count; ++component) {
        const uint32_t component_offset = static_cast<uint32_t>(offset) +
            component * component_size;
        float value = 0;
        if (descriptor.component_type == 4) {
            const auto floating = archive.data_float(component_offset);
            if (!floating.has_value()) {
                error = "floating-point position lies outside the HSD data";
                return false;
            }
            value = *floating;
        } else {
            const auto first = archive.data_byte(component_offset);
            if (!first.has_value()) {
                error = "integer position lies outside the HSD data";
                return false;
            }
            int32_t integer = *first;
            if (component_size == 2) {
                const auto second = archive.data_byte(component_offset + 1);
                if (!second.has_value()) {
                    error = "16-bit position lies outside the HSD data";
                    return false;
                }
                integer = (integer << 8) | *second;
                if (descriptor.component_type == 3) {
                    integer = static_cast<int16_t>(integer);
                }
            } else if (descriptor.component_type == 1) {
                integer = static_cast<int8_t>(integer);
            }
            value = std::ldexp(static_cast<float>(integer),
                               -static_cast<int>(descriptor.fraction));
        }
        position[component] = value;
    }
    return true;
}

bool materialize_positions(const Archive& archive, HostDrawObject& object,
                           std::string& error)
{
    if (object.triangle_position_indices.empty()) {
        return true;
    }
    const auto descriptor = std::find_if(
        object.vertex_descriptors.begin(), object.vertex_descriptors.end(),
        [](const HostVertexDescriptor& candidate) {
            return candidate.attribute == 9;
        });
    if (descriptor == object.vertex_descriptors.end()) {
        error = "GX stream has no position descriptor";
        return false;
    }

    if (descriptor->attribute_type != kGxIndex8 &&
        descriptor->attribute_type != kGxIndex16) {
        error = "position descriptor is not index-addressed";
        return false;
    }

    std::unordered_map<uint32_t, uint32_t> host_indices;
    for (uint32_t source_index : object.triangle_position_indices) {
        const auto existing = host_indices.find(source_index);
        if (existing != host_indices.end()) {
            object.triangle_indices.push_back(existing->second);
            continue;
        }
        std::array<float, 3> position{};
        if (!read_position(archive, *descriptor, source_index, position,
                           error)) {
            return false;
        }
        const uint32_t host_index =
            static_cast<uint32_t>(object.positions.size());
        object.positions.push_back(position);
        host_indices.emplace(source_index, host_index);
        object.triangle_indices.push_back(host_index);
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
    last_error_ = "could not resolve HSD scene roots";

    const auto scene = archive.scene_roots(symbol);
    const auto model_count = archive.scene_model_count(symbol);
    if (!scene.has_value() || !model_count.has_value()) {
        return false;
    }

    last_error_ = "could not build HSD joint hierarchy";
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

    last_error_ = "could not link HSD joint hierarchy";
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

    last_error_ = "could not decode HSD draw objects";
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
                    if (*attribute == kGxVertexAttributeNull) {
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
                    last_error_ = "unsupported or malformed GX display stream";
                    return false;
                }
                std::string position_error;
                if (materialize_positions(archive, object, position_error)) {
                    object.position_stream_decoded = true;
                } else {
                    // An individual legacy vertex layout must not prevent the
                    // rest of a scene from loading.  The host renderer can
                    // skip this object until its layout is implemented.
                    object.positions.clear();
                    object.triangle_indices.clear();
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

    last_error_ = "could not resolve HSD model roots";
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
    last_error_.clear();
    return true;
}

const std::string& HostScene::last_error() const
{
    return last_error_;
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
