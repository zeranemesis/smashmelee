#include "vi_bridge.hpp"

#include "os_scheduler.hpp"

namespace meleeboard::vi {
namespace {

VIRetraceCallback g_pre = nullptr;
VIRetraceCallback g_post = nullptr;

const void* g_pending = nullptr;
const void* g_current = nullptr;
const GXRenderModeObj* g_mode = nullptr;

bool g_black = false;
bool g_pending_black = false;

std::uint32_t g_retraces = 0;

// The console latches VI's registers at the start of a field.  Everything
// VISetNextFrameBuffer and VISetBlack changed since the last flush takes
// effect there, which is why the game can set them mid-frame without tearing.
bool g_dirty = false;

void latch()
{
    g_current = g_pending;
    g_black = g_pending_black;
    g_dirty = false;
}

void pre_retrace(std::uint32_t count)
{
    if (g_pre != nullptr) {
        g_pre(count);
    }
}

void post_retrace(std::uint32_t count)
{
    if (g_post != nullptr) {
        g_post(count);
    }
}

} // namespace

void reset()
{
    g_pre = nullptr;
    g_post = nullptr;
    g_pending = nullptr;
    g_current = nullptr;
    g_mode = nullptr;
    g_black = false;
    g_pending_black = false;
    g_retraces = 0;
    g_dirty = false;
}

std::uint32_t retrace_count() { return g_retraces; }
const void* pending_frame_buffer() { return g_pending; }
const void* current_frame_buffer() { return g_current; }
const GXRenderModeObj* configured_mode() { return g_mode; }
bool is_black() { return g_black; }

} // namespace meleeboard::vi

// ---------------------------------------------------------------------------
// The SDK surface, as upstream calls it.

extern "C" void VIInit(void) { meleeboard::vi::reset(); }

extern "C" void VIConfigure(const GXRenderModeObj* mode)
{
    meleeboard::vi::g_mode = mode;
    meleeboard::vi::g_dirty = true;
}

extern "C" void VISetNextFrameBuffer(void* frame_buffer)
{
    meleeboard::vi::g_pending = frame_buffer;
    meleeboard::vi::g_dirty = true;
}

extern "C" void VISetBlack(BOOL black)
{
    meleeboard::vi::g_pending_black = black != FALSE;
    meleeboard::vi::g_dirty = true;
}

extern "C" void VIFlush(void) { meleeboard::vi::latch(); }

extern "C" u32 VIGetRetraceCount(void) { return meleeboard::vi::g_retraces; }

extern "C" u32 VIGetTvFormat(void)
{
    // The format is a property of the configured mode, not of VI: the mode
    // object carries the TV mode the game asked for.  Before the game has
    // configured one, answer NTSC, which is what GALE01 v1.02 is.
    if (meleeboard::vi::g_mode == nullptr) {
        return VI_NTSC;
    }
    return static_cast<u32>(meleeboard::vi::g_mode->viTVmode) >> 2;
}

extern "C" VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback)
{
    VIRetraceCallback previous = meleeboard::vi::g_pre;
    meleeboard::vi::g_pre = callback;
    return previous;
}

extern "C" VIRetraceCallback
VISetPostRetraceCallback(VIRetraceCallback callback)
{
    VIRetraceCallback previous = meleeboard::vi::g_post;
    meleeboard::vi::g_post = callback;
    return previous;
}

extern "C" void VIWaitForRetrace(void)
{
    // One field of the timebase, with every alarm that comes due inside it
    // firing at its own instant -- then the retrace itself.
    meleeboard::os::advance_frame();

    ++meleeboard::vi::g_retraces;

    // Pre-retrace runs before the hardware latches, post-retrace after.  Both
    // go through the scheduler, so a critical section holds them off exactly
    // as it holds off an alarm.
    meleeboard::os::deliver_as_interrupt(&meleeboard::vi::pre_retrace,
                                         meleeboard::vi::g_retraces);
    if (meleeboard::vi::g_dirty) {
        meleeboard::vi::latch();
    }
    meleeboard::os::deliver_as_interrupt(&meleeboard::vi::post_retrace,
                                         meleeboard::vi::g_retraces);
}
