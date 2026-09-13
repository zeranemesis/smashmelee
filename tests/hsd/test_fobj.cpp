#include "harness.hpp"

#include <melee/sysdolphin/baselib/fobj.h>
#include <melee/sysdolphin/baselib/objalloc.h>

#include <cstdint>
#include <vector>

// HSD animation bytecode is a stream of packs.  Each pack is an opcode byte, a
// packed key count, and then one value/wait pair per key.  The wait counts are
// the frames the interpreter spends between a key and its successor.

namespace {

struct Update {
    uint32_t type = 0;
    float value = 0.0F;
};

std::vector<Update>* gUpdates = nullptr;

void record(void*, uint32_t type, HSD_ObjData* value)
{
    gUpdates->push_back({ type, value->fv });
}

// A pack header for two keys: ((0x10 >> 4) & 7) + 1.
// One byte carries both: low nibble opcode, bits 4-6 key count - 1.
constexpr uint8_t kTwoKeys = 0x10;

} // namespace

MELEE_TEST(FObj, RequestsEveryChannelInAChain)
{
    HSD_FObjInitAllocData();
    HSD_FObj* first = HSD_FObjAlloc();
    HSD_FObj* second = HSD_FObjAlloc();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

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

MELEE_TEST(FObj, InterpretsConstantKeys)
{
    std::vector<Update> updates;
    gUpdates = &updates;
    HSD_FObjInitAllocData();

    // Two u8 constant keys, each held for one frame.
    uint8_t stream[] = { kTwoKeys | HSD_A_OP_CON, 7, 1, 9, 1 };
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(channel != nullptr);
    channel->ad_head = stream;
    channel->length = sizeof(stream);
    channel->frac_value = HSD_A_FRAC_U8;
    channel->obj_type = 17;

    HSD_FObjReqAnimAll(channel, 0.0F);
    HSD_FObjInterpretAnim(channel, nullptr, record, 0.0F);
    REQUIRE(updates.size() == 1);
    CHECK_EQ(updates[0].type, 17U);
    CHECK_EQ(updates[0].value, 7.0F);

    HSD_FObjInterpretAnim(channel, nullptr, record, 1.0F);
    REQUIRE(updates.size() == 2);
    CHECK_EQ(updates[1].value, 9.0F);

    HSD_FObjRemove(channel);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_FObjGetAllocData()), 0U);
    gUpdates = nullptr;
}

MELEE_TEST(FObj, InterpolatesLinearKeys)
{
    std::vector<Update> updates;
    gUpdates = &updates;
    HSD_FObjInitAllocData();

    // Two linear keys, 0 then 4, held for two frames and then one.  The
    // expected values are the ones sysdolphin/baselib/fobj.c produces for
    // this exact stream: the slope is recomputed from the new segment length
    // each time a key rolls over, so the ramp steepens rather than stopping
    // at the second key's value.
    uint8_t stream[] = { kTwoKeys | HSD_A_OP_LIN, 0, 2, 4, 1 };
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(channel != nullptr);
    channel->ad_head = stream;
    channel->length = sizeof(stream);
    channel->frac_value = HSD_A_FRAC_U8;
    channel->frac_slope = HSD_A_FRAC_U8;
    channel->obj_type = 5;

    HSD_FObjReqAnimAll(channel, 0.0F);
    for (uint32_t tick = 0; tick < 4; ++tick) {
        HSD_FObjInterpretAnim(channel, nullptr, record, tick == 0 ? 0.0F : 1.0F);
    }
    REQUIRE(updates.size() == 4);
    CHECK_EQ(updates[0].value, 0.0F);
    CHECK_EQ(updates[1].value, 2.0F);
    CHECK_EQ(updates[2].value, 8.0F);
    CHECK_EQ(updates[3].value, 12.0F);

    HSD_FObjRemove(channel);
    gUpdates = nullptr;
}

