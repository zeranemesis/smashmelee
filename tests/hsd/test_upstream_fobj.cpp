#include "harness.hpp"

// The same twelve bytecode streams tests/hsd/test_fobj.cpp asserts of the
// port's interpreter, run through upstream's.  Both sides asserting one table
// is what keeps them from drifting: the port's interpreter was reading two
// bytes where HSD packs the opcode and the key count into one, and nothing
// short of this comparison would have shown it.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <melee/port/dolphin_compat.h>

extern "C" {
#include <sysdolphin/baselib/fobj.h>
#include <sysdolphin/baselib/objalloc.h>
}

namespace {

struct Update {
    int type = 0;
    float value = 0.0F;
};

std::vector<Update>* gUpdates = nullptr;

void record(void*, int type, HSD_ObjData* value)
{
    gUpdates->push_back({ type, value->fv });
}

// One byte carries both: low nibble opcode, bits 4-6 key count - 1.
constexpr uint8_t kTwoKeys = 0x10;
constexpr uint8_t kThreeKeys = 0x20;

} // namespace

MELEE_TEST(UpstreamFObj, MatchesThePortAcrossEveryOpcode)
{
    struct Case {
        const char* name;
        std::vector<uint8_t> bytecode;
        uint8_t value_fraction;
        std::vector<float> expected;
    };
    const std::vector<Case> cases = {
        { "constant, two keys",
          { kTwoKeys | HSD_A_OP_CON, 7, 1, 9, 1 }, HSD_A_FRAC_U8,
          { 7, 9, 9, 9, 9, 9, 9, 9 } },
        { "constant, three keys",
          { kThreeKeys | HSD_A_OP_CON, 1, 1, 2, 3, 3, 1 }, HSD_A_FRAC_U8,
          { 1, 2, 2, 2, 3, 3, 3, 3 } },
        { "linear, two keys",
          { kTwoKeys | HSD_A_OP_LIN, 0, 2, 4, 1 }, HSD_A_FRAC_U8,
          { 0, 2, 8, 12, 16, 20, 24, 28 } },
        { "linear, three keys",
          { kThreeKeys | HSD_A_OP_LIN, 0, 4, 10, 2, 20, 1 }, HSD_A_FRAC_U8,
          { 0, 2.5F, 5, 7.5F, 10, 15, 30, 40 } },
        { "spline without slopes",
          { kTwoKeys | HSD_A_OP_SPL0, 0, 2, 8, 1 }, HSD_A_FRAC_U8,
          { 0, 4, -32, -216, -640, -1400, -2592, -4312 } },
        { "spline with slopes",
          { kTwoKeys | HSD_A_OP_SPL, 0, 2, 2, 8, 1, 1 }, HSD_A_FRAC_U8,
          { 0, 4.25F, -24, -174, -520, -1140, -2112, -3514 } },
        { "slope then constant",
          { 0x00 | HSD_A_OP_SLP, 3, 1, kTwoKeys | HSD_A_OP_CON, 4, 2, 9, 1 },
          HSD_A_FRAC_U8, { 17, 17, 17, 17, 9, 9, 9, 9 } },
        { "key",
          { kTwoKeys | HSD_A_OP_KEY, 5, 2, 9, 1 }, HSD_A_FRAC_U8, { 5, 9 } },
        { "multi-byte wait",
          { kTwoKeys | HSD_A_OP_CON, 7, 0x84, 0x01, 9, 1 }, HSD_A_FRAC_U8,
          { 7, 7, 7, 7, 7, 7, 7, 7 } },
        { "constant pack then linear pack",
          { kTwoKeys | HSD_A_OP_CON, 2, 1, 4, 1,
            kTwoKeys | HSD_A_OP_LIN, 8, 2, 16, 1 },
          HSD_A_FRAC_U8, { 2, 4, 8, 12, 24, 32, 40, 48 } },
        { "two fraction bits",
          { kTwoKeys | HSD_A_OP_CON, 7, 1, 9, 1 },
          static_cast<uint8_t>(HSD_A_FRAC_U8 | 2),
          { 1.75F, 2.25F, 2.25F, 2.25F, 2.25F, 2.25F, 2.25F, 2.25F } },
        { "signed eight-bit values",
          { kTwoKeys | HSD_A_OP_CON, 7, 1, 9, 1 }, HSD_A_FRAC_S8,
          { 7, 9, 9, 9, 9, 9, 9, 9 } },
    };

    HSD_FObjInitAllocData();
    for (const Case& test_case : cases) {
        std::vector<Update> updates;
        gUpdates = &updates;
        std::vector<uint8_t> bytecode = test_case.bytecode;
        HSD_FObj* channel = HSD_FObjAlloc();
        REQUIRE(channel != nullptr);
        std::memset(channel, 0, sizeof(*channel));
        channel->ad_head = bytecode.data();
        channel->length = static_cast<u32>(bytecode.size());
        channel->frac_value = test_case.value_fraction;
        channel->frac_slope = test_case.value_fraction;
        channel->obj_type = 5;

        HSD_FObjReqAnimAll(channel, 0.0F);
        for (uint32_t tick = 0; tick < 8; ++tick) {
            HSD_FObjInterpretAnim(channel, nullptr, record,
                                  tick == 0 ? 0.0F : 1.0F);
        }
        if (updates.size() != test_case.expected.size()) {
            ::meleeboard::test::record_failure(
                __FILE__, __LINE__,
                std::string(test_case.name) + ": produced " +
                    std::to_string(updates.size()) + " values, the port "
                    "produced " + std::to_string(test_case.expected.size()));
        } else {
            for (std::size_t index = 0; index < updates.size(); ++index) {
                if (updates[index].value != test_case.expected[index]) {
                    ::meleeboard::test::record_failure(
                        __FILE__, __LINE__,
                        std::string(test_case.name) + " tick " +
                            std::to_string(index) + ": " +
                            std::to_string(updates[index].value) +
                            ", the port " +
                            std::to_string(test_case.expected[index]));
                }
            }
        }
        HSD_FObjRemove(channel);
        gUpdates = nullptr;
    }
}

MELEE_TEST(UpstreamFObj, RequestsEveryChannelInAChain)
{
    HSD_FObjInitAllocData();
    HSD_FObj* first = HSD_FObjAlloc();
    HSD_FObj* second = HSD_FObjAlloc();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    std::memset(first, 0, sizeof(*first));
    std::memset(second, 0, sizeof(*second));

    static uint8_t stream[] = { kTwoKeys | HSD_A_OP_CON, 1, 1, 2, 1 };
    first->next = second;
    first->startframe = 12;
    first->ad_head = stream;
    first->length = sizeof(stream);

    HSD_FObjReqAnimAll(first, 3.0F);
    // The request frame is relative to the channel's own start frame.
    CHECK_EQ(first->time, 15.0F);
    CHECK_EQ(HSD_FObjGetState(first), 1U);
    CHECK_EQ(HSD_FObjGetState(second), 1U);
    CHECK(first->ad == first->ad_head);

    HSD_FObjRemoveAll(first);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_FObjGetAllocData()), 0U);
}
