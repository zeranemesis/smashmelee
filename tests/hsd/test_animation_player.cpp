#include "harness.hpp"

#include "animation.hpp"
#include "animation_player.hpp"
#include "dat_builder.hpp"
#include "hsd_fixtures.hpp"
#include "scene.hpp"

#include <melee/sysdolphin/baselib/aobj.h>
#include <melee/sysdolphin/baselib/archive.hpp>
#include <melee/sysdolphin/baselib/fobj.h>
#include <melee/sysdolphin/baselib/objalloc.h>

#include <vector>

using meleeboard::hsd::Archive;
using meleeboard::hsd::HostAnimation;
using meleeboard::hsd::HostAnimationPlayer;
using meleeboard::hsd::HostJoint;
using meleeboard::test::DatBuilder;
namespace fixtures = meleeboard::test::hsd;

namespace {

constexpr uint8_t kTranslationX = 5;
constexpr uint8_t kVisibility = 11;
constexpr uint8_t kTwoKeyPack = 0x10;
constexpr uint32_t kJointHidden = 1U << 4;

// A single joint whose X translation holds 7 and then 9.
uint32_t add_translation_animation(DatBuilder& builder)
{
    const uint32_t channel = fixtures::add_anim_channel(
        builder, kTranslationX, HSD_A_FRAC_U8, HSD_A_FRAC_U8, 0.0F,
        { HSD_A_OP_CON, kTwoKeyPack, 7, 1, 9, 1 });
    const uint32_t object =
        fixtures::add_anim_object(builder, AOBJ_LOOP, 20.0F, channel);
    const uint32_t joint = fixtures::add_anim_joint(builder);
    fixtures::set_anim_joint_object(builder, joint, object);
    return joint;
}

std::vector<uint32_t> identity_mapping(size_t count)
{
    std::vector<uint32_t> mapping(count);
    for (uint32_t index = 0; index < mapping.size(); ++index) {
        mapping[index] = index;
    }
    return mapping;
}

} // namespace

MELEE_TEST(AnimationPlayer, DrivesAHostJoint)
{
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();

    DatBuilder builder;
    builder.symbol("anim", add_translation_animation(builder));
    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostAnimation animation;
    REQUIRE(animation.load(archive, "anim"));

    std::vector<HostJoint> joints(1);
    HostAnimationPlayer player;
    REQUIRE(player.attach(animation, joints, { 0 }));

    player.tick();
    CHECK_EQ(joints.front().translation[0], 7.0F);
    player.tick();
    CHECK_EQ(joints.front().translation[0], 9.0F);
}

MELEE_TEST(AnimationPlayer, KeepsConcurrentPlayersIndependent)
{
    // The AObj/FObj allocators are process-global.  Attaching a second player
    // must not disturb the objects the first one already owns, which is what
    // the main menu relies on while several animated layers are alive.
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();

    DatBuilder builder;
    builder.symbol("anim", add_translation_animation(builder));
    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostAnimation animation;
    REQUIRE(animation.load(archive, "anim"));

    std::vector<HostJoint> first_joints(1);
    std::vector<HostJoint> second_joints(1);
    HostAnimationPlayer first;
    HostAnimationPlayer second;
    REQUIRE(first.attach(animation, first_joints, { 0 }));
    first.tick();
    REQUIRE(second.attach(animation, second_joints, { 0 }));
    second.tick();

    CHECK_EQ(first_joints.front().translation[0], 7.0F);
    CHECK_EQ(second_joints.front().translation[0], 7.0F);
    first.tick();
    CHECK_EQ(first_joints.front().translation[0], 9.0F);
    CHECK_EQ(second_joints.front().translation[0], 7.0F);
}

MELEE_TEST(AnimationPlayer, ReleasesItsObjectsOnClear)
{
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();

    DatBuilder builder;
    builder.symbol("anim", add_translation_animation(builder));
    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostAnimation animation;
    REQUIRE(animation.load(archive, "anim"));

    const uint32_t animation_objects =
        HSD_ObjAllocGetUsing(HSD_AObjGetAllocData());
    const uint32_t channels = HSD_ObjAllocGetUsing(HSD_FObjGetAllocData());
    {
        std::vector<HostJoint> joints(1);
        HostAnimationPlayer player;
        REQUIRE(player.attach(animation, joints, { 0 }));
        CHECK(HSD_ObjAllocGetUsing(HSD_AObjGetAllocData()) > animation_objects);
        CHECK(HSD_ObjAllocGetUsing(HSD_FObjGetAllocData()) > channels);
    }
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_AObjGetAllocData()), animation_objects);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_FObjGetAllocData()), channels);
}

