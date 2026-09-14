#include "os_scheduler.hpp"

#include <cstdio>
#include <vector>

namespace meleeboard::os {
namespace {

// The console's timebase, in ticks since reset().  Advanced only by
// advance_frame(), and only to values a real decrementer could have shown.
OSTime g_now = 0;

// The interrupt mask is a flag, not a counter, because that is the SDK's
// contract: OSDisableInterrupts returns the previous state and the caller
// hands that exact value back to OSRestoreInterrupts.  Nesting works because
// an inner disable returns FALSE, so its restore leaves them disabled.
bool g_interrupts_enabled = true;

// Alarms are owned by the caller -- they are embedded in game structures --
// so the queue holds pointers, never copies.
std::vector<OSAlarm*> g_armed;

// Handlers whose alarm came due while interrupts were masked.
struct Deferred {
    OSAlarm* alarm;
    OSAlarmHandler handler;
};
std::vector<Deferred> g_deferred;

// Guards against a handler that masks and restores interrupts from draining
// the queue it is itself being drained from.
bool g_draining = false;

bool g_runaway = false;

void disarm(OSAlarm* alarm)
{
    for (std::size_t index = 0; index < g_armed.size(); ++index) {
        if (g_armed[index] == alarm) {
            g_armed.erase(g_armed.begin() + static_cast<long>(index));
            return;
        }
    }
}

void arm(OSAlarm* alarm)
{
    disarm(alarm);
    g_armed.push_back(alarm);
}

// The next alarm due at or before `deadline`.  Earliest fire time wins; ties
// go to whichever was armed first, which is what keeps the order of two
// alarms set for the same tick from depending on the allocator.
OSAlarm* earliest_due(OSTime deadline)
{
    OSAlarm* best = nullptr;
    for (OSAlarm* alarm : g_armed) {
        if (alarm->fire > deadline) {
            continue;
        }
        if (best == nullptr || alarm->fire < best->fire) {
            best = alarm;
        }
    }
    return best;
}

void drain_deferred()
{
    if (g_draining) {
        return;
    }
    g_draining = true;
    // Indexed rather than ranged: a handler may defer more work, and those
    // run in this same drain, in order.
    for (std::size_t index = 0; index < g_deferred.size(); ++index) {
        const Deferred entry = g_deferred[index];
        if (entry.handler != nullptr) {
            entry.handler(entry.alarm, nullptr);
        }
    }
    g_deferred.clear();
    g_draining = false;
}

// Runs a handler now, or queues it if interrupts are masked.
void deliver(OSAlarm* alarm, OSAlarmHandler handler)
{
    if (handler == nullptr) {
        return;
    }
    if (!g_interrupts_enabled) {
        g_deferred.push_back(Deferred{ alarm, handler });
        return;
    }
    handler(alarm, nullptr);
}

} // namespace

void reset()
{
    g_now = 0;
    g_interrupts_enabled = true;
    g_armed.clear();
    g_deferred.clear();
    g_draining = false;
    g_runaway = false;
}

std::size_t armed_alarms() { return g_armed.size(); }
std::size_t deferred_callbacks() { return g_deferred.size(); }
bool interrupts_enabled() { return g_interrupts_enabled; }
bool runaway_alarm_detected() { return g_runaway; }

void advance_frame()
{
    const OSTime deadline = g_now + kTicksPerFrame;

    // A periodic alarm whose period is zero, or a handler that re-arms in the
    // past, would keep coming due at the same instant forever.  The console
    // would livelock; this stops and says so.  The bound is generous: a 3 ms
    // alarm -- the shortest the game sets -- comes due five times a frame.
    constexpr int kMaxFiringsPerFrame = 4096;
    int firings = 0;

    while (OSAlarm* alarm = earliest_due(deadline)) {
        if (++firings > kMaxFiringsPerFrame) {
            g_runaway = true;
            break;
        }

        // Time moves to the alarm's own fire instant, so a handler reading
        // OSGetTime() sees the moment it was scheduled for.  Never backwards:
        // a deferred alarm may come due before the clock reached it.
        if (alarm->fire > g_now) {
            g_now = alarm->fire;
        }

        OSAlarmHandler handler = alarm->handler;
        if (alarm->period > 0) {
            alarm->fire += alarm->period;
        } else {
            disarm(alarm);
            alarm->handler = nullptr;
        }
        deliver(alarm, handler);
    }

    g_now = deadline;
}

} // namespace meleeboard::os

