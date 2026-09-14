#include "harness.hpp"

// Upstream's audio layer, running.
//
// docs/PLAN.md called audio its one unmeasured risk and asked for a spike of
// five AX calls.  Measuring the boundary made the spike unnecessary and the
// whole layer reachable instead.  Three upstream units touch the sound SDK --
// axdriver.c, synth.c and melee/lb/lbaudio_ax.c -- and between them they
// reference 35 entry points: 22 in AX and 13 in AXFX.  The DSP library is
// referenced nowhere at all, because AX talks to the DSP on the game's
// behalf.  AI is four calls.  And the whole ARAM surface the game uses is
// already implemented by Aurora for the shipping build.
//
// So tests/hsd/ax_record.hpp supplies that surface and
// src/melee_port/upstream/aram.cpp supplies the auxiliary RAM, and upstream's
// own sound driver comes up through its own init.

#include <cstdio>
#include <string>
#include <vector>

#include <melee/port/dolphin_compat.h>

extern "C" {
#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <sysdolphin/baselib/axdriver.h>
#include <sysdolphin/baselib/synth.h>

// What tests/hsd/upstream_host.c recorded of the analog output stage.
unsigned long melee_ai_dsp_sample_rate(void);
unsigned char melee_ai_stream_vol_left(void);
}

#include "aram.hpp"
#include "ax_record.hpp"
#include "os_scheduler.hpp"

namespace ax = meleeboard::test::ax;
namespace os = meleeboard::os;

namespace {

// The ARAM allocation stack.  ARInit keeps the offset of every live block
// here; the game gives it 64 entries, and so does this.
u32 g_ar_stack[64];

// Melee's own audio bring-up, from lbaudio_ax.c lines 2098-2122.  The three
// SDK calls come first -- ARAM, its queue, and the analog output stage -- and
// then the driver, with the voice count, priority, sample rate and bank size
// the game passes.
void bring_audio_up()
{
    ax::reset();
    os::reset_aram();
    os::reset();

    ARInit(g_ar_stack, 64);
    ARQInit();
    AIInit(nullptr);
    AXDriver_8038E498(AX_MAX_VOICES, 0, 0x40, 0x100000);
}

// An ARQ completion callback takes the request it completed, which is not
// what this case needs to know -- only that it ran.
bool g_dma_completed = false;
void note_dma_completion(uintptr_t) { g_dma_completed = true; }

} // namespace

MELEE_TEST(UpstreamAudio, BringsTheDriverUpThroughItsOwnInit)
{
    bring_audio_up();

    // The whole of it, in order.  This is upstream's AXDriver_8038E498 and
    // HSD_SynthInit talking to AX with nothing of the port's in between: the
    // DSP comes up, the frame callback is registered, both auxiliary buses
    // are cleared twice -- once each by the two AXDriverSetupAux calls -- and
    // the effect allocator hooks are installed last.
    std::string shape;
    for (const std::string& line : ax::trace()) {
        if (!shape.empty()) {
            shape += '\n';
        }
        shape += line;
    }
    CHECK_EQ(shape, std::string("AXInit()\n"
                                "AXRegisterCallback(p0)\n"
                                "AXRegisterAuxACallback(null, null)\n"
                                "AXRegisterAuxACallback(null, null)\n"
                                "AXRegisterAuxBCallback(null, null)\n"
                                "AXRegisterAuxBCallback(null, null)\n"
                                "AXFXSetHooks(p1, p2)"));

    // The frame callback is HSD_SynthCallback, and a mixer would drive it 
    // once per AX frame.  There is no mixer, so the recorder holds it and
    // never calls it -- what a frame would produce is not something this
    // target can claim to know.
    CHECK(ax::frame_callback() != nullptr);

    // Nothing has acquired a voice yet: the driver comes up idle, and a voice
    // is taken when a sound plays.
    CHECK_EQ(ax::voices_in_use(), std::size_t(0));

    // AX runs at 32 kHz, which is sample-rate selector zero.
    CHECK_EQ(melee_ai_dsp_sample_rate(), 0UL);
    // And the stream volume is set from the synthesizer's own master level.
    CHECK(melee_ai_stream_vol_left() != 0);
}

