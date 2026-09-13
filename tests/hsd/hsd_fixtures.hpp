#pragma once

// Helpers that lay out real HSD structures inside a synthetic DAT container.
// Offsets and field meanings mirror the GameCube on-disc layout the decoders
// in src/melee_port/hsd read, so a fixture change that drifts from the format
// shows up as a decode failure rather than a silently different test.

#include "dat_builder.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace meleeboard::test::hsd {

// Structure sizes, in the on-disc 32-bit layout.
inline constexpr uint32_t kSceneSize = 0x10;
inline constexpr uint32_t kModelSize = 0x10;
inline constexpr uint32_t kJointSize = 0x40;
inline constexpr uint32_t kDrawObjectSize = 0x10;
inline constexpr uint32_t kMaterialObjectSize = 0x18;
inline constexpr uint32_t kMaterialSize = 0x14;
inline constexpr uint32_t kPrimitiveSize = 0x18;
inline constexpr uint32_t kVertexDescriptorSize = 0x18;
inline constexpr uint32_t kCameraSize = 0x40;
inline constexpr uint32_t kWorldObjectSize = 0x10;
inline constexpr uint32_t kTextureSize = 0x5C;
inline constexpr uint32_t kImageSize = 0x18;
inline constexpr uint32_t kPaletteSize = 0x10;
inline constexpr uint32_t kAnimJointSize = 0x14;
inline constexpr uint32_t kAnimObjectSize = 0x10;
inline constexpr uint32_t kAnimChannelSize = 0x14;

// GX vertex attributes and addressing modes used by the fixtures.
inline constexpr uint32_t kAttributePosition = 9;
inline constexpr uint32_t kAttributeNormal = 10;
inline constexpr uint32_t kAttributeColor0 = 11;
inline constexpr uint32_t kAttributeTexture0 = 13;
inline constexpr uint32_t kAttributeNull = 0xFF;
inline constexpr uint32_t kAddressDirect = 1;
inline constexpr uint32_t kAddressIndex8 = 2;
inline constexpr uint32_t kAddressIndex16 = 3;
inline constexpr uint32_t kComponentFloat = 4;

// GX display-list opcodes.
inline constexpr uint8_t kOpTriangles = 0x90;
inline constexpr uint8_t kOpTriangleStrip = 0x98;
inline constexpr uint8_t kOpTriangleFan = 0xA0;
inline constexpr uint8_t kOpQuads = 0x80;

// HSD_JObjDesc: a joint with identity scale and no links.
uint32_t add_joint(DatBuilder& builder, uint32_t flags = 0);
void set_joint_child(DatBuilder& builder, uint32_t joint, uint32_t child);
void set_joint_sibling(DatBuilder& builder, uint32_t joint, uint32_t sibling);
void set_joint_draw_object(DatBuilder& builder, uint32_t joint, uint32_t object);
void set_joint_translation(DatBuilder& builder, uint32_t joint,
                           std::array<float, 3> translation);

// HSD_PObjDesc vertex arrays and display lists.
uint32_t add_float_positions(DatBuilder& builder,
                             const std::vector<std::array<float, 3>>& positions);
// Emits a one-entry HSD_VtxDescList plus its GX_VA_NULL terminator.
uint32_t add_position_descriptor(DatBuilder& builder, uint32_t vertex_data,
                                 uint16_t stride, uint32_t addressing);
// Emits a display list padded to the 32-byte granularity HSD stores it in;
// `command` is a GX primitive opcode and `indices` one index per vertex.
uint32_t add_indexed_display_list(DatBuilder& builder, uint8_t command,
                                  const std::vector<uint8_t>& indices,
                                  uint16_t& list_length);

uint32_t add_primitive(DatBuilder& builder, uint32_t descriptors,
                       uint32_t display_list, uint16_t display_list_length);
void set_primitive_next(DatBuilder& builder, uint32_t primitive, uint32_t next);
uint32_t add_material(DatBuilder& builder, std::array<uint8_t, 4> diffuse,
                      float alpha, float shininess);
uint32_t add_material_object(DatBuilder& builder, uint32_t texture,
                             uint32_t material, uint32_t render_mode = 0);
uint32_t add_draw_object(DatBuilder& builder, uint32_t material_object,
                         uint32_t primitive);
void set_draw_object_next(DatBuilder& builder, uint32_t object, uint32_t next);

// HSD_CObjDesc plus the two HSD_WObjDesc records it points at.
uint32_t add_camera(DatBuilder& builder, std::array<float, 3> eye,
                    std::array<float, 3> interest, float near_plane,
                    float far_plane);

// HSD_TObjDesc for a direct (non-paletted) GX format.
uint32_t add_texture(DatBuilder& builder, uint16_t width, uint16_t height,
                     uint32_t format);

// HSD_TObjDesc for a palette-backed format (C4/C8/C14X2), including the
// HSD_TlutDesc its image indexes into.
uint32_t add_paletted_texture(DatBuilder& builder, uint16_t width,
                              uint16_t height, uint32_t format,
                              uint32_t palette_format, uint16_t entries);

// SceneDesc { models, cameras, lights, fogs } with one DynamicModelDesc per
// root joint and an optional single-camera array.
uint32_t add_scene(DatBuilder& builder, const std::vector<uint32_t>& roots,
                   uint32_t camera = 0);

// HSD_AnimJoint / HSD_AObjDesc / HSD_FObjDesc.
uint32_t add_anim_joint(DatBuilder& builder);
void set_anim_joint_child(DatBuilder& builder, uint32_t joint, uint32_t child);
void set_anim_joint_sibling(DatBuilder& builder, uint32_t joint,
                            uint32_t sibling);
void set_anim_joint_object(DatBuilder& builder, uint32_t joint,
                           uint32_t object);
uint32_t add_anim_object(DatBuilder& builder, uint32_t flags, float end_frame,
                         uint32_t channel);
uint32_t add_anim_channel(DatBuilder& builder, uint8_t object_type,
                          uint8_t value_fraction, uint8_t slope_fraction,
                          float start_frame,
                          const std::vector<uint8_t>& bytecode);
void set_anim_channel_next(DatBuilder& builder, uint32_t channel, uint32_t next);

} // namespace meleeboard::test::hsd