MELEE_TEST(FObj, ScalesFixedPointValuesByTheirFraction)
{
    std::vector<Update> updates;
    gUpdates = &updates;
    HSD_FObjInitAllocData();

    // HSD_A_FRAC_U8 with two fraction bits divides the stored byte by four.
    uint8_t stream[] = { kTwoKeys | HSD_A_OP_CON, 10, 1, 20, 1 };
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(channel != nullptr);
    channel->ad_head = stream;
    channel->length = sizeof(stream);
    channel->frac_value = HSD_A_FRAC_U8 | 2;

    HSD_FObjReqAnimAll(channel, 0.0F);
    HSD_FObjInterpretAnim(channel, nullptr, record, 0.0F);
    REQUIRE(updates.size() == 1);
    CHECK_EQ(updates[0].value, 2.5F);

    HSD_FObjRemove(channel);
    gUpdates = nullptr;
}

MELEE_TEST(FObj, EndsOnAStreamWithNoBytecode)
{
    HSD_FObjInitAllocData();
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(channel != nullptr);
    channel->ad_head = nullptr;
    channel->length = 0;

    HSD_FObjReqAnimAll(channel, 0.0F);
    // A channel with no bytecode falls straight through to the terminal state
    // instead of reading past its (absent) buffer.  Reaching the end does not
    // store a state, so the requested one stands, exactly as upstream leaves
    // it.
    HSD_FObjInterpretAnim(channel, nullptr, nullptr, 0.0F);
    CHECK_EQ(HSD_FObjGetState(channel), 1U);

    HSD_FObjRemove(channel);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_FObjGetAllocData()), 0U);
}

MELEE_TEST(FObj, StopsOnTruncatedBytecode)
{
    HSD_FObjInitAllocData();
    // An opcode and key count with no value bytes behind them.
    uint8_t stream[] = { kTwoKeys | HSD_A_OP_CON };
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(channel != nullptr);
    channel->ad_head = stream;
    channel->length = sizeof(stream);
    channel->frac_value = HSD_A_FRAC_U8;

    HSD_FObjReqAnimAll(channel, 0.0F);
    HSD_FObjInterpretAnim(channel, nullptr, nullptr, 0.0F);
    CHECK_EQ(HSD_FObjGetState(channel), 0U);

    HSD_FObjRemove(channel);
}

MELEE_TEST(FObj, RejectsANonFiniteRate)
{
    HSD_FObjInitAllocData();
    uint8_t stream[] = { kTwoKeys | HSD_A_OP_CON, 7, 1, 9, 1 };
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(channel != nullptr);
    channel->ad_head = stream;
    channel->length = sizeof(stream);
    channel->frac_value = HSD_A_FRAC_U8;

    HSD_FObjReqAnimAll(channel, 0.0F);
    const float infinite = 1.0F / 0.0F;
    HSD_FObjInterpretAnim(channel, nullptr, nullptr, infinite);
    // The channel must not advance its clock to a non-finite value.
    CHECK_EQ(channel->time, 0.0F);
    CHECK_EQ(HSD_FObjGetState(channel), 1U);

    HSD_FObjRemove(channel);
}