MELEE_TEST(UpstreamAudio, CarvesItsSoundBanksOutOfTheAuxiliaryRam)
{
    bring_audio_up();

    // HSD_SynthInit takes three blocks off ARAM's bump allocator, in this
    // order: 0x500 bytes for the synthesizer's own table, the bank size it
    // was given, and 0x30000 for its streaming buffer.  They are contiguous,
    // starting past the 0x4000 the allocation stack occupies -- which is the
    // console's layout, and the reason an ARAM offset of zero is not a block.
    CHECK_EQ(os::aram_used(), 0x500u + 0x100000u + 0x30000u);

    // Nothing here can read ARAM the way the CPU cannot on hardware, so the
    // check that the blocks landed is that the range is addressable at all.
    CHECK(os::aram_bytes(0x4000, 0x500) != nullptr);
    CHECK(os::aram_bytes(0x4500, 0x100000) != nullptr);
    CHECK(os::aram_bytes(0x104500, 0x30000) != nullptr);
}

MELEE_TEST(UpstreamAudio, CompletesADmaAtAnInterruptAndNotBefore)
{
    bring_audio_up();
    REQUIRE_EQ(os::aram_queue_depth(), std::size_t(0));

    unsigned char source[64];
    for (std::size_t index = 0; index < sizeof source; ++index) {
        source[index] = static_cast<unsigned char>(index + 1);
    }

    ARQRequest request{};
    g_dma_completed = false;
    ARQPostRequest(&request, 0, ARQ_TYPE_MRAM_TO_ARAM, ARQ_PRIORITY_HIGH,
                   reinterpret_cast<uintptr_t>(source),
                   static_cast<uintptr_t>(0x4000), sizeof source,
                   &note_dma_completion);

    // Queued, not performed.  This is the correction upstream forced: the
    // first version of aram.cpp copied and ran the callback inside
    // ARQPostRequest, and devcom.c crashed, because HSD_DevComARAMWakeUp
    // posts a request and then advances the very bookkeeping the callback
    // tears down -- `aramDC->dest += xfer_size` on the next line.  On the
    // console a DMA completes at an interrupt, strictly after the post
    // returns.
    CHECK_EQ(os::aram_queue_depth(), std::size_t(1));
    CHECK(!g_dma_completed);
    CHECK_EQ(os::aram_transfers().size(), std::size_t(0));

    // And a completion is an interrupt, so a critical section holds it off.
    const BOOL enabled = OSDisableInterrupts();
    CHECK_EQ(os::service_aram_queue(), std::size_t(0));
    CHECK_EQ(os::aram_queue_depth(), std::size_t(1));
    CHECK(!g_dma_completed);

    OSRestoreInterrupts(enabled);
    CHECK_EQ(os::service_aram_queue(), std::size_t(1));
    CHECK_EQ(os::aram_queue_depth(), std::size_t(0));
    CHECK(g_dma_completed);

    // The bytes moved into ARAM, which is the only way anything reaches it:
    // the CPU cannot address ARAM on the console at all.
    REQUIRE_EQ(os::aram_transfers().size(), std::size_t(1));
    CHECK_EQ(os::aram_transfers()[0].length, (std::uint32_t) sizeof source);
    const unsigned char* landed = os::aram_bytes(0x4000, sizeof source);
    REQUIRE(landed != nullptr);
    for (std::size_t index = 0; index < sizeof source; ++index) {
        if (landed[index] != source[index]) {
            CHECK(false);
            break;
        }
    }
}

