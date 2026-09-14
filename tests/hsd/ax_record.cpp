#include "ax_record.hpp"

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <dolphin/axfx.h>
}

namespace {

std::vector<std::string> g_trace;
std::vector<const void*> g_pointers;

// The voice pool.  Sixty-four is AX_MAX_VOICES, which is what the console
// has; synth.c indexes its own node table by a voice's `index`, so the
// numbering has to be stable and inside that range.
AXVPB g_voices[AX_MAX_VOICES];
bool g_held[AX_MAX_VOICES];
AXPBITDBUFFER g_itd_buffers[AX_MAX_VOICES];

void* g_frame_callback = nullptr;
void* g_aux_a_callback = nullptr;
void* g_aux_b_callback = nullptr;

// Returns the pool to its initial state without touching the trace: AXInit
// does that, and the call that caused it belongs in the record.
void reset_voices()
{
    for (u32 index = 0; index < AX_MAX_VOICES; ++index) {
        std::memset(&g_voices[index], 0, sizeof g_voices[index]);
        std::memset(&g_itd_buffers[index], 0, sizeof g_itd_buffers[index]);
        g_voices[index].index = index;
        g_voices[index].itdBuffer = &g_itd_buffers[index];
        g_held[index] = false;
    }
    g_frame_callback = nullptr;
    g_aux_a_callback = nullptr;
    g_aux_b_callback = nullptr;
}

// Pointers are printed as small indices assigned by first appearance, so a
// recorded frame compares equal across runs and hosts.  The GX recorder does
// the same, for the same reason.
std::string tag(const void* pointer)
{
    if (pointer == nullptr) {
        return "null";
    }
    for (std::size_t index = 0; index < g_pointers.size(); ++index) {
        if (g_pointers[index] == pointer) {
            return "p" + std::to_string(index);
        }
    }
    g_pointers.push_back(pointer);
    return "p" + std::to_string(g_pointers.size() - 1);
}

std::string field(const void* value) { return tag(value); }
std::string field(u32 value) { return std::to_string(value); }
std::string field(s32 value) { return std::to_string(value); }
std::string field(u16 value) { return std::to_string(value); }
std::string field(unsigned long value) { return std::to_string(value); }

// A float prints with a fixed number of digits so a ratio compares as text.
std::string field(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.6g", static_cast<double>(value));
    return buffer;
}

void append(std::string line) { g_trace.push_back(std::move(line)); }

template <typename... Args> void record(const char* name, Args... args)
{
    std::string line = name;
    line += '(';
    const std::string parts[] = { field(args)... };
    for (std::size_t index = 0; index < sizeof...(Args); ++index) {
        if (index != 0) {
            line += ", ";
        }
        line += parts[index];
    }
    line += ')';
    append(std::move(line));
}

void record(const char* name)
{
    append(std::string(name) + "()");
}

// A voice is named by its index rather than its address: the pool is static,
// so an address would be the same every run but would say less.
std::string voice_name(const AXVPB* voice)
{
    if (voice == nullptr) {
        return "v?";
    }
    return "v" + std::to_string(voice->index);
}

} // namespace

namespace meleeboard::test::ax {

void reset()
{
    g_trace.clear();
    g_pointers.clear();
    reset_voices();
}

const std::vector<std::string>& trace() { return g_trace; }

std::string joined()
{
    std::string text;
    for (const std::string& line : g_trace) {
        text += line;
        text += '\n';
    }
    return text;
}

std::size_t count(const char* name)
{
    const std::string prefix = std::string(name) + "(";
    std::size_t total = 0;
    for (const std::string& line : g_trace) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            ++total;
        }
    }
    return total;
}

const std::string* call(const char* name, std::size_t index)
{
    const std::string prefix = std::string(name) + "(";
    std::size_t seen = 0;
    for (const std::string& line : g_trace) {
        if (line.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        if (seen++ == index) {
            return &line;
        }
    }
    return nullptr;
}

std::vector<std::string> names()
{
    std::vector<std::string> result;
    result.reserve(g_trace.size());
    for (const std::string& line : g_trace) {
        result.push_back(line.substr(0, line.find('(')));
    }
    return result;
}

std::size_t voices_in_use()
{
    std::size_t held = 0;
    for (bool one : g_held) {
        held += one ? 1 : 0;
    }
    return held;
}

const AXVPB* voice(std::size_t index)
{
    return index < AX_MAX_VOICES ? &g_voices[index] : nullptr;
}

void* frame_callback() { return g_frame_callback; }
void* aux_a_callback() { return g_aux_a_callback; }
void* aux_b_callback() { return g_aux_b_callback; }

} // namespace meleeboard::test::ax