// ---------------------------------------------------------------------------
// The SDK surface, as upstream calls it.

extern "C" BOOL OSDisableInterrupts(void)
{
    const BOOL previous = meleeboard::os::g_interrupts_enabled ? TRUE : FALSE;
    meleeboard::os::g_interrupts_enabled = false;
    return previous;
}

extern "C" BOOL OSEnableInterrupts(void)
{
    const BOOL previous = meleeboard::os::g_interrupts_enabled ? TRUE : FALSE;
    meleeboard::os::g_interrupts_enabled = true;
    meleeboard::os::drain_deferred();
    return previous;
}

extern "C" BOOL OSRestoreInterrupts(BOOL level)
{
    const BOOL previous = meleeboard::os::g_interrupts_enabled ? TRUE : FALSE;
    meleeboard::os::g_interrupts_enabled = level != FALSE;
    if (meleeboard::os::g_interrupts_enabled) {
        // Whatever came due inside the critical section runs here, on this
        // stack, which is where the console would have taken the interrupt.
        meleeboard::os::drain_deferred();
    }
    return previous;
}

extern "C" OSTime OSGetTime(void) { return meleeboard::os::g_now; }

extern "C" OSTick OSGetTick(void)
{
    return static_cast<OSTick>(meleeboard::os::g_now);
}

extern "C" void OSInitAlarm(void) {}

extern "C" void OSCreateAlarm(OSAlarm* alarm)
{
    if (alarm == nullptr) {
        return;
    }
    meleeboard::os::disarm(alarm);
    *alarm = OSAlarm{};
}

extern "C" void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
    if (alarm == nullptr) {
        return;
    }
    alarm->handler = handler;
    alarm->period = 0;
    alarm->start = 0;
    alarm->fire = meleeboard::os::g_now + tick;
    meleeboard::os::arm(alarm);
}

extern "C" void OSSetAbsAlarm(OSAlarm* alarm, OSTime time,
                              OSAlarmHandler handler)
{
    if (alarm == nullptr) {
        return;
    }
    alarm->handler = handler;
    alarm->period = 0;
    alarm->start = 0;
    alarm->fire = time;
    meleeboard::os::arm(alarm);
}

extern "C" void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period,
                                   OSAlarmHandler handler)
{
    if (alarm == nullptr) {
        return;
    }
    // `start` is taken as a delay from now, matching OSSetAlarm -- the SDK
    // keeps OSSetAbsAlarm for the absolute form.  Both shipping callers pass
    // start == period (lbmthp.c a 60 Hz movie tick, lb_0195.c the controller
    // sampling period), so the two readings differ only in the first firing
    // and agree on every one after it.  The SDK's own source would settle it;
    // it is not in the decompilation.
    alarm->handler = handler;
    alarm->start = start;
    alarm->period = period;
    alarm->fire = meleeboard::os::g_now + start;
    meleeboard::os::arm(alarm);
}

extern "C" void OSCancelAlarm(OSAlarm* alarm)
{
    if (alarm == nullptr) {
        return;
    }
    meleeboard::os::disarm(alarm);
    alarm->handler = nullptr;
    alarm->period = 0;
}

extern "C" void OSSetAlarmTag(OSAlarm* alarm, u32 tag)
{
    if (alarm != nullptr) {
        alarm->tag = tag;
    }
}

extern "C" void OSCancelAlarms(u32 tag)
{
    std::vector<OSAlarm*> keep;
    for (OSAlarm* alarm : meleeboard::os::g_armed) {
        if (alarm->tag == tag) {
            alarm->handler = nullptr;
            alarm->period = 0;
        } else {
            keep.push_back(alarm);
        }
    }
    meleeboard::os::g_armed.swap(keep);
}

extern "C" BOOL OSCheckAlarmQueue(void)
{
    return meleeboard::os::g_armed.empty() ? FALSE : TRUE;
}
