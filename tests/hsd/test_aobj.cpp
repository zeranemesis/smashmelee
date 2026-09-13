#include "harness.hpp"

#include <melee/sysdolphin/baselib/aobj.h>
#include <melee/sysdolphin/baselib/fobj.h>
#include <melee/sysdolphin/baselib/objalloc.h>

#include <cstdint>
#include <vector>

namespace {

std::vector<float>* gValues = nullptr;

void record(void*, uint32_t, HSD_ObjData* value)
{
    gValues->push_back(value->fv);
}

// One byte carries both: low nibble opcode, bits 4-6 key count - 1.
constexpr uint8_t kTwoKeys = 0x10;

} // namespace

MELEE_TEST(AObj, RequestsPlaybackAndOwnsItsChannels)
{
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();
    HSD_AObj* animation = HSD_AObjAlloc();
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(animation != nullptr);
    REQUIRE(channel != nullptr);

    channel->startframe = 4;
    HSD_AObjSetFObj(animation, channel);
    HSD_AObjSetRate(animation, 0.5F);
    HSD_AObjSetEndFrame(animation, 30.0F);
    HSD_AObjSetFlags(animation, AOBJ_LOOP);
    HSD_AObjReqAnim(animation, 2.0F);

    CHECK_EQ(animation->curr_frame, 2.0F);
    CHECK_EQ(animation->framerate, 0.5F);
    CHECK_EQ(animation->end_frame, 30.0F);
    CHECK((HSD_AObjGetFlags(animation) & AOBJ_LOOP) != 0);
    CHECK((HSD_AObjGetFlags(animation) & AOBJ_NO_ANIM) == 0);
    CHECK_EQ(HSD_FObjGetState(channel), 1U);

    // Removing the animation object releases the channels it owns.
    HSD_AObjRemove(animation);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_AObjGetAllocData()), 0U);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_FObjGetAllocData()), 0U);
}

MELEE_TEST(AObj, HoldsTheFirstFrameBeforeAdvancing)
{
    std::vector<float> values;
    gValues = &values;
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();

    uint8_t stream[] = { kTwoKeys | HSD_A_OP_LIN, 0, 2, 4, 1 };
    HSD_AObj* animation = HSD_AObjAlloc();
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(animation != nullptr);
    REQUIRE(channel != nullptr);
    channel->ad_head = stream;
    channel->length = sizeof(stream);
    channel->frac_value = HSD_A_FRAC_U8;
    channel->frac_slope = HSD_A_FRAC_U8;

    HSD_AObjSetFObj(animation, channel);
    HSD_AObjSetRate(animation, 1.0F);
    HSD_AObjSetEndFrame(animation, 10.0F);
    HSD_AObjReqAnim(animation, 0.0F);

    // The first interpretation after a request plays the requested frame
    // itself; only later ticks advance the clock.
    HSD_AObjInterpretAnim(animation, nullptr, record);
    CHECK_EQ(animation->curr_frame, 0.0F);
    HSD_AObjInterpretAnim(animation, nullptr, record);
    CHECK_EQ(animation->curr_frame, 1.0F);
    REQUIRE(values.size() == 2);
    CHECK_EQ(values[0], 0.0F);
    CHECK_EQ(values[1], 2.0F);

    HSD_AObjRemove(animation);
    gValues = nullptr;
}

MELEE_TEST(AObj, RewindsALoopedAnimation)
{
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();
    HSD_AObj* animation = HSD_AObjAlloc();
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(animation != nullptr);
    REQUIRE(channel != nullptr);
    static uint8_t stream[] = { kTwoKeys | HSD_A_OP_CON, 1, 1, 2, 1 };
    channel->ad_head = stream;
    channel->length = sizeof(stream);
    channel->frac_value = HSD_A_FRAC_U8;

    HSD_AObjSetFObj(animation, channel);
    HSD_AObjSetRate(animation, 1.0F);
    HSD_AObjSetEndFrame(animation, 2.0F);
    HSD_AObjSetRewindFrame(animation, 0.0F);
    HSD_AObjSetFlags(animation, AOBJ_LOOP);
    HSD_AObjReqAnim(animation, 0.0F);

    HSD_AObjInterpretAnim(animation, nullptr, nullptr); // frame 0
    HSD_AObjInterpretAnim(animation, nullptr, nullptr); // frame 1
    HSD_AObjInterpretAnim(animation, nullptr, nullptr); // frame 2 -> rewind
    CHECK_EQ(animation->curr_frame, 0.0F);
    CHECK((HSD_AObjGetFlags(animation) & AOBJ_REWINDED) != 0);
    // A looping animation never stops on its own.
    CHECK((HSD_AObjGetFlags(animation) & AOBJ_NO_ANIM) == 0);

    HSD_AObjRemove(animation);
}

MELEE_TEST(AObj, StopsAtTheEndFrameWithoutLooping)
{
    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();
    HSD_AObj* animation = HSD_AObjAlloc();
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(animation != nullptr);
    REQUIRE(channel != nullptr);
    static uint8_t stream[] = { kTwoKeys | HSD_A_OP_CON, 1, 1, 2, 1 };
    channel->ad_head = stream;
    channel->length = sizeof(stream);
    channel->frac_value = HSD_A_FRAC_U8;

    HSD_AObjSetFObj(animation, channel);
    HSD_AObjSetRate(animation, 1.0F);
    HSD_AObjSetEndFrame(animation, 2.0F);
    HSD_AObjReqAnim(animation, 0.0F);

    HSD_AObjInterpretAnim(animation, nullptr, nullptr);
    HSD_AObjInterpretAnim(animation, nullptr, nullptr);
    HSD_AObjInterpretAnim(animation, nullptr, nullptr);
    CHECK((HSD_AObjGetFlags(animation) & AOBJ_NO_ANIM) != 0);

    // Further ticks are inert once the animation has stopped.
    const float stopped_frame = animation->curr_frame;
    HSD_AObjInterpretAnim(animation, nullptr, nullptr);
    CHECK_EQ(animation->curr_frame, stopped_frame);

    HSD_AObjRemove(animation);
}