// ---------------------------------------------------------------------------
// AX
//
// The setters write the fields their names say and set the sync flag that
// tells the DSP what changed.  What the console's implementation additionally
// resets inside the block is not reproduced: its sources are in neither tree,
// and inventing them would be worse than leaving them out and saying so.

extern "C" {

void AXInit(void)
{
    // The console's AXInit brings the DSP up and clears every voice.  Here it
    // returns the pool to its initial state and leaves the trace alone, so a
    // test sees the initialization it asked for.
    reset_voices();
    record("AXInit");
}

AXVPB* AXAcquireVoice(u32 priority, void (*callback)(void*), u32 userContext)
{
    for (u32 index = 0; index < AX_MAX_VOICES; ++index) {
        if (g_held[index]) {
            continue;
        }
        AXVPB& voice = g_voices[index];
        std::memset(&voice.pb, 0, sizeof voice.pb);
        voice.priority = static_cast<int>(priority);
        voice.callback = callback;
        voice.userContext = userContext;
        voice.sync = 0;
        g_held[index] = true;
        record("AXAcquireVoice", priority, userContext);
        g_trace.back() += " -> " + voice_name(&voice);
        return &voice;
    }

    // The console drops a lower-priority voice rather than failing.  The
    // recorder does not: a test that runs out of voices should see that it
    // did, not a silently stolen one.
    record("AXAcquireVoice", priority, userContext);
    g_trace.back() += " -> none";
    return nullptr;
}

void AXFreeVoice(AXVPB* p)
{
    if (p == nullptr) {
        record("AXFreeVoice", static_cast<const void*>(nullptr));
        return;
    }
    append("AXFreeVoice(" + voice_name(p) + ")");
    if (p->index < AX_MAX_VOICES) {
        g_held[p->index] = false;
    }
}

void AXSetVoicePriority(AXVPB* p, u32 priority)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoicePriority(" + voice_name(p) + ", " +
           std::to_string(priority) + ")");
    p->priority = static_cast<int>(priority);
}

void AXSetVoiceState(AXVPB* p, u16 state)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoiceState(" + voice_name(p) + ", " + std::to_string(state) +
           ")");
    p->pb.state = state;
    p->sync |= AX_SYNC_FLAG_COPYSTATE;
}

void AXSetVoiceType(AXVPB* p, u16 type)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoiceType(" + voice_name(p) + ", " + std::to_string(type) +
           ")");
    p->pb.type = type;
    p->sync |= AX_SYNC_FLAG_COPYTYPE;
}

void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix)
{
    if (p == nullptr || mix == nullptr) {
        return;
    }
    // The mixer is twenty fields, and which of them a voice uses is the whole
    // content of the call, so the left and right volumes go in the line and
    // the block goes into the voice.
    append("AXSetVoiceMix(" + voice_name(p) + ", L=" +
           std::to_string(mix->vL) + ", R=" + std::to_string(mix->vR) +
           ", S=" + std::to_string(mix->vS) + ")");
    p->pb.mix = *mix;
    p->sync |= AX_SYNC_FLAG_COPYAXPBMIX;
}

void AXSetVoiceVe(AXVPB* p, AXPBVE* ve)
{
    if (p == nullptr || ve == nullptr) {
        return;
    }
    append("AXSetVoiceVe(" + voice_name(p) + ", " +
           std::to_string(ve->currentVolume) + ", " +
           std::to_string(ve->currentDelta) + ")");
    p->pb.ve = *ve;
    p->sync |= AX_SYNC_FLAG_COPYVOL;
}

void AXSetVoiceVeDelta(AXVPB* p, s16 delta)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoiceVeDelta(" + voice_name(p) + ", " +
           std::to_string(delta) + ")");
    p->pb.ve.currentDelta = delta;
    p->sync |= AX_SYNC_FLAG_COPYVOL;
}