// Every expectation below was taken by running the same bytes through
// sysdolphin/baselib/fobj.c compiled for x86-64, so this case pins the host
// interpreter to the original one rather than to a reading of the format.
// tools/upstream_native_spike.py builds that reference.
MELEE_TEST(FObj, MatchesUpstreamAcrossEveryOpcode)
{
    struct Case {
        const char* name;
        std::vector<uint8_t> bytecode;
        uint8_t value_fraction;
        std::vector<float> expected;
    };
    // The first byte of each pack carries the opcode in its low nibble and
    // the key count minus one in bits 4-6.
    const std::vector<Case> cases = {
        { "constant, two keys",
          { 0x10 | HSD_A_OP_CON, 7, 1, 9, 1 }, HSD_A_FRAC_U8,
          { 7, 9, 9, 9, 9, 9, 9, 9 } },
        { "constant, three keys",
          { 0x20 | HSD_A_OP_CON, 1, 1, 2, 3, 3, 1 }, HSD_A_FRAC_U8,
          { 1, 2, 2, 2, 3, 3, 3, 3 } },
        { "linear, two keys",
          { 0x10 | HSD_A_OP_LIN, 0, 2, 4, 1 }, HSD_A_FRAC_U8,
          { 0, 2, 8, 12, 16, 20, 24, 28 } },
        { "linear, three keys",
          { 0x20 | HSD_A_OP_LIN, 0, 4, 10, 2, 20, 1 }, HSD_A_FRAC_U8,
          { 0, 2.5F, 5, 7.5F, 10, 15, 30, 40 } },
        { "spline without slopes",
          { 0x10 | HSD_A_OP_SPL0, 0, 2, 8, 1 }, HSD_A_FRAC_U8,
          { 0, 4, -32, -216, -640, -1400, -2592, -4312 } },
        { "spline with slopes",
          { 0x10 | HSD_A_OP_SPL, 0, 2, 2, 8, 1, 1 }, HSD_A_FRAC_U8,
          { 0, 4.25F, -24, -174, -520, -1140, -2112, -3514 } },
        { "slope then constant",
          { 0x00 | HSD_A_OP_SLP, 3, 1, 0x10 | HSD_A_OP_CON, 4, 2, 9, 1 },
          HSD_A_FRAC_U8, { 17, 17, 17, 17, 9, 9, 9, 9 } },
        { "key",
          { 0x10 | HSD_A_OP_KEY, 5, 2, 9, 1 }, HSD_A_FRAC_U8, { 5, 9 } },
        { "multi-byte wait",
          { 0x10 | HSD_A_OP_CON, 7, 0x84, 0x01, 9, 1 }, HSD_A_FRAC_U8,
          { 7, 7, 7, 7, 7, 7, 7, 7 } },
        { "constant pack then linear pack",
          { 0x10 | HSD_A_OP_CON, 2, 1, 4, 1, 0x10 | HSD_A_OP_LIN, 8, 2, 16, 1 },
          HSD_A_FRAC_U8, { 2, 4, 8, 12, 24, 32, 40, 48 } },
        { "two fraction bits",
          { 0x10 | HSD_A_OP_CON, 7, 1, 9, 1 },
          static_cast<uint8_t>(HSD_A_FRAC_U8 | 2),
          { 1.75F, 2.25F, 2.25F, 2.25F, 2.25F, 2.25F, 2.25F, 2.25F } },
        { "signed eight-bit values",
          { 0x10 | HSD_A_OP_CON, 7, 1, 9, 1 }, HSD_A_FRAC_S8,
          { 7, 9, 9, 9, 9, 9, 9, 9 } },
    };

    HSD_FObjInitAllocData();
    for (const Case& test_case : cases) {
        std::vector<Update> updates;
        gUpdates = &updates;
        std::vector<uint8_t> bytecode = test_case.bytecode;
        HSD_FObj* channel = HSD_FObjAlloc();
        REQUIRE(channel != nullptr);
        channel->ad_head = bytecode.data();
        channel->length = static_cast<uint32_t>(bytecode.size());
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
                    std::to_string(updates.size()) + " values, upstream "
                    "produced " + std::to_string(test_case.expected.size()));
        } else {
            for (size_t index = 0; index < updates.size(); ++index) {
                if (updates[index].value != test_case.expected[index]) {
                    ::meleeboard::test::record_failure(
                        __FILE__, __LINE__,
                        std::string(test_case.name) + " tick " +
                            std::to_string(index) + ": " +
                            std::to_string(updates[index].value) +
                            ", upstream " +
                            std::to_string(test_case.expected[index]));
                }
            }
        }
        HSD_FObjRemove(channel);
        gUpdates = nullptr;
    }
}
