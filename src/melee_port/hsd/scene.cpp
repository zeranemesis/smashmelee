#include "scene.hpp"

#include <melee/sysdolphin/baselib/archive.hpp>

#include <dolphin/gx/GXTexture.h>

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
constexpr uint32_t kMaxSceneCameras = 16;
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
constexpr uint32_t kGxPosition = 9;
constexpr uint32_t kGxNormal = 10;
constexpr uint32_t kGxTex0 = 13;

bool parse_display_list(const Archive& archive, HostDrawObject& object)
{
    uint32_t bytes_per_vertex = 0;
    uint32_t position_index_offset = UINT32_MAX;
    uint32_t position_index_width = 0;
    uint32_t normal_index_offset = UINT32_MAX;
    uint32_t normal_index_width = 0;
    uint32_t texcoord_index_offset = UINT32_MAX;
    uint32_t texcoord_index_width = 0;
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
        if (descriptor.attribute == kGxPosition) {
            if (descriptor.attribute_type != kGxIndex8 &&
                descriptor.attribute_type != kGxIndex16) {
                return false;
            }
            position_index_offset = bytes_per_vertex;
            position_index_width = attribute_bytes;
        }
        if (descriptor.attribute == kGxNormal &&
            (descriptor.attribute_type == kGxIndex8 ||
             descriptor.attribute_type == kGxIndex16)) {
            normal_index_offset = bytes_per_vertex;
            normal_index_width = attribute_bytes;
        }
        if (descriptor.attribute == kGxTex0 &&
            (descriptor.attribute_type == kGxIndex8 ||
             descriptor.attribute_type == kGxIndex16)) {
            texcoord_index_offset = bytes_per_vertex;
            texcoord_index_width = attribute_bytes;
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
        std::vector<uint32_t> normal_indices;
        std::vector<uint32_t> texcoord_indices;
        position_indices.reserve(vertices);
        normal_indices.reserve(vertices);
        texcoord_indices.reserve(vertices);
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
            uint32_t normal_index = UINT32_MAX;
            if (normal_index_offset != UINT32_MAX) {
                const auto normal_first = archive.data_byte(object.display_list +
                    cursor + vertex * bytes_per_vertex + normal_index_offset);
                if (!normal_first.has_value()) {
                    return false;
                }
                normal_index = *normal_first;
                if (normal_index_width == 2) {
                    const auto normal_second = archive.data_byte(object.display_list +
                        cursor + vertex * bytes_per_vertex + normal_index_offset + 1);
                    if (!normal_second.has_value()) {
                        return false;
                    }
                    normal_index = (normal_index << 8) | *normal_second;
                }
            }
            normal_indices.push_back(normal_index);
            uint32_t texcoord_index = UINT32_MAX;
            if (texcoord_index_offset != UINT32_MAX) {
                const auto texcoord_first = archive.data_byte(object.display_list +
                    cursor + vertex * bytes_per_vertex + texcoord_index_offset);
                if (!texcoord_first.has_value()) return false;
                texcoord_index = *texcoord_first;
                if (texcoord_index_width == 2) {
                    const auto texcoord_second = archive.data_byte(object.display_list +
                        cursor + vertex * bytes_per_vertex + texcoord_index_offset + 1);
                    if (!texcoord_second.has_value()) return false;
                    texcoord_index = (texcoord_index << 8) | *texcoord_second;
                }
            }
            texcoord_indices.push_back(texcoord_index);
        }
        const auto append_triangle = [&object, &position_indices, &normal_indices,
                                      &texcoord_indices](uint32_t a, uint32_t b,
                                                        uint32_t c) {
            object.triangle_position_indices.push_back(position_indices[a]);
            object.triangle_position_indices.push_back(position_indices[b]);
            object.triangle_position_indices.push_back(position_indices[c]);
            object.triangle_normal_indices.push_back(normal_indices[a]);
            object.triangle_normal_indices.push_back(normal_indices[b]);
            object.triangle_normal_indices.push_back(normal_indices[c]);
            object.triangle_texcoord_indices.push_back(texcoord_indices[a]);
            object.triangle_texcoord_indices.push_back(texcoord_indices[b]);
            object.triangle_texcoord_indices.push_back(texcoord_indices[c]);
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
    if (descriptor.attribute != kGxPosition) {
        error = "position descriptor has a non-position attribute";
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

bool read_normal(const Archive& archive, const HostVertexDescriptor& descriptor,
                 uint32_t index, std::array<float, 3>& normal,
                 std::string& error)
{
    if (descriptor.attribute != kGxNormal || descriptor.component_count > 1) {
        error = "normal descriptor has an unsupported component count";
        return false;
    }
    uint32_t component_size = 0;
    switch (descriptor.component_type) {
    case 0: case 1: component_size = 1; break;
    case 2: case 3: component_size = 2; break;
    case 4: component_size = 4; break;
    default: error = "normal descriptor has an unsupported component type"; return false;
    }
    if (descriptor.stride < component_size * 3) {
        error = "normal descriptor stride is shorter than its components";
        return false;
    }
    const uint64_t offset = static_cast<uint64_t>(descriptor.vertex_data) +
        static_cast<uint64_t>(index) * descriptor.stride;
    if (offset > std::numeric_limits<uint32_t>::max() ||
        !archive.contains_data_range(static_cast<uint32_t>(offset), component_size * 3)) {
        error = "normal index points outside the HSD vertex array";
        return false;
    }
    for (uint32_t component = 0; component < 3; ++component) {
        const uint32_t at = static_cast<uint32_t>(offset) + component * component_size;
        float value = 0;
        if (descriptor.component_type == 4) {
            const auto floating = archive.data_float(at);
            if (!floating.has_value()) { error = "floating-point normal lies outside the HSD data"; return false; }
            value = *floating;
        } else {
            const auto first = archive.data_byte(at);
            if (!first.has_value()) { error = "integer normal lies outside the HSD data"; return false; }
            int32_t integer = *first;
            if (component_size == 2) {
                const auto second = archive.data_byte(at + 1);
                if (!second.has_value()) { error = "16-bit normal lies outside the HSD data"; return false; }
                integer = (integer << 8) | *second;
                if (descriptor.component_type == 3) integer = static_cast<int16_t>(integer);
            } else if (descriptor.component_type == 1) {
                integer = static_cast<int8_t>(integer);
            }
            value = std::ldexp(static_cast<float>(integer), -static_cast<int>(descriptor.fraction));
        }
        normal[component] = value;
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

bool materialize_normals(const Archive& archive, HostDrawObject& object,
                         std::string& error)
{
    if (object.triangle_normal_indices.empty() ||
        std::all_of(object.triangle_normal_indices.begin(),
                    object.triangle_normal_indices.end(),
                    [](uint32_t index) { return index == UINT32_MAX; })) {
        return true;
    }
    const auto descriptor = std::find_if(object.vertex_descriptors.begin(),
        object.vertex_descriptors.end(), [](const HostVertexDescriptor& candidate) {
            return candidate.attribute == kGxNormal;
        });
    if (descriptor == object.vertex_descriptors.end() ||
        (descriptor->attribute_type != kGxIndex8 && descriptor->attribute_type != kGxIndex16)) {
        error = "normal stream has no indexed normal descriptor";
        return false;
    }
    std::unordered_map<uint32_t, uint32_t> host_indices;
    for (uint32_t& source_index : object.triangle_normal_indices) {
        if (source_index == UINT32_MAX) continue;
        const auto existing = host_indices.find(source_index);
        if (existing != host_indices.end()) { source_index = existing->second; continue; }
        std::array<float, 3> normal{};
        if (!read_normal(archive, *descriptor, source_index, normal, error)) return false;
        const uint32_t host_index = static_cast<uint32_t>(object.normals.size());
        object.normals.push_back(normal);
        host_indices.emplace(source_index, host_index);
        source_index = host_index;
    }
    return true;
}

bool materialize_texcoords(const Archive& archive, HostDrawObject& object,
                           std::string& error)
{
    if (object.triangle_texcoord_indices.empty() ||
        std::all_of(object.triangle_texcoord_indices.begin(),
                    object.triangle_texcoord_indices.end(),
                    [](uint32_t index) { return index == UINT32_MAX; })) return true;
    const auto descriptor = std::find_if(object.vertex_descriptors.begin(),
        object.vertex_descriptors.end(), [](const HostVertexDescriptor& candidate) {
            return candidate.attribute == kGxTex0;
        });
    if (descriptor == object.vertex_descriptors.end() ||
        (descriptor->attribute_type != kGxIndex8 && descriptor->attribute_type != kGxIndex16)) {
        error = "texture stream has no indexed TEX0 descriptor";
        return false;
    }
    uint32_t component_size = 0;
    switch (descriptor->component_type) {
    case 0: case 1: component_size = 1; break;
    case 2: case 3: component_size = 2; break;
    case 4: component_size = 4; break;
    default: error = "texture descriptor has an unsupported component type"; return false;
    }
    const uint32_t component_count = descriptor->component_count == 0 ? 1 : 2;
    if (descriptor->component_count > 1 || descriptor->stride < component_size * component_count) {
        error = "texture descriptor has an unsupported component count or stride";
        return false;
    }
    std::unordered_map<uint32_t, uint32_t> host_indices;
    for (uint32_t& source_index : object.triangle_texcoord_indices) {
        if (source_index == UINT32_MAX) continue;
        const auto existing = host_indices.find(source_index);
        if (existing != host_indices.end()) { source_index = existing->second; continue; }
        const uint64_t source_offset = static_cast<uint64_t>(descriptor->vertex_data) +
            static_cast<uint64_t>(source_index) * descriptor->stride;
        if (source_offset > std::numeric_limits<uint32_t>::max() ||
            !archive.contains_data_range(static_cast<uint32_t>(source_offset),
                                         component_size * component_count)) {
            error = "texture index points outside the HSD vertex array";
            return false;
        }
        std::array<float, 2> texcoord{};
        for (uint32_t component = 0; component < component_count; ++component) {
            const uint32_t at = static_cast<uint32_t>(source_offset) + component * component_size;
            if (descriptor->component_type == 4) {
                const auto value = archive.data_float(at);
                if (!value.has_value()) { error = "floating-point texture coordinate lies outside the HSD data"; return false; }
                texcoord[component] = *value;
            } else {
                const auto first = archive.data_byte(at);
                if (!first.has_value()) { error = "integer texture coordinate lies outside the HSD data"; return false; }
                int32_t value = *first;
                if (component_size == 2) {
                    const auto second = archive.data_byte(at + 1);
                    if (!second.has_value()) { error = "16-bit texture coordinate lies outside the HSD data"; return false; }
                    value = (value << 8) | *second;
                    if (descriptor->component_type == 3) value = static_cast<int16_t>(value);
                } else if (descriptor->component_type == 1) value = static_cast<int8_t>(value);
                texcoord[component] = std::ldexp(static_cast<float>(value), -static_cast<int>(descriptor->fraction));
            }
        }
        const uint32_t host_index = static_cast<uint32_t>(object.texcoords.size());
        object.texcoords.push_back(texcoord);
        host_indices.emplace(source_index, host_index);
        source_index = host_index;
    }
    return true;
}

bool materialize_primitive(const Archive& archive, uint32_t primitive,
                           HostDrawObject& object, std::string& error)
{
    const auto vertex_description = archive.data_pointer(primitive + 0x08);
    const auto flags_and_display_count = archive.data_word(primitive + 0x0C);
    const auto display_list = archive.data_pointer(primitive + 0x10);
    if (!vertex_description.has_value() || !flags_and_display_count.has_value() ||
        !display_list.has_value()) {
        error = "truncated HSD PObjDesc";
        return false;
    }
    object.primitive_description = primitive;
    object.vertex_description = *vertex_description;
    object.primitive_flags = static_cast<uint16_t>(*flags_and_display_count >> 16);
    object.display_list_count = static_cast<uint16_t>(*flags_and_display_count);
    object.display_list = *display_list;
    const uint32_t display_bytes = static_cast<uint32_t>(object.display_list_count) << 5;
    if (!archive.contains_data_range(object.display_list, display_bytes)) {
        error = "invalid HSD PObj display list";
        return false;
    }
    bool terminated = object.vertex_description == 0;
    for (uint32_t index = 0; object.vertex_description != 0 &&
         index < kMaxVertexDescriptors; ++index) {
        const uint32_t descriptor = object.vertex_description + index * kVertexDescriptorSize;
        const auto attribute = archive.data_word(descriptor);
        if (!attribute.has_value()) { error = "truncated HSD vertex descriptor"; return false; }
        if (*attribute == kGxVertexAttributeNull) { terminated = true; break; }
        const auto type = archive.data_word(descriptor + 0x04);
        const auto count = archive.data_word(descriptor + 0x08);
        const auto component = archive.data_word(descriptor + 0x0C);
        const auto fraction_stride = archive.data_word(descriptor + 0x10);
        const auto vertex_data = archive.data_pointer(descriptor + 0x14);
        if (!type || !count || !component || !fraction_stride || !vertex_data) {
            error = "truncated HSD vertex descriptor fields"; return false;
        }
        object.vertex_descriptors.push_back({
            .attribute = *attribute, .attribute_type = *type, .component_count = *count,
            .component_type = *component, .vertex_data = *vertex_data,
            .stride = static_cast<uint16_t>(*fraction_stride),
            .fraction = static_cast<uint8_t>(*fraction_stride >> 24),
        });
    }
    if (!terminated || !parse_display_list(archive, object)) {
        error = "unsupported or malformed GX display stream";
        return false;
    }
    std::string position_error;
    if (materialize_positions(archive, object, position_error)) {
        object.position_stream_decoded = true;
    } else {
        object.position_decode_error = std::move(position_error);
        object.positions.clear();
        object.triangle_indices.clear();
    }
    std::string normal_error;
    if (!materialize_normals(archive, object, normal_error)) {
        object.normals.clear();
        std::fill(object.triangle_normal_indices.begin(),
                  object.triangle_normal_indices.end(), UINT32_MAX);
    }
    std::string texcoord_error;
    if (!materialize_texcoords(archive, object, texcoord_error)) {
        object.texcoords.clear();
        std::fill(object.triangle_texcoord_indices.begin(),
                  object.triangle_texcoord_indices.end(), UINT32_MAX);
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

bool read_s16(const Archive& archive, uint32_t offset, int16_t& value)
{
    const auto high = archive.data_byte(offset);
    const auto low = archive.data_byte(offset + 1);
    if (!high.has_value() || !low.has_value()) {
        return false;
    }
    value = static_cast<int16_t>((static_cast<uint16_t>(*high) << 8) | *low);
    return true;
}

bool read_camera(const Archive& archive, uint32_t description, HostCamera& camera)
{
    const auto flags_projection = archive.data_word(description + 0x04);
    const auto eye_description = archive.data_pointer(description + 0x18);
    const auto interest_description = archive.data_pointer(description + 0x1C);
    const auto near_plane = archive.data_float(description + 0x28);
    const auto far_plane = archive.data_float(description + 0x2C);
    if (!flags_projection.has_value() || !eye_description.has_value() ||
        !interest_description.has_value() || !near_plane.has_value() ||
        !far_plane.has_value() || *eye_description == 0 ||
        *interest_description == 0 || *near_plane <= 0.0F ||
        *far_plane <= *near_plane) {
        return false;
    }

    camera.flags = static_cast<uint16_t>(*flags_projection >> 16);
    camera.projection_type = static_cast<uint16_t>(*flags_projection);
    for (uint32_t index = 0; index < camera.viewport.size(); ++index) {
        if (!read_s16(archive, description + 0x08 + index * 2,
                      camera.viewport[index])) {
            return false;
        }
    }
    for (uint32_t index = 0; index < camera.scissor.size(); ++index) {
        const auto high = archive.data_byte(description + 0x10 + index * 2);
        const auto low = archive.data_byte(description + 0x11 + index * 2);
        if (!high.has_value() || !low.has_value()) {
            return false;
        }
        camera.scissor[index] =
            static_cast<uint16_t>((static_cast<uint16_t>(*high) << 8) | *low);
    }
    if (!read_transform(archive, *eye_description + 0x04, camera.eye) ||
        !read_transform(archive, *interest_description + 0x04,
                        camera.interest)) {
        return false;
    }
    camera.near_plane = *near_plane;
    camera.far_plane = *far_plane;

    if (camera.flags & 1) {
        const auto up_vector = archive.data_pointer(description + 0x24);
        if (up_vector.has_value() && *up_vector != 0 &&
            !read_transform(archive, *up_vector, camera.up)) {
            return false;
        }
    }
    for (uint32_t index = 0; index < camera.projection.size(); ++index) {
        const auto value = archive.data_float(description + 0x30 + index * 4);
        if (!value.has_value()) {
            return false;
        }
        camera.projection[index] = *value;
    }
    return true;
}

bool read_material(const Archive& archive, uint32_t description,
                   uint32_t material_offset, HostMaterial& material)
{
    // HSD_MObjDesc is { class, rendermode, texdesc, mat, renderdesc, pedesc }.
    // HSD_Material is three GXColor values followed by alpha and shininess.
    const auto render_mode = archive.data_word(description + 0x04);
    const auto alpha = archive.data_float(material_offset + 0x0C);
    const auto shininess = archive.data_float(material_offset + 0x10);
    if (!render_mode.has_value() || !alpha.has_value() ||
        !shininess.has_value() || !std::isfinite(*alpha) ||
        !std::isfinite(*shininess)) {
        return false;
    }
    material.source_offset = material_offset;
    material.render_mode = *render_mode;
    material.alpha = *alpha;
    material.shininess = *shininess;
    std::array<std::array<uint8_t, 4>*, 3> colors = {
        &material.ambient, &material.diffuse, &material.specular,
    };
    for (uint32_t color = 0; color < colors.size(); ++color) {
        for (uint32_t component = 0; component < (*colors[color]).size();
             ++component) {
            const auto value = archive.data_byte(material_offset + color * 4 + component);
            if (!value.has_value()) {
                return false;
            }
            (*colors[color])[component] = *value;
        }
    }
    return true;
}

bool read_u16(const Archive& archive, uint32_t offset, uint16_t& value)
{
    const auto high = archive.data_byte(offset);
    const auto low = archive.data_byte(offset + 1);
    if (!high.has_value() || !low.has_value()) {
        return false;
    }
    value = static_cast<uint16_t>((static_cast<uint16_t>(*high) << 8) | *low);
    return true;
}

bool supported_texture_format(uint32_t format)
{
    // Palette-backed C4/C8/C14X2 images need their HSD_TlutDesc chain.  They
    // are deliberately postponed; all formats accepted here are directly
    // consumable by GXInitTexObj.
    switch (format) {
    case 0x0: // GX_TF_I4
    case 0x1: // GX_TF_I8
    case 0x2: // GX_TF_IA4
    case 0x3: // GX_TF_IA8
    case 0x4: // GX_TF_RGB565
    case 0x5: // GX_TF_RGB5A3
    case 0x6: // GX_TF_RGBA8
    case 0xE: // GX_TF_CMPR
        return true;
    default:
        return false;
    }
}

bool read_texture(const Archive& archive, uint32_t texture_description,
                  HostTexture& texture)
{
    // HSD_TObjDesc offsets are the on-disc 32-bit GameCube layout.
    const auto image_description =
        archive.data_pointer(texture_description + 0x4C);
    const auto wrap_s = archive.data_word(texture_description + 0x34);
    const auto wrap_t = archive.data_word(texture_description + 0x38);
    if (!image_description.has_value() || !wrap_s.has_value() ||
        !wrap_t.has_value() || *image_description == 0) {
        return false;
    }
    const auto image_data = archive.data_pointer(*image_description);
    const auto format = archive.data_word(*image_description + 0x08);
    const auto mipmap = archive.data_word(*image_description + 0x0C);
    const auto max_lod = archive.data_float(*image_description + 0x14);
    if (!image_data.has_value() || !format.has_value() || !mipmap.has_value() ||
        !max_lod.has_value() || !supported_texture_format(*format) ||
        !std::isfinite(*max_lod) || *max_lod < 0.0F || *max_lod > 255.0F ||
        !read_u16(archive, *image_description + 0x04, texture.width) ||
        !read_u16(archive, *image_description + 0x06, texture.height) ||
        texture.width == 0 || texture.height == 0) {
        return false;
    }
    const bool has_mipmaps = *mipmap != 0;
    const uint32_t image_size = GXGetTexBufferSize(
        texture.width, texture.height, *format, has_mipmaps ? GX_TRUE : GX_FALSE,
        static_cast<uint8_t>(std::ceil(*max_lod)));
    if (image_size == 0 || !archive.contains_data_range(*image_data, image_size)) {
        return false;
    }
    texture.source_offset = *image_description;
    texture.format = *format;
    texture.wrap_s = *wrap_s;
    texture.wrap_t = *wrap_t;
    texture.mipmap = has_mipmaps;
    texture.image_data.resize(image_size);
    for (uint32_t index = 0; index < image_size; ++index) {
        const auto byte = archive.data_byte(*image_data + index);
        if (!byte.has_value()) {
            return false;
        }
        texture.image_data[index] = *byte;
    }
    return true;
}

} // namespace

bool HostScene::load(const Archive& archive, std::string_view symbol)
{
    joints_.clear();
    cameras_.clear();
    materials_.clear();
    textures_.clear();
    draw_objects_.clear();
    model_roots_.clear();
    last_error_ = "could not resolve HSD scene roots";

    const auto scene = archive.scene_roots(symbol);
    const auto model_count = archive.scene_model_count(symbol);
    if (!scene.has_value() || !model_count.has_value()) {
        return false;
    }

    // SceneCameraDesc is { HSD_CObjDesc* desc, HSD_CameraAnim** anims } and
    // is terminated by a null desc.  Camera animation remains a later HSD
    // milestone; this copies the static camera used by standScene.
    if (scene->cameras != 0) {
        for (uint32_t index = 0; index < kMaxSceneCameras; ++index) {
            const uint32_t entry = scene->cameras + index * 8;
            const auto camera_description = archive.data_pointer(entry);
            if (!camera_description.has_value()) {
                return false;
            }
            if (*camera_description == 0) {
                break;
            }
            HostCamera camera{};
            if (!read_camera(archive, *camera_description, camera)) {
                // A malformed/unsupported camera must not discard otherwise
                // renderable scene geometry; the renderer retains its debug
                // camera fallback until a valid CObj is available.
                continue;
            }
            cameras_.push_back(camera);
        }
    }

    last_error_ = "could not build HSD joint hierarchy";
    std::vector<uint32_t> pending;
    for (uint32_t index = 0; index < *model_count; ++index) {
        const auto model = archive.data_pointer(scene->models +
                                             index * sizeof(uint32_t));
        if (!model.has_value()) {
            return false;
        }
        const auto root = archive.data_pointer(*model);
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
        const auto child = archive.data_pointer(offset + 0x08);
        const auto sibling = archive.data_pointer(offset + 0x0C);
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
        const auto child = archive.data_pointer(joint.source_offset + 0x08);
        const auto sibling = archive.data_pointer(joint.source_offset + 0x0C);
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
    std::unordered_map<uint32_t, int32_t> material_indices;
    std::unordered_map<uint32_t, int32_t> texture_indices;
    for (HostJoint& joint : joints_) {
        const auto first = archive.data_pointer(joint.source_offset + 0x10);
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
            const auto next = archive.data_pointer(description + 0x04);
            const auto material = archive.data_pointer(description + 0x08);
            const auto primitive = archive.data_pointer(description + 0x0C);
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
                    archive.data_pointer(*material + 0x08);
                const auto material_data = archive.data_pointer(*material + 0x0C);
                if (!render_mode.has_value() ||
                    !texture_description.has_value() ||
                    !material_data.has_value()) {
                    return false;
                }
                object.render_mode = *render_mode;
                object.texture_description = *texture_description;
                object.material = *material_data;
                if (*material_data != 0) {
                    const auto existing = material_indices.find(*material);
                    if (existing != material_indices.end()) {
                        object.material_index = existing->second;
                    } else {
                        HostMaterial host_material{};
                        if (read_material(archive, *material, *material_data,
                                          host_material)) {
                            object.material_index =
                                static_cast<int32_t>(materials_.size());
                            material_indices.emplace(*material,
                                                     object.material_index);
                            if (*texture_description != 0) {
                                const auto texture = texture_indices.find(*texture_description);
                                if (texture != texture_indices.end()) {
                                    host_material.texture_index = texture->second;
                                } else {
                                    HostTexture host_texture{};
                                    if (read_texture(archive, *texture_description,
                                                     host_texture)) {
                                        host_material.texture_index =
                                            static_cast<int32_t>(textures_.size());
                                        texture_indices.emplace(*texture_description,
                                                                host_material.texture_index);
                                        textures_.push_back(std::move(host_texture));
                                    }
                                }
                            }
                            materials_.push_back(host_material);
                        }
                    }
                }
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
                if (!archive.contains_data_range(object.display_list,
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
                        archive.data_pointer(descriptor + 0x14);
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
                        // HSD_VtxDescList has a u8 fraction followed by the
                        // compiler's alignment padding and a big-endian u16
                        // stride.  The stride is consequently the low word
                        // returned by Archive::data_word.
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
                    object.position_decode_error = std::move(position_error);
                    // An individual legacy vertex layout must not prevent the
                    // rest of a scene from loading.  The host renderer can
                    // skip this object until its layout is implemented.
                    object.positions.clear();
                    object.triangle_indices.clear();
                }
                std::string normal_error;
                if (!materialize_normals(archive, object, normal_error)) {
                    object.normals.clear();
                    std::fill(object.triangle_normal_indices.begin(),
                              object.triangle_normal_indices.end(), UINT32_MAX);
                }
                std::string texcoord_error;
                if (!materialize_texcoords(archive, object, texcoord_error)) {
                    object.texcoords.clear();
                    std::fill(object.triangle_texcoord_indices.begin(),
                              object.triangle_texcoord_indices.end(), UINT32_MAX);
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

            // A DObj points at the head of an HSD_PObjDesc chain.  The
            // original runtime loads every PObj in that chain; keeping only
            // the head silently discards geometry when it is stored in a
            // later PObj (as happens in standScene).
            uint32_t primitive_link = *primitive;
            std::unordered_set<uint32_t> primitive_chain;
            while (primitive_link != 0) {
                if (!primitive_chain.insert(primitive_link).second) {
                    last_error_ = "cyclic HSD PObjDesc chain";
                    return false;
                }
                const auto primitive_next = archive.data_pointer(primitive_link + 0x04);
                if (!primitive_next.has_value()) {
                    last_error_ = "truncated HSD PObjDesc chain";
                    return false;
                }
                primitive_link = *primitive_next;
                if (primitive_link == 0) {
                    break;
                }
                if (draw_objects_.size() >= kMaxSceneDrawObjects) {
                    last_error_ = "too many HSD PObjDesc entries";
                    return false;
                }
                HostDrawObject chained{};
                chained.source_offset = primitive_link;
                chained.material_description = *material;
                chained.primitive_description = primitive_link;
                chained.render_mode = object.render_mode;
                chained.texture_description = object.texture_description;
                chained.material = object.material;
                chained.material_index = object.material_index;
                std::string primitive_error;
                if (!materialize_primitive(archive, primitive_link, chained, primitive_error)) {
                    last_error_ = primitive_error;
                    return false;
                }
                const int32_t chained_index = static_cast<int32_t>(draw_objects_.size());
                draw_objects_.push_back(std::move(chained));
                draw_objects_[static_cast<size_t>(previous_draw_object)].next = chained_index;
                previous_draw_object = chained_index;
            }
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

const std::vector<HostCamera>& HostScene::cameras() const
{
    return cameras_;
}

const std::vector<HostMaterial>& HostScene::materials() const
{
    return materials_;
}

const std::vector<HostTexture>& HostScene::textures() const
{
    return textures_;
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