void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr)
{
    if (p == nullptr || addr == nullptr) {
        return;
    }
    append("AXSetVoiceAddr(" + voice_name(p) + ", format=" +
           std::to_string(addr->format) + ", loop=" +
           std::to_string(addr->loopFlag) + ")");
    p->pb.addr = *addr;
    p->sync |= AX_SYNC_FLAG_COPYADDR;
}

void AXSetVoiceLoop(AXVPB* p, u16 loop)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoiceLoop(" + voice_name(p) + ", " + std::to_string(loop) +
           ")");
    p->pb.addr.loopFlag = loop;
    p->sync |= AX_SYNC_FLAG_COPYLOOP;
}

void AXSetVoiceLoopAddr(AXVPB* p, u32 addr)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoiceLoopAddr(" + voice_name(p) + ", " +
           std::to_string(addr) + ")");
    p->pb.addr.loopAddressHi = static_cast<u16>(addr >> 16);
    p->pb.addr.loopAddressLo = static_cast<u16>(addr & 0xFFFFu);
    p->sync |= AX_SYNC_FLAG_COPYLOOPADDR;
}

void AXSetVoiceEndAddr(AXVPB* p, u32 addr)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoiceEndAddr(" + voice_name(p) + ", " + std::to_string(addr) +
           ")");
    p->pb.addr.endAddressHi = static_cast<u16>(addr >> 16);
    p->pb.addr.endAddressLo = static_cast<u16>(addr & 0xFFFFu);
    p->sync |= AX_SYNC_FLAG_COPYENDADDR;
}

void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoiceCurrentAddr(" + voice_name(p) + ", " +
           std::to_string(addr) + ")");
    p->pb.addr.currentAddressHi = static_cast<u16>(addr >> 16);
    p->pb.addr.currentAddressLo = static_cast<u16>(addr & 0xFFFFu);
    p->sync |= AX_SYNC_FLAG_COPYCURADDR;
}

void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* adpcm)
{
    if (p == nullptr || adpcm == nullptr) {
        return;
    }
    append("AXSetVoiceAdpcm(" + voice_name(p) + ", gain=" +
           std::to_string(adpcm->gain) + ", pred=" +
           std::to_string(adpcm->pred_scale) + ")");
    p->pb.adpcm = *adpcm;
    p->sync |= AX_SYNC_FLAG_COPYADPCM;
}

void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* adpcmloop)
{
    if (p == nullptr || adpcmloop == nullptr) {
        return;
    }
    append("AXSetVoiceAdpcmLoop(" + voice_name(p) + ", " +
           std::to_string(adpcmloop->loop_pred_scale) + ")");
    p->pb.adpcmLoop = *adpcmloop;
    p->sync |= AX_SYNC_FLAG_COPYADPCMLOOP;
}

void AXSetVoiceSrc(AXVPB* p, AXPBSRC* src_)
{
    if (p == nullptr || src_ == nullptr) {
        return;
    }
    append("AXSetVoiceSrc(" + voice_name(p) + ", ratio=" +
           std::to_string((static_cast<u32>(src_->ratioHi) << 16) |
                          src_->ratioLo) +
           ")");
    p->pb.src = *src_;
    p->sync |= AX_SYNC_FLAG_COPYSRC;
}

void AXSetVoiceSrcRatio(AXVPB* p, float ratio)
{
    if (p == nullptr) {
        return;
    }
    // The ratio is 16.16 fixed point: one sample of source per sample of
    // output is 0x00010000.
    const u32 fixed = static_cast<u32>(ratio * 65536.0F);
    append("AXSetVoiceSrcRatio(" + voice_name(p) + ", " +
           std::to_string(fixed) + ")");
    p->pb.src.ratioHi = static_cast<u16>(fixed >> 16);
    p->pb.src.ratioLo = static_cast<u16>(fixed & 0xFFFFu);
    p->sync |= AX_SYNC_FLAG_COPYRATIO;
}

void AXSetVoiceItdOn(AXVPB* p)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoiceItdOn(" + voice_name(p) + ")");
    p->pb.itd.flag = 1;
    const auto buffer = reinterpret_cast<std::uintptr_t>(p->itdBuffer);
    p->pb.itd.bufferHi = static_cast<u16>((buffer >> 16) & 0xFFFFu);
    p->pb.itd.bufferLo = static_cast<u16>(buffer & 0xFFFFu);
    p->sync |= AX_SYNC_FLAG_COPYITD;
}

