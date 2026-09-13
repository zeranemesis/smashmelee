#include "harness.hpp"

#include "dat_builder.hpp"
#include "hsd_fixtures.hpp"
#include "scene.hpp"

#include <melee/sysdolphin/baselib/archive.hpp>

#include <array>
#include <vector>

using meleeboard::hsd::Archive;
using meleeboard::hsd::HostScene;
using meleeboard::test::DatBuilder;
namespace fixtures = meleeboard::test::hsd;

namespace {

constexpr std::array<std::array<float, 3>, 3> kTriangle = { {
    { 0.0F, 0.0F, 0.0F },
    { 1.0F, 0.0F, 0.0F },
    { 0.0F, 1.0F, 0.0F },
} };

// Builds one drawable triangle and returns the HSD_DObjDesc that owns it.
uint32_t add_triangle_draw_object(DatBuilder& builder, uint32_t material_object)
{
    const uint32_t positions = fixtures::add_float_positions(
        builder, { kTriangle.begin(), kTriangle.end() });
    const uint32_t descriptors = fixtures::add_position_descriptor(
        builder, positions, 12, fixtures::kAddressIndex8);
    uint16_t length = 0;
    const uint32_t display_list = fixtures::add_indexed_display_list(
        builder, fixtures::kOpTriangles, { 0, 1, 2 }, length);
    const uint32_t primitive =
        fixtures::add_primitive(builder, descriptors, display_list, length);
    return fixtures::add_draw_object(builder, material_object, primitive);
}

} // namespace

MELEE_TEST(Scene, DecodesASceneGraphIntoHostIndices)
{
    DatBuilder builder;
    const uint32_t material = fixtures::add_material(
        builder, { 12, 34, 56, 200 }, 0.75F, 8.0F);
    const uint32_t texture = fixtures::add_texture(builder, 8, 8, 0x4);
    const uint32_t material_object =
        fixtures::add_material_object(builder, texture, material, 0x12345678);
    const uint32_t draw_object =
        add_triangle_draw_object(builder, material_object);

    const uint32_t root = fixtures::add_joint(builder);
    const uint32_t child = fixtures::add_joint(builder);
    fixtures::set_joint_child(builder, root, child);
    fixtures::set_joint_translation(builder, child, { 1.0F, 2.0F, 3.0F });
    fixtures::set_joint_draw_object(builder, root, draw_object);
    const uint32_t camera = fixtures::add_camera(builder, { 0.0F, 0.0F, 10.0F },
                                                 { 0.0F, 0.0F, 0.0F }, 1.0F,
                                                 100.0F);
    builder.symbol("testScene", fixtures::add_scene(builder, { root }, camera));

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    HostScene scene;
    REQUIRE(scene.load(archive, "testScene"));
    CHECK(scene.last_error().empty());

    REQUIRE(scene.joints().size() == 2);
    REQUIRE(scene.model_roots().size() == 1);
    // Model roots are resolved to joint indices, not archive offsets.
    const uint32_t root_index = scene.model_roots().front();
    REQUIRE(root_index < scene.joints().size());
    CHECK_EQ(scene.joints()[root_index].source_offset, root);
    REQUIRE(scene.joints()[root_index].child >= 0);
    const auto child_index =
        static_cast<size_t>(scene.joints()[root_index].child);
    CHECK_EQ(scene.joints()[child_index].source_offset, child);
    CHECK_EQ(scene.joints()[child_index].translation[1], 2.0F);
    CHECK_EQ(scene.joints()[child_index].scale[0], 1.0F);

    REQUIRE(scene.cameras().size() == 1);
    CHECK_EQ(scene.cameras().front().eye[2], 10.0F);
    CHECK_EQ(scene.cameras().front().near_plane, 1.0F);
    CHECK_EQ(scene.cameras().front().far_plane, 100.0F);
    CHECK_EQ(scene.cameras().front().viewport[1], 640);
    CHECK_EQ(scene.cameras().front().scissor[3], 480);

    REQUIRE(scene.materials().size() == 1);
    CHECK_EQ(scene.materials().front().render_mode, 0x12345678U);
    CHECK_EQ(scene.materials().front().diffuse[0], 12);
    CHECK_EQ(scene.materials().front().diffuse[3], 200);
    CHECK_EQ(scene.materials().front().alpha, 0.75F);
    CHECK_EQ(scene.materials().front().shininess, 8.0F);
    REQUIRE(scene.materials().front().texture_index >= 0);

    REQUIRE(scene.textures().size() == 1);
    CHECK_EQ(scene.textures().front().width, 8);
    CHECK_EQ(scene.textures().front().height, 8);
    CHECK_EQ(scene.textures().front().format, 0x4U);
    // GX_TF_RGB565 packs 4x4 tiles at 32 bytes each.
    CHECK_EQ(scene.textures().front().image_data.size(), 128U);
}

