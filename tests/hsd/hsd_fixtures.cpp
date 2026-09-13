#include "hsd_fixtures.hpp"

namespace meleeboard::test::hsd {

uint32_t add_joint(DatBuilder& builder, uint32_t flags)
{
    const uint32_t joint = builder.allocate(kJointSize);
    builder.u32(joint + 0x04, flags);
    // Identity scale keeps a fixture joint neutral in the render transform.
    builder.f32(joint + 0x20, 1.0F);
    builder.f32(joint + 0x24, 1.0F);
    builder.f32(joint + 0x28, 1.0F);
    return joint;
}

void set_joint_child(DatBuilder& builder, uint32_t joint, uint32_t child)
{
    builder.pointer(joint + 0x08, child);
}

void set_joint_sibling(DatBuilder& builder, uint32_t joint, uint32_t sibling)
{
    builder.pointer(joint + 0x0C, sibling);
}

void set_joint_draw_object(DatBuilder& builder, uint32_t joint, uint32_t object)
{
    builder.pointer(joint + 0x10, object);
}

void set_joint_translation(DatBuilder& builder, uint32_t joint,
                           std::array<float, 3> translation)
{
    builder.f32(joint + 0x2C, translation[0]);
    builder.f32(joint + 0x30, translation[1]);
    builder.f32(joint + 0x34, translation[2]);
}

uint32_t add_float_positions(DatBuilder& builder,
                             const std::vector<std::array<float, 3>>& positions)
{
    const uint32_t array =
        builder.allocate(static_cast<uint32_t>(positions.size()) * 12);
    for (size_t index = 0; index < positions.size(); ++index) {
        const auto base = array + static_cast<uint32_t>(index) * 12;
        builder.f32(base, positions[index][0]);
        builder.f32(base + 4, positions[index][1]);
        builder.f32(base + 8, positions[index][2]);
    }
    return array;
}

uint32_t add_position_descriptor(DatBuilder& builder, uint32_t vertex_data,
                                 uint16_t stride, uint32_t addressing)
{
    const uint32_t list = builder.allocate(kVertexDescriptorSize * 2);
    builder.u32(list + 0x00, kAttributePosition);
    builder.u32(list + 0x04, addressing);
    builder.u32(list + 0x08, 1); // GX_POS_XYZ
    builder.u32(list + 0x0C, kComponentFloat);
    // HSD_VtxDescList stores a u8 fraction, padding, then a big-endian u16
    // stride, which the decoder reads back as one word.
    builder.u8(list + 0x10, 0);
    builder.u16(list + 0x12, stride);
    builder.pointer(list + 0x14, vertex_data);
    builder.u32(list + kVertexDescriptorSize, kAttributeNull);
    return list;
}

uint32_t add_indexed_display_list(DatBuilder& builder, uint8_t command,
                                  const std::vector<uint8_t>& indices,
                                  uint16_t& list_length)
{
    // A display list is stored as a count of 32-byte units; the decoder walks
    // the whole padded range and treats a zero opcode as padding.
    const uint32_t used = 3 + static_cast<uint32_t>(indices.size());
    list_length = static_cast<uint16_t>((used + 31) / 32);
    const uint32_t list = builder.allocate(list_length * 32, 32);
    builder.u8(list, command);
    builder.u16(list + 1, static_cast<uint16_t>(indices.size()));
    for (size_t index = 0; index < indices.size(); ++index) {
        builder.u8(list + 3 + static_cast<uint32_t>(index), indices[index]);
    }
    return list;
}

uint32_t add_primitive(DatBuilder& builder, uint32_t descriptors,
                       uint32_t display_list, uint16_t display_list_length)
{
    const uint32_t primitive = builder.allocate(kPrimitiveSize);
    builder.pointer(primitive + 0x08, descriptors);
    builder.u32(primitive + 0x0C, display_list_length);
    builder.pointer(primitive + 0x10, display_list);
    return primitive;
}

void set_primitive_next(DatBuilder& builder, uint32_t primitive, uint32_t next)
{
    builder.pointer(primitive + 0x04, next);
}

uint32_t add_material(DatBuilder& builder, std::array<uint8_t, 4> diffuse,
                      float alpha, float shininess)
{
    const uint32_t material = builder.allocate(kMaterialSize);
    for (uint32_t index = 0; index < diffuse.size(); ++index) {
        builder.u8(material + 0x04 + index, diffuse[index]);
    }
    builder.f32(material + 0x0C, alpha);
    builder.f32(material + 0x10, shininess);
    return material;
}

uint32_t add_material_object(DatBuilder& builder, uint32_t texture,
                             uint32_t material, uint32_t render_mode)
{
    const uint32_t object = builder.allocate(kMaterialObjectSize);
    builder.u32(object + 0x04, render_mode);
    if (texture != 0) {
        builder.pointer(object + 0x08, texture);
    }
    builder.pointer(object + 0x0C, material);
    return object;
}

uint32_t add_draw_object(DatBuilder& builder, uint32_t material_object,
                         uint32_t primitive)
{
    const uint32_t object = builder.allocate(kDrawObjectSize);
    if (material_object != 0) {
        builder.pointer(object + 0x08, material_object);
    }
    if (primitive != 0) {
        builder.pointer(object + 0x0C, primitive);
    }
    return object;
}

void set_draw_object_next(DatBuilder& builder, uint32_t object, uint32_t next)
{
    builder.pointer(object + 0x04, next);
}

uint32_t add_camera(DatBuilder& builder, std::array<float, 3> eye,
                    std::array<float, 3> interest, float near_plane,
                    float far_plane)
{
    const uint32_t eye_object = builder.allocate(kWorldObjectSize);
    builder.f32(eye_object + 0x04, eye[0]);
    builder.f32(eye_object + 0x08, eye[1]);
    builder.f32(eye_object + 0x0C, eye[2]);
    const uint32_t interest_object = builder.allocate(kWorldObjectSize);
    builder.f32(interest_object + 0x04, interest[0]);
    builder.f32(interest_object + 0x08, interest[1]);
    builder.f32(interest_object + 0x0C, interest[2]);

    const uint32_t camera = builder.allocate(kCameraSize);
    builder.u32(camera + 0x04, 1); // GX_PERSPECTIVE, no up-vector flag
    builder.s16(camera + 0x08, 0);
    builder.s16(camera + 0x0A, 640);
    builder.s16(camera + 0x0C, 0);
    builder.s16(camera + 0x0E, 480);
    builder.u16(camera + 0x10, 0);
    builder.u16(camera + 0x12, 640);
    builder.u16(camera + 0x14, 0);
    builder.u16(camera + 0x16, 480);
    builder.pointer(camera + 0x18, eye_object);
    builder.pointer(camera + 0x1C, interest_object);
    builder.f32(camera + 0x28, near_plane);
    builder.f32(camera + 0x2C, far_plane);
    builder.f32(camera + 0x30, 30.0F);  // field of view
    builder.f32(camera + 0x34, 4.0F / 3.0F);
    return camera;
}

uint32_t add_texture(DatBuilder& builder, uint16_t width, uint16_t height,
                     uint32_t format)
{
    // GX_TF_RGB565/RGB5A3 store 32 bytes per 4x4 tile; the fixtures only need
    // the buffer to be large enough for the decoder's own size computation.
    const uint32_t tiles = ((width + 3U) / 4U) * ((height + 3U) / 4U);
    const uint32_t image_bytes = tiles * 32U;
    const uint32_t pixels = builder.allocate(image_bytes, 32);

    const uint32_t image = builder.allocate(kImageSize);
    builder.pointer(image + 0x00, pixels);
    builder.u16(image + 0x04, width);
    builder.u16(image + 0x06, height);
    builder.u32(image + 0x08, format);
    builder.u32(image + 0x0C, 0); // no mipmaps
    builder.f32(image + 0x14, 0.0F);

    const uint32_t texture = builder.allocate(kTextureSize);
    builder.u32(texture + 0x34, 0); // GX_CLAMP
    builder.u32(texture + 0x38, 0);
    builder.pointer(texture + 0x4C, image);
    return texture;
}

uint32_t add_paletted_texture(DatBuilder& builder, uint16_t width,
                              uint16_t height, uint32_t format,
                              uint32_t palette_format, uint16_t entries)
{
    // C4 packs 8x8 tiles, C8 8x4, C14X2 4x4; all of them at 32 bytes a tile.
    const uint32_t tile_width = format == 0x8 ? 8U : format == 0x9 ? 8U : 4U;
    const uint32_t tile_height = format == 0x8 ? 8U : 4U;
    const uint32_t tiles = ((width + tile_width - 1) / tile_width) *
        ((height + tile_height - 1) / tile_height);
    const uint32_t indices = builder.allocate(tiles * 32U, 32);

    const uint32_t image = builder.allocate(kImageSize);
    builder.pointer(image + 0x00, indices);
    builder.u16(image + 0x04, width);
    builder.u16(image + 0x06, height);
    builder.u32(image + 0x08, format);
    builder.u32(image + 0x0C, 0);
    builder.f32(image + 0x14, 0.0F);

    const uint32_t entries_bytes = static_cast<uint32_t>(entries) * 2U;
    const uint32_t colors = builder.allocate(entries_bytes, 32);
    const uint32_t palette = builder.allocate(kPaletteSize);
    builder.pointer(palette + 0x00, colors);
    builder.u32(palette + 0x04, palette_format);
    builder.u16(palette + 0x0C, entries);

    const uint32_t texture = builder.allocate(kTextureSize);
    builder.u32(texture + 0x34, 0);
    builder.u32(texture + 0x38, 0);
    builder.pointer(texture + 0x4C, image);
    builder.pointer(texture + 0x50, palette);
    return texture;
}

uint32_t add_scene(DatBuilder& builder, const std::vector<uint32_t>& roots,
                   uint32_t camera)
{
    std::vector<uint32_t> models;
    models.reserve(roots.size());
    for (uint32_t root : roots) {
        const uint32_t model = builder.allocate(kModelSize);
        builder.pointer(model, root);
        models.push_back(model);
    }
    // The model array is NULL terminated, as scene_model_count expects.
    const uint32_t model_array =
        builder.allocate((static_cast<uint32_t>(models.size()) + 1) * 4);
    for (size_t index = 0; index < models.size(); ++index) {
        builder.pointer(model_array + static_cast<uint32_t>(index) * 4,
                        models[index]);
    }

    uint32_t camera_array = 0;
    if (camera != 0) {
        // SceneCameraDesc pairs are { HSD_CObjDesc*, HSD_CameraAnim** }, and
        // the array ends at the first null descriptor.
        camera_array = builder.allocate(16);
        builder.pointer(camera_array, camera);
    }

    const uint32_t scene = builder.allocate(kSceneSize);
    builder.pointer(scene + 0x00, model_array);
    if (camera_array != 0) {
        builder.pointer(scene + 0x04, camera_array);
    }
    return scene;
}

uint32_t add_anim_joint(DatBuilder& builder)
{
    return builder.allocate(kAnimJointSize);
}

void set_anim_joint_child(DatBuilder& builder, uint32_t joint, uint32_t child)
{
    builder.pointer(joint + 0x00, child);
}

void set_anim_joint_sibling(DatBuilder& builder, uint32_t joint,
                            uint32_t sibling)
{
    builder.pointer(joint + 0x04, sibling);
}

void set_anim_joint_object(DatBuilder& builder, uint32_t joint, uint32_t object)
{
    builder.pointer(joint + 0x08, object);
}

uint32_t add_anim_object(DatBuilder& builder, uint32_t flags, float end_frame,
                         uint32_t channel)
{
    const uint32_t object = builder.allocate(kAnimObjectSize);
    builder.u32(object + 0x00, flags);
    builder.f32(object + 0x04, end_frame);
    builder.pointer(object + 0x08, channel);
    return object;
}

uint32_t add_anim_channel(DatBuilder& builder, uint8_t object_type,
                          uint8_t value_fraction, uint8_t slope_fraction,
                          float start_frame,
                          const std::vector<uint8_t>& bytecode)
{
    const uint32_t stream =
        builder.allocate(static_cast<uint32_t>(bytecode.size()));
    builder.blob(stream, bytecode);

    const uint32_t channel = builder.allocate(kAnimChannelSize);
    builder.u32(channel + 0x04, static_cast<uint32_t>(bytecode.size()));
    builder.f32(channel + 0x08, start_frame);
    builder.u8(channel + 0x0C, object_type);
    builder.u8(channel + 0x0D, value_fraction);
    builder.u8(channel + 0x0E, slope_fraction);
    builder.pointer(channel + 0x10, stream);
    return channel;
}

void set_anim_channel_next(DatBuilder& builder, uint32_t channel, uint32_t next)
{
    builder.pointer(channel + 0x00, next);
}

} // namespace meleeboard::test::hsd