void AXSetVoiceItdTarget(AXVPB* p, u16 lShift, u16 rShift)
{
    if (p == nullptr) {
        return;
    }
    append("AXSetVoiceItdTarget(" + voice_name(p) + ", " +
           std::to_string(lShift) + ", " + std::to_string(rShift) + ")");
    p->pb.itd.targetShiftL = lShift;
    p->pb.itd.targetShiftR = rShift;
    p->sync |= AX_SYNC_FLAG_COPYITD;
}

void AXRegisterCallback(void (*callback)())
{
    record("AXRegisterCallback", reinterpret_cast<const void*>(callback));
    g_frame_callback = reinterpret_cast<void*>(callback);
}

void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context)
{
    record("AXRegisterAuxACallback", reinterpret_cast<const void*>(callback),
           static_cast<const void*>(context));
    g_aux_a_callback = reinterpret_cast<void*>(callback);
}

void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context)
{
    record("AXRegisterAuxBCallback", reinterpret_cast<const void*>(callback),
           static_cast<const void*>(context));
    g_aux_b_callback = reinterpret_cast<void*>(callback);
}

// ---------------------------------------------------------------------------
// AXFX
//
// Every one of these is an init or a shutdown.  The game never calls an effect
// per frame -- the aux callbacks it registers are the SDK's own, handed
// straight back -- so recording the setup and reporting success is the whole
// of what the units above can observe.  The consequence is exactly one thing:
// the dry path is complete and the wet path is silent.

int AXFXChorusInit(struct AXFX_CHORUS* c)
{
    record("AXFXChorusInit", static_cast<const void*>(c));
    return 0;
}
int AXFXChorusShutdown(struct AXFX_CHORUS* c)
{
    record("AXFXChorusShutdown", static_cast<const void*>(c));
    return 0;
}
void AXFXChorusCallback(struct AXFX_BUFFERUPDATE* update,
                        struct AXFX_CHORUS* chorus)
{
    record("AXFXChorusCallback", static_cast<const void*>(update),
           static_cast<const void*>(chorus));
}

int AXFXDelayInit(struct AXFX_DELAY* delay)
{
    record("AXFXDelayInit", static_cast<const void*>(delay));
    return 0;
}
int AXFXDelayShutdown(struct AXFX_DELAY* delay)
{
    record("AXFXDelayShutdown", static_cast<const void*>(delay));
    return 0;
}
void AXFXDelayCallback(struct AXFX_BUFFERUPDATE* update,
                       struct AXFX_DELAY* delay)
{
    record("AXFXDelayCallback", static_cast<const void*>(update),
           static_cast<const void*>(delay));
}

int AXFXReverbHiInit(struct AXFX_REVERBHI* rev)
{
    record("AXFXReverbHiInit", static_cast<const void*>(rev));
    return 0;
}
int AXFXReverbHiShutdown(struct AXFX_REVERBHI* rev)
{
    record("AXFXReverbHiShutdown", static_cast<const void*>(rev));
    return 0;
}
void AXFXReverbHiCallback(struct AXFX_BUFFERUPDATE* update,
                          struct AXFX_REVERBHI* reverb)
{
    record("AXFXReverbHiCallback", static_cast<const void*>(update),
           static_cast<const void*>(reverb));
}

int AXFXReverbStdInit(struct AXFX_REVERBSTD* rev)
{
    record("AXFXReverbStdInit", static_cast<const void*>(rev));
    return 0;
}
int AXFXReverbStdShutdown(struct AXFX_REVERBSTD* rev)
{
    record("AXFXReverbStdShutdown", static_cast<const void*>(rev));
    return 0;
}
void AXFXReverbStdCallback(struct AXFX_BUFFERUPDATE* update,
                           struct AXFX_REVERBSTD* reverb)
{
    record("AXFXReverbStdCallback", static_cast<const void*>(update),
           static_cast<const void*>(reverb));
}

void* (*__AXFXAlloc)(unsigned long) = nullptr;
void (*__AXFXFree)(void*) = nullptr;

void AXFXSetHooks(void* (*alloc_hook)(unsigned long),
                  void (*free_hook)(void*))
{
    record("AXFXSetHooks", reinterpret_cast<const void*>(alloc_hook),
           reinterpret_cast<const void*>(free_hook));
    __AXFXAlloc = alloc_hook;
    __AXFXFree = free_hook;
}

} // extern "C"