MELEE_TEST(UpstreamAudio, LeavesTheVoiceBlockAsTheSettersWroteIt)
{
    // The AXPB is the complete per-voice DSP state, and Melee's own
    // <dolphin/ax.h> specifies all of it -- which is what makes a host mixer
    // a bounded job rather than an open question.  These are the setters
    // synth.c actually uses, and this pins what each one leaves behind,
    // because a mixer reads exactly these fields.
    ax::reset();

    AXVPB* voice = AXAcquireVoice(15, nullptr, 0x1234);
    REQUIRE(voice != nullptr);
    CHECK_EQ(voice->priority, 15);
    CHECK_EQ(voice->userContext, 0x1234u);
    CHECK_EQ(voice->index, 0u);
    CHECK_EQ(ax::voices_in_use(), std::size_t(1));

    AXSetVoiceState(voice, 1);
    CHECK_EQ(voice->pb.state, 1);

    // An address is split across two 16-bit halves, high first, because the
    // DSP's registers are 16 bits wide.
    AXSetVoiceCurrentAddr(voice, 0x00123456);
    CHECK_EQ(voice->pb.addr.currentAddressHi, 0x0012);
    CHECK_EQ(voice->pb.addr.currentAddressLo, 0x3456);
    AXSetVoiceEndAddr(voice, 0x00FEDCBA);
    CHECK_EQ(voice->pb.addr.endAddressHi, 0x00FE);
    CHECK_EQ(voice->pb.addr.endAddressLo, 0xDCBA);
    AXSetVoiceLoopAddr(voice, 0x00001000);
    CHECK_EQ(voice->pb.addr.loopAddressHi, 0x0000);
    CHECK_EQ(voice->pb.addr.loopAddressLo, 0x1000);
    AXSetVoiceLoop(voice, 1);
    CHECK_EQ(voice->pb.addr.loopFlag, 1);

    // The resampling ratio is 16.16 fixed point: one source sample per output
    // sample is 0x00010000, so half speed is 0x00008000.
    AXSetVoiceSrcRatio(voice, 0.5F);
    CHECK_EQ(voice->pb.src.ratioHi, 0x0000);
    CHECK_EQ(voice->pb.src.ratioLo, 0x8000);

    AXPBVE envelope{};
    envelope.currentVolume = 0x4000;
    envelope.currentDelta = -16;
    AXSetVoiceVe(voice, &envelope);
    CHECK_EQ(voice->pb.ve.currentVolume, 0x4000);
    CHECK_EQ(voice->pb.ve.currentDelta, -16);
    AXSetVoiceVeDelta(voice, 32);
    CHECK_EQ(voice->pb.ve.currentDelta, 32);

    AXPBMIX mix{};
    mix.vL = 0x7FFF;
    mix.vR = 0x1000;
    AXSetVoiceMix(voice, &mix);
    CHECK_EQ(voice->pb.mix.vL, 0x7FFF);
    CHECK_EQ(voice->pb.mix.vR, 0x1000);

    // Interaural time difference: the voice's own buffer is what the DSP
    // delays through, so turning it on points the block at that buffer.
    AXSetVoiceItdOn(voice);
    CHECK_EQ(voice->pb.itd.flag, 1);
    CHECK(voice->pb.itd.bufferHi != 0 || voice->pb.itd.bufferLo != 0);
    AXSetVoiceItdTarget(voice, 3, 5);
    CHECK_EQ(voice->pb.itd.targetShiftL, 3);
    CHECK_EQ(voice->pb.itd.targetShiftR, 5);

    // Releasing it returns the slot, and the next acquisition gets it back
    // with a cleared block -- which is what stops one sound inheriting
    // another's loop point.
    AXFreeVoice(voice);
    CHECK_EQ(ax::voices_in_use(), std::size_t(0));
    AXVPB* again = AXAcquireVoice(1, nullptr, 0);
    REQUIRE(again == voice);
    CHECK_EQ(again->pb.addr.loopFlag, 0);
    CHECK_EQ(again->pb.src.ratioLo, 0);
    AXFreeVoice(again);
}

MELEE_TEST(UpstreamAudio, RunsOutOfVoicesRatherThanStealingOne)
{
    ax::reset();

    // Sixty-four is AX_MAX_VOICES, and synth.c indexes its own node table by
    // a voice's index, so the pool cannot be larger.
    for (int taken = 0; taken < AX_MAX_VOICES; ++taken) {
        REQUIRE(AXAcquireVoice(1, nullptr, 0) != nullptr);
    }
    CHECK_EQ(ax::voices_in_use(), std::size_t(AX_MAX_VOICES));

    // The console drops a lower-priority voice at this point.  The recorder
    // does not, deliberately: a test that exhausts the pool should see that
    // it did rather than get a silently stolen voice, and the trace says so.
    CHECK(AXAcquireVoice(1, nullptr, 0) == nullptr);
    REQUIRE(ax::count("AXAcquireVoice") == (std::size_t) AX_MAX_VOICES + 1);
    const std::string* last =
        ax::call("AXAcquireVoice", (std::size_t) AX_MAX_VOICES);
    REQUIRE(last != nullptr);
    CHECK(last->find("-> none") != std::string::npos);
}
