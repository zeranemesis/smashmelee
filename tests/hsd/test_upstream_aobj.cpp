#include "harness.hpp"

// Upstream's animation-object playback, against the seven traces
// tests/hsd/test_aobj.cpp asserts of the port's.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/fobj.h>
#include <sysdolphin/baselib/objalloc.h>
}

namespace {

std::vector<float>* gValues = nullptr;

void record(void*, int, HSD_ObjData* value)
{
    gValues->push_back(value->fv);
}

// Formats like printf's %.4g, which is how the reference trace was produced.
std::string trim(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4g", static_cast<double>(value));
    return buffer;
}

constexpr uint8_t kThreeKeys = 0x20;

} // namespace

MELEE_TEST(UpstreamAObj, MatchesThePortAcrossPlaybackModes)
{
    struct Case {
        const char* name;
        u32 flags;
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
        std::vector<uint8_t> bytecode = {
            static_cast<uint8_t>(kThreeKeys | HSD_A_OP_CON), 1, 1, 2, 1, 3, 1
        };
        HSD_AObj* animation = HSD_AObjAlloc();
        HSD_FObj* channel = HSD_FObjAlloc();
        REQUIRE(animation != nullptr);
        REQUIRE(channel != nullptr);
        std::memset(channel, 0, sizeof(*channel));
        channel->ad_head = bytecode.data();
        channel->length = static_cast<u32>(bytecode.size());
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
            const u32 flags = HSD_AObjGetFlags(animation);
            trace += (flags & AOBJ_NO_ANIM) ? "S" : "-";
            trace += (flags & AOBJ_REWINDED) ? "R" : "-";
            trace += (flags & AOBJ_LOOP) ? "L" : "-";
            trace += "]";
        }
        gValues = nullptr;
        if (trace != test_case.expected) {
            ::meleeboard::test::record_failure(
                __FILE__, __LINE__,
                std::string(test_case.name) + ":\n             upstream " +
                    trace + "\n             port     " + test_case.expected);
        }
        HSD_AObjRemove(animation);
    }
}
