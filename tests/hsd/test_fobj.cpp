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
constexpr uint8_t kTwoKeyPack = 0x10;

} // namespace

MELEE_TEST(FObj, RequestsEveryChannelInAChain)
{
    HSD_FObjInitAllocData();
    HSD_FObj* first = HSD_FObjAlloc();
    HSD_FObj* second = HSD_FObjAlloc();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    static uint8_t stream[] = { HSD_A_OP_CON, kTwoKeyPack, 1, 1, 2, 1 };
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
    uint8_t stream[] = { HSD_A_OP_CON, kTwoKeyPack, 7, 1, 9, 1 };
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

    // A linear ramp from 0 to 4 spread over two frames.
    uint8_t stream[] = { HSD_A_OP_LIN, kTwoKeyPack, 0, 2, 4, 1 };
    HSD_FObj* channel = HSD_FObjAlloc();
    REQUIRE(channel != nullptr);
    channel->ad_head = stream;
    channel->length = sizeof(stream);
    channel->frac_value = HSD_A_FRAC_U8;
    channel->frac_slope = HSD_A_FRAC_U8;
    channel->obj_type = 5;

    HSD_FObjReqAnimAll(channel, 0.0F);
    HSD_FObjInterpretAnim(channel, nullptr, record, 0.0F);
    HSD_FObjInterpretAnim(channel, nullptr, record, 1.0F);
    HSD_FObjInterpretAnim(channel, nullptr, record, 1.0F);
    REQUIRE(updates.size() == 3);
    CHECK_EQ(updates[0].value, 0.0F);
    CHECK_EQ(updates[1].value, 2.0F);
    CHECK_EQ(updates[2].value, 4.0F);

    HSD_FObjRemove(channel);
    gUpdates = nullptr;
}

MELEE_TEST(FObj, ScalesFixedPointValuesByTheirFraction)
{
    std::vector<Update> updates;
    gUpdates = &updates;
    HSD_FObjInitAllocData();

    // HSD_A_FRAC_U8 with two fraction bits divides the stored byte by four.
    uint8_t stream[] = { HSD_A_OP_CON, kTwoKeyPack, 10, 1, 20, 1 };
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
    // A channel with no bytecode reaches the terminal state instead of reading
    // past its (absent) buffer.
    HSD_FObjInterpretAnim(channel, nullptr, nullptr, 0.0F);
    CHECK_EQ(HSD_FObjGetState(channel), 6U);

    HSD_FObjRemove(channel);
    CHECK_EQ(HSD_ObjAllocGetUsing(HSD_FObjGetAllocData()), 0U);
}

MELEE_TEST(FObj, StopsOnTruncatedBytecode)
{
    HSD_FObjInitAllocData();
    // An opcode and key count with no value bytes behind them.
    uint8_t stream[] = { HSD_A_OP_CON, kTwoKeyPack };
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
    uint8_t stream[] = { HSD_A_OP_CON, kTwoKeyPack, 7, 1, 9, 1 };
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