MELEE_TEST(AnimationPlayer, RejectsAMappingThatDoesNotMatch)
{
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();

    DatBuilder builder;
    builder.symbol("anim", add_translation_animation(builder));
    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostAnimation animation;
    REQUIRE(animation.load(archive, "anim"));

    std::vector<HostJoint> joints(1);
    HostAnimationPlayer player;
    // Too few entries, and an entry past the end of the joint vector.
    CHECK(!player.attach(animation, joints, {}));
    CHECK(!player.attach(animation, joints, { 4 }));
}

MELEE_TEST(AnimationPlayer, AddressesSubtreesInPreOrder)
{
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();

    // A root with two animated children; lb_80011E24 addresses them by
    // pre-order traversal index, not by storage order.
    DatBuilder builder;
    const auto add_channel = [&builder](uint8_t value) {
        return fixtures::add_anim_channel(
            builder, kTranslationX, HSD_A_FRAC_U8, HSD_A_FRAC_U8, 0.0F,
            { HSD_A_OP_CON, kTwoKeyPack, value,
              static_cast<uint8_t>(1), static_cast<uint8_t>(value + 1),
              static_cast<uint8_t>(1) });
    };
    const uint32_t root = fixtures::add_anim_joint(builder);
    const uint32_t first = fixtures::add_anim_joint(builder);
    const uint32_t second = fixtures::add_anim_joint(builder);
    fixtures::set_anim_joint_object(
        builder, first,
        fixtures::add_anim_object(builder, 0, 20.0F, add_channel(3)));
    fixtures::set_anim_joint_object(
        builder, second,
        fixtures::add_anim_object(builder, 0, 20.0F, add_channel(5)));
    fixtures::set_anim_joint_child(builder, root, first);
    fixtures::set_anim_joint_sibling(builder, first, second);
    builder.symbol("tree", root);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostAnimation animation;
    REQUIRE(animation.load(archive, "tree"));
    REQUIRE(animation.joints().size() == 3);

    std::vector<HostJoint> joints(3);
    HostAnimationPlayer player;
    REQUIRE(player.attach(animation, joints, identity_mapping(3)));
    player.tick();

    // Traversal index 0 is the root, 1 its first child, 2 that child's sibling.
    CHECK(player.request_joint(1, 0.0F));
    CHECK(player.request_subtree(0, 0.0F));
    CHECK(!player.request_joint(3, 0.0F));
    CHECK(!player.request_subtree(3, 0.0F));
    CHECK(!player.set_subtree_hidden(3, true));

    CHECK(player.set_subtree_hidden(0, true));
    for (const HostJoint& joint : joints) {
        CHECK((joint.flags & kJointHidden) != 0);
    }
    CHECK(player.set_subtree_hidden(0, false));
    for (const HostJoint& joint : joints) {
        CHECK((joint.flags & kJointHidden) == 0);
    }
}

MELEE_TEST(AnimationPlayer, AppliesVisibilityChannels)
{
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();

    DatBuilder builder;
    // A visibility channel that starts hidden (0) and then becomes visible (1).
    const uint32_t channel = fixtures::add_anim_channel(
        builder, kVisibility, HSD_A_FRAC_U8, HSD_A_FRAC_U8, 0.0F,
        { HSD_A_OP_CON, kTwoKeyPack, 0, 1, 1, 1 });
    const uint32_t object = fixtures::add_anim_object(builder, 0, 20.0F, channel);
    const uint32_t joint = fixtures::add_anim_joint(builder);
    fixtures::set_anim_joint_object(builder, joint, object);
    builder.symbol("visibility", joint);

    Archive archive;
    REQUIRE(archive.parse(builder.build()));
    HostAnimation animation;
    REQUIRE(animation.load(archive, "visibility"));

    std::vector<HostJoint> joints(1);
    HostAnimationPlayer player;
    REQUIRE(player.attach(animation, joints, { 0 }));

    player.tick();
    CHECK((joints.front().flags & kJointHidden) != 0);
    player.tick();
    CHECK((joints.front().flags & kJointHidden) == 0);
}
