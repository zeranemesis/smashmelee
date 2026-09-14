#include "harness.hpp"

#include "dat_builder.hpp"
#include "hsd_fixtures.hpp"

#include <melee/sysdolphin/baselib/archive.hpp>

#include <vector>

using meleeboard::hsd::Archive;
using meleeboard::test::DatBuilder;
namespace fixtures = meleeboard::test::hsd;

MELEE_TEST(Archive, ParsesHeaderTablesAndSymbols)
{
    DatBuilder builder;
    const uint32_t value = builder.allocate(8);
    builder.u32(value, 0xDEADBEEF);
    builder.f32(value + 4, 2.5F);
    builder.symbol("testValue", value);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    CHECK(archive.is_valid());
    CHECK_EQ(archive.public_symbol_count(), 1U);
    CHECK(archive.has_public_symbol("testValue"));
    CHECK(!archive.has_public_symbol("missing"));
    REQUIRE(archive.public_symbol_offset("testValue").has_value());
    CHECK_EQ(*archive.public_symbol_offset("testValue"), value);
    CHECK_EQ(archive.public_symbols().size(), 1U);
    CHECK_EQ(archive.public_symbols().front(), std::string("testValue"));

    REQUIRE(archive.data_word(value).has_value());
    CHECK_EQ(*archive.data_word(value), 0xDEADBEEFU);
    REQUIRE(archive.data_float(value + 4).has_value());
    CHECK_EQ(*archive.data_float(value + 4), 2.5F);
    REQUIRE(archive.data_byte(value).has_value());
    CHECK_EQ(*archive.data_byte(value), 0xDEU);
}

MELEE_TEST(Archive, RejectsAMalformedContainer)
{
    DatBuilder builder;
    const uint32_t value = builder.allocate(8);
    builder.symbol("testValue", value);
    const std::vector<unsigned char> good = builder.build();

    Archive too_short;
    CHECK(!too_short.parse({ 0x00, 0x01, 0x02 }));

    // A file size that disagrees with the buffer must not be trusted.
    std::vector<unsigned char> wrong_size = good;
    wrong_size[3] = static_cast<unsigned char>(wrong_size[3] + 1);
    Archive resized;
    CHECK(!resized.parse(wrong_size));

    // A data section larger than the file would let every later table alias
    // the payload.
    std::vector<unsigned char> wrong_data = good;
    wrong_data[0x04] = 0x10;
    Archive oversized;
    CHECK(!oversized.parse(wrong_data));

    // A truncated tail cannot hold the relocation and symbol tables the header
    // advertises.
    std::vector<unsigned char> truncated(good.begin(), good.end() - 4);
    truncated[3] = static_cast<unsigned char>(truncated.size());
    Archive cut;
    CHECK(!cut.parse(truncated));
}

MELEE_TEST(Archive, TreatsRelocatedAndRawFieldsDifferently)
{
    DatBuilder builder;
    const uint32_t target = builder.allocate(4);
    builder.u32(target, 0x11223344);
    const uint32_t fields = builder.allocate(16);
    builder.pointer(fields + 0, target);
    builder.pointer(fields + 4, 0);      // relocated zero: offset 0 is valid
    // fields + 8 stays an unrelocated zero, the on-disc encoding of NULL.
    builder.u32(fields + 12, 0x80345678); // a raw GameCube address
    builder.symbol("fields", fields);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    REQUIRE(archive.data_pointer(fields + 0).has_value());
    CHECK_EQ(*archive.data_pointer(fields + 0), target);
    // HSD relocation is `*field += (u32) archive->data` over every entry in
    // the relocation table (Locate, in sysdolphin/baselib/archive.c).  A
    // relocated zero therefore becomes the address of the first byte of the
    // data section, while an unrelocated zero stays NULL; the two must not
    // decode to the same offset on a host that keeps offsets.
    REQUIRE(archive.data_pointer(fields + 4).has_value());
    CHECK_EQ(*archive.data_pointer(fields + 4), 0U);
    REQUIRE(archive.data_pointer(fields + 8).has_value());
    CHECK_EQ(*archive.data_pointer(fields + 8), Archive::kNullOffset);
    // An unrelocated non-zero word is a GameCube address, never a host-safe
    // data-section offset.
    CHECK(!archive.data_pointer(fields + 12).has_value());
}

MELEE_TEST(Archive, BoundsChecksDataAccess)
{
    DatBuilder builder;
    const uint32_t value = builder.allocate(8);
    builder.symbol("value", value);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    const uint32_t size = archive.data_size();

    CHECK(archive.contains_data_range(0, size));
    CHECK(archive.contains_data_range(size, 0));
    CHECK(!archive.contains_data_range(0, size + 1));
    CHECK(!archive.contains_data_range(size, 1));
    CHECK(!archive.data_word(size - 3).has_value());
    CHECK(!archive.data_byte(size).has_value());
    CHECK(!archive.data_float(size).has_value());
}

MELEE_TEST(Archive, CountsSceneModelsAndJoints)
{
    DatBuilder builder;
    const uint32_t root = fixtures::add_joint(builder);
    const uint32_t child = fixtures::add_joint(builder);
    const uint32_t sibling = fixtures::add_joint(builder);
    fixtures::set_joint_child(builder, root, child);
    fixtures::set_joint_sibling(builder, child, sibling);
    const uint32_t scene = fixtures::add_scene(builder, { root });
    builder.symbol("testScene", scene);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));

    const auto roots = archive.scene_roots("testScene");
    REQUIRE(roots.has_value());
    CHECK(roots->models != Archive::kNullOffset);
    CHECK_EQ(roots->cameras, Archive::kNullOffset);
    CHECK_EQ(roots->lights, Archive::kNullOffset);
    CHECK_EQ(roots->fogs, Archive::kNullOffset);

    REQUIRE(archive.scene_model_count("testScene").has_value());
    CHECK_EQ(*archive.scene_model_count("testScene"), 1U);
    REQUIRE(archive.scene_joint_count("testScene").has_value());
    CHECK_EQ(*archive.scene_joint_count("testScene"), 3U);

    CHECK(!archive.scene_roots("missingScene").has_value());
    CHECK(!archive.scene_model_count("missingScene").has_value());
}

MELEE_TEST(Archive, CountsAJointTreeFromItsSymbol)
{
    DatBuilder builder;
    const uint32_t root = fixtures::add_joint(builder);
    const uint32_t first = fixtures::add_joint(builder);
    const uint32_t second = fixtures::add_joint(builder);
    const uint32_t grandchild = fixtures::add_joint(builder);
    fixtures::set_joint_child(builder, root, first);
    fixtures::set_joint_sibling(builder, first, second);
    fixtures::set_joint_child(builder, first, grandchild);
    builder.symbol("tree_joint", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    REQUIRE(archive.joint_tree_count("tree_joint").has_value());
    CHECK_EQ(*archive.joint_tree_count("tree_joint"), 4U);
    CHECK(!archive.joint_tree_count("absent_joint").has_value());
}

MELEE_TEST(Archive, RejectsAJointTreeThatLeavesTheDataSection)
{
    DatBuilder builder;
    const uint32_t root = fixtures::add_joint(builder);
    // A child pointer relocated past the end of the data section is a corrupt
    // archive, not a joint the host may walk.
    builder.pointer(root + 0x08, builder.data_size() + 0x40);
    builder.symbol("broken_joint", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    CHECK(!archive.joint_tree_count("broken_joint").has_value());
}