MELEE_TEST(Scene, DecodesIndexedTriangles)
{
    DatBuilder builder;
    const uint32_t draw_object = add_triangle_draw_object(builder, 0);
    const uint32_t root = fixtures::add_joint(builder);
    fixtures::set_joint_draw_object(builder, root, draw_object);
    builder.symbol("model_joint", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    HostScene scene;
    REQUIRE(scene.load_joint(archive, "model_joint"));
    REQUIRE(scene.draw_objects().size() == 1);

    const auto& object = scene.draw_objects().front();
    CHECK(object.position_stream_decoded);
    CHECK_EQ(object.triangle_count, 1U);
    CHECK_EQ(object.vertex_count, 3U);
    CHECK_EQ(object.primitive_batch_count, 1U);
    REQUIRE(object.positions.size() == 3);
    CHECK_EQ(object.positions[1][0], 1.0F);
    CHECK_EQ(object.positions[2][1], 1.0F);
    REQUIRE(object.triangle_indices.size() == 3);
    CHECK_EQ(object.triangle_indices[0], 0U);
    CHECK_EQ(object.triangle_indices[2], 2U);
}

MELEE_TEST(Scene, ExpandsStripsFansAndQuads)
{
    struct Expectation {
        uint8_t command;
        std::vector<uint8_t> indices;
        uint32_t triangles;
    };
    const Expectation cases[] = {
        { fixtures::kOpTriangleStrip, { 0, 1, 2, 0 }, 2 },
        { fixtures::kOpTriangleFan, { 0, 1, 2, 1 }, 2 },
        { fixtures::kOpQuads, { 0, 1, 2, 1 }, 2 },
    };

    for (const Expectation& expected : cases) {
        DatBuilder builder;
        const uint32_t positions = fixtures::add_float_positions(
            builder, { kTriangle.begin(), kTriangle.end() });
        const uint32_t descriptors = fixtures::add_position_descriptor(
            builder, positions, 12, fixtures::kAddressIndex8);
        uint16_t length = 0;
        const uint32_t display_list = fixtures::add_indexed_display_list(
            builder, expected.command, expected.indices, length);
        const uint32_t primitive =
            fixtures::add_primitive(builder, descriptors, display_list, length);
        const uint32_t draw_object =
            fixtures::add_draw_object(builder, 0, primitive);
        const uint32_t root = fixtures::add_joint(builder);
        fixtures::set_joint_draw_object(builder, root, draw_object);
        builder.symbol("model_joint", root);

        Archive archive;
        REQUIRE(archive.parse(builder.build()));
        HostScene scene;
        REQUIRE(scene.load_joint(archive, "model_joint"));
        REQUIRE(scene.draw_objects().size() == 1);
        CHECK_EQ(scene.draw_objects().front().triangle_count, expected.triangles);
        CHECK_EQ(scene.draw_objects().front().triangle_position_indices.size(),
                 expected.triangles * 3);
    }
}

MELEE_TEST(Scene, FollowsThePrimitiveChainOfADrawObject)
{
    // A DObj points at the head of a PObjDesc chain; keeping only the head
    // silently discards geometry stored in later entries.
    DatBuilder builder;
    const uint32_t positions = fixtures::add_float_positions(
        builder, { kTriangle.begin(), kTriangle.end() });
    const uint32_t descriptors = fixtures::add_position_descriptor(
        builder, positions, 12, fixtures::kAddressIndex8);
    uint16_t first_length = 0;
    uint16_t second_length = 0;
    const uint32_t first_list = fixtures::add_indexed_display_list(
        builder, fixtures::kOpTriangles, { 0, 1, 2 }, first_length);
    const uint32_t second_list = fixtures::add_indexed_display_list(
        builder, fixtures::kOpTriangles, { 2, 1, 0 }, second_length);
    const uint32_t first =
        fixtures::add_primitive(builder, descriptors, first_list, first_length);
    const uint32_t second = fixtures::add_primitive(builder, descriptors,
                                                    second_list, second_length);
    fixtures::set_primitive_next(builder, first, second);
    const uint32_t draw_object = fixtures::add_draw_object(builder, 0, first);
    const uint32_t root = fixtures::add_joint(builder);
    fixtures::set_joint_draw_object(builder, root, draw_object);
    builder.symbol("model_joint", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostScene scene;
    REQUIRE(scene.load_joint(archive, "model_joint"));
    REQUIRE(scene.draw_objects().size() == 2);
    CHECK_EQ(scene.draw_objects()[0].triangle_count, 1U);
    CHECK_EQ(scene.draw_objects()[1].triangle_count, 1U);
    CHECK_EQ(scene.draw_objects()[0].next, 1);
    CHECK_EQ(scene.joints().front().first_draw_object, 0);
}

MELEE_TEST(Scene, FollowsTheDrawObjectChainOfAJoint)
{
    DatBuilder builder;
    const uint32_t first = add_triangle_draw_object(builder, 0);
    const uint32_t second = add_triangle_draw_object(builder, 0);
    fixtures::set_draw_object_next(builder, first, second);
    const uint32_t root = fixtures::add_joint(builder);
    fixtures::set_joint_draw_object(builder, root, first);
    builder.symbol("model_joint", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostScene scene;
    REQUIRE(scene.load_joint(archive, "model_joint"));
    REQUIRE(scene.draw_objects().size() == 2);
    CHECK_EQ(scene.draw_objects()[0].source_offset, first);
    CHECK_EQ(scene.draw_objects()[1].source_offset, second);
    CHECK_EQ(scene.draw_objects()[0].next, 1);
    CHECK_EQ(scene.draw_objects()[1].next, -1);
}

MELEE_TEST(Scene, RejectsACyclicDrawObjectChain)
{
    DatBuilder builder;
    const uint32_t first = add_triangle_draw_object(builder, 0);
    const uint32_t second = add_triangle_draw_object(builder, 0);
    fixtures::set_draw_object_next(builder, first, second);
    fixtures::set_draw_object_next(builder, second, first);
    const uint32_t root = fixtures::add_joint(builder);
    fixtures::set_joint_draw_object(builder, root, first);
    builder.symbol("model_joint", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostScene scene;
    CHECK(!scene.load_joint(archive, "model_joint"));
    CHECK(!scene.last_error().empty());
}

MELEE_TEST(Scene, RejectsACyclicPrimitiveChain)
{
    DatBuilder builder;
    const uint32_t positions = fixtures::add_float_positions(
        builder, { kTriangle.begin(), kTriangle.end() });
    const uint32_t descriptors = fixtures::add_position_descriptor(
        builder, positions, 12, fixtures::kAddressIndex8);
    uint16_t length = 0;
    const uint32_t list = fixtures::add_indexed_display_list(
        builder, fixtures::kOpTriangles, { 0, 1, 2 }, length);
    const uint32_t first =
        fixtures::add_primitive(builder, descriptors, list, length);
    const uint32_t second =
        fixtures::add_primitive(builder, descriptors, list, length);
    fixtures::set_primitive_next(builder, first, second);
    fixtures::set_primitive_next(builder, second, first);
    const uint32_t draw_object = fixtures::add_draw_object(builder, 0, first);
    const uint32_t root = fixtures::add_joint(builder);
    fixtures::set_joint_draw_object(builder, root, draw_object);
    builder.symbol("model_joint", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostScene scene;
    CHECK(!scene.load_joint(archive, "model_joint"));
    CHECK_EQ(scene.last_error(), std::string("cyclic HSD PObjDesc chain"));
}

MELEE_TEST(Scene, RejectsADisplayListOutsideTheArchive)
{
    DatBuilder builder;
    const uint32_t positions = fixtures::add_float_positions(
        builder, { kTriangle.begin(), kTriangle.end() });
    const uint32_t descriptors = fixtures::add_position_descriptor(
        builder, positions, 12, fixtures::kAddressIndex8);
    uint16_t length = 0;
    const uint32_t list = fixtures::add_indexed_display_list(
        builder, fixtures::kOpTriangles, { 0, 1, 2 }, length);
    // Claim far more 32-byte units than the archive holds.
    const uint32_t primitive = fixtures::add_primitive(builder, descriptors,
                                                       list, 4096);
    const uint32_t draw_object = fixtures::add_draw_object(builder, 0, primitive);
    const uint32_t root = fixtures::add_joint(builder);
    fixtures::set_joint_draw_object(builder, root, draw_object);
    builder.symbol("model_joint", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostScene scene;
    CHECK(!scene.load_joint(archive, "model_joint"));
}

MELEE_TEST(Scene, KeepsGeometryWhenACameraIsUnusable)
{
    // A malformed camera must not discard renderable geometry; the renderer
    // keeps its debug camera until a valid CObj shows up.
    DatBuilder builder;
    const uint32_t draw_object = add_triangle_draw_object(builder, 0);
    const uint32_t root = fixtures::add_joint(builder);
    fixtures::set_joint_draw_object(builder, root, draw_object);
    // near_plane and far_plane left at zero make the descriptor unusable.
    const uint32_t camera = builder.allocate(fixtures::kCameraSize);
    builder.symbol("testScene", fixtures::add_scene(builder, { root }, camera));

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostScene scene;
    REQUIRE(scene.load(archive, "testScene"));
    CHECK(scene.cameras().empty());
    REQUIRE(scene.draw_objects().size() == 1);
    CHECK(scene.draw_objects().front().position_stream_decoded);
}

MELEE_TEST(Scene, LoadsAJointTreeFromARelocatedOffset)
{
    DatBuilder builder;
    const uint32_t root = fixtures::add_joint(builder);
    const uint32_t child = fixtures::add_joint(builder);
    fixtures::set_joint_child(builder, root, child);
    builder.symbol("unused", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    HostScene by_offset;
    REQUIRE(by_offset.load_joint_at(archive, root));
    CHECK_EQ(by_offset.joints().size(), 2U);

    HostScene out_of_range;
    CHECK(!out_of_range.load_joint_at(archive, archive.data_size()));
    HostScene missing_symbol;
    CHECK(!missing_symbol.load_joint(archive, "absent_joint"));
}
