#include "harness.hpp"

#include <melee/sysdolphin/baselib/aobj.h>
#include <melee/sysdolphin/baselib/fobj.h>
#include <melee/sysdolphin/baselib/objalloc.h>

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<float>* gValues = nullptr;

void record(void*, uint32_t, HSD_ObjData* value)
{
    gValues->push_back(value->fv);
}

// One byte carries both: low nibble opcode, bits 4-6 key count - 1.
constexpr uint8_t kTwoKeys = 0x10;

// Formats like printf's %.4g, which is how the reference trace was produced.
std::string trim(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4g", static_cast<double>(value));
    return buffer;
}

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

// The expected traces below were produced by sysdolphin/baselib/aobj.c
// compiled for x86-64; see tools/upstream_native_spike.py.  Each entry is one
// tick: the value the channel published, the animation frame after the tick,
// and the stopped/rewound/loop flags.
MELEE_TEST(AObj, MatchesUpstreamAcrossPlaybackModes)
{
    struct Case {
        const char* name;
        uint32_t flags;
        float end_frame;
        float rewind_frame;
        float rate;
        uint32_t ticks;
        const char* expected;
    };
    const Case cases[] = {
        { "once, rate 1", 0, 3.0F, 0.0F, 1.0F, 6,
          "[1|f=0|---][2|f=1|---][3|f=2|---][3|f=3|S--][|f=3|S--][|f=3|S--]" },
        { "once, rate 0.5", 0, 3.0F, 0.0F, 0.5F, 8,
          "[1|f=0|---][1|f=0.5|---][2|f=1|---][2|f=1.5|---][3|f=2|---]"
          "[3|f=2.5|---][3|f=3|S--][|f=3|S--]" },
        { "loop, rewind 0", AOBJ_LOOP, 3.0F, 0.0F, 1.0F, 8,
          "[1|f=0|--L][2|f=1|--L][3|f=2|--L][1|f=0|-RL][2|f=1|--L]"
          "[3|f=2|--L][1|f=0|-RL][2|f=1|--L]" },
        { "loop, rewind 1", AOBJ_LOOP, 3.0F, 1.0F, 1.0F, 8,
          "[1|f=0|--L][2|f=1|--L][3|f=2|--L][2|f=1|-RL][3|f=2|--L]"
          "[2|f=1|-RL][3|f=2|--L][2|f=1|-RL]" },
        { "loop, rewind == end", AOBJ_LOOP, 3.0F, 3.0F, 1.0F, 6,
          "[1|f=0|--L][2|f=1|--L][3|f=2|--L][3|f=3|-RL][3|f=3|-RL]"
          "[3|f=3|-RL]" },
        { "no update", AOBJ_NO_UPDATE, 4.0F, 0.0F, 1.0F, 5,
          "[|f=0|---][|f=1|---][|f=2|---][|f=3|---][|f=4|S--]" },
        { "loop, no update", AOBJ_LOOP | AOBJ_NO_UPDATE, 3.0F, 0.0F, 1.0F, 6,
          "[|f=0|--L][|f=1|--L][|f=2|--L][|f=0|-RL][|f=1|--L][|f=2|--L]" },
    };

    HSD_AObjInitAllocData();
    HSD_FObjInitAllocData();
    for (const Case& test_case : cases) {
        // Three constant keys, each held for one frame.
        std::vector<uint8_t> bytecode = { 0x20 | HSD_A_OP_CON, 1, 1, 2, 1, 3, 1 };
        HSD_AObj* animation = HSD_AObjAlloc();
        HSD_FObj* channel = HSD_FObjAlloc();
        REQUIRE(animation != nullptr);
        REQUIRE(channel != nullptr);
        channel->ad_head = bytecode.data();
        channel->length = static_cast<uint32_t>(bytecode.size());
        channel->frac_value = HSD_A_FRAC_U8;
        channel->frac_slope = HSD_A_FRAC_U8;
        channel->obj_type = 5;
        HSD_AObjSetFObj(animation, channel);
        HSD_AObjSetRate(animation, test_case.rate);
        HSD_AObjSetEndFrame(animation, test_case.end_frame);
        HSD_AObjSetRewindFrame(animation, test_case.rewind_frame);
        if (test_case.flags != 0) {
            HSD_AObjSetFlags(animation, test_case.flags);
        }
        HSD_AObjReqAnim(animation, 0.0F);

        std::vector<float> values;
        gValues = &values;
        std::string trace;
        for (uint32_t tick = 0; tick < test_case.ticks; ++tick) {
            values.clear();
            HSD_AObjInterpretAnim(animation, nullptr, record);
            trace += "[";
            for (float value : values) trace += trim(value);
            trace += "|f=" + trim(animation->curr_frame) + "|";
            const uint32_t flags = HSD_AObjGetFlags(animation);
            trace += (flags & AOBJ_NO_ANIM) ? "S" : "-";
            trace += (flags & AOBJ_REWINDED) ? "R" : "-";
            trace += (flags & AOBJ_LOOP) ? "L" : "-";
            trace += "]";
        }
        gValues = nullptr;
        if (trace != test_case.expected) {
            ::meleeboard::test::record_failure(
                __FILE__, __LINE__,
                std::string(test_case.name) + ":\n             port     " +
                    trace + "\n             upstream " + test_case.expected);
        }
        HSD_AObjRemove(animation);
    }
}
