#include "harness.hpp"

// The threading model, tested rather than described.
//
// src/melee_port/upstream/os_scheduler.hpp says why there is one thread: the
// measurement that found no OSCreateThread in Melee's shipping code.  These
// cases pin the three behaviours the game actually depends on -- a timebase
// that advances exactly, alarms that fire at their own instant, and critical
// sections that really do hold a handler off.

#include <string>
#include <vector>

#include <melee/port/dolphin_compat.h>

#include "os_scheduler.hpp"

namespace os = meleeboard::os;

namespace {

std::vector<std::string>* g_log = nullptr;

void note(const char* name, OSTime at)
{
    if (g_log != nullptr) {
        g_log->push_back(std::string(name) + "@" + std::to_string(at));
    }
}

void record_a(OSAlarm*, OSContext*) { note("a", OSGetTime()); }
void record_b(OSAlarm*, OSContext*) { note("b", OSGetTime()); }

// Reports the instant it was *scheduled* for rather than the instant it ran.
void record_scheduled(OSAlarm* alarm, OSContext*)
{
    note("s", alarm->fire);
}

// Re-arms itself the way lbmemory.c chunks a copy: three milliseconds, again.
OSAlarm g_chunked{};
int g_chunks_left = 0;
void copy_one_chunk(OSAlarm*, OSContext*)
{
    note("chunk", OSGetTime());
    if (--g_chunks_left > 0) {
        OSCreateAlarm(&g_chunked);
        OSSetAlarm(&g_chunked, OSMillisecondsToTicks(3), copy_one_chunk);
    }
}

} // namespace

MELEE_TEST(UpstreamScheduler, AdvancesTheTimebaseByAnExactFrame)
{
    // 40.5 MHz over 60000/1001 Hz is 675675 ticks with no remainder, so a
    // fixed step accumulates no error -- ten frames land exactly on ten
    // frames, not on a number that has drifted.
    os::reset();
    CHECK_EQ(OSGetTime(), OSTime(0));

    for (int frame = 0; frame < 10; ++frame) {
        os::advance_frame();
    }
    CHECK_EQ(OSGetTime(), OSTime(10) * os::kTicksPerFrame);
    CHECK_EQ(OSGetTime(), OSTime(6756750));
}

MELEE_TEST(UpstreamScheduler, FiresAnAlarmAtItsOwnInstant)
{
    // A handler that reads the clock has to see the moment it was scheduled
    // for, not the end of the frame it happened to fall in.  Anything else
    // would make a timestamp depend on the step size.
    std::vector<std::string> log;
    g_log = &log;
    os::reset();

    OSAlarm alarm{};
    OSCreateAlarm(&alarm);
    OSSetAlarm(&alarm, OSMillisecondsToTicks(3), record_a);
    CHECK_EQ(os::armed_alarms(), std::size_t(1));

    os::advance_frame();
    REQUIRE_EQ(log.size(), std::size_t(1));
    CHECK_EQ(log[0], std::string("a@121500"));

    // A one-shot disarms itself, so the next frame is quiet.
    CHECK_EQ(os::armed_alarms(), std::size_t(0));
    os::advance_frame();
    CHECK_EQ(log.size(), std::size_t(1));
    g_log = nullptr;
}

MELEE_TEST(UpstreamScheduler, OrdersTwoAlarmsByTimeThenByArming)
{
    // Two alarms in one frame fire in time order; two set for the same tick
    // fire in the order they were armed, so the sequence cannot depend on
    // where the caller happened to put the structures.
    std::vector<std::string> log;
    g_log = &log;
    os::reset();

    OSAlarm later{};
    OSAlarm sooner{};
    OSCreateAlarm(&later);
    OSCreateAlarm(&sooner);
    OSSetAlarm(&later, OSMillisecondsToTicks(9), record_b);
    OSSetAlarm(&sooner, OSMillisecondsToTicks(3), record_a);

    os::advance_frame();
    REQUIRE_EQ(log.size(), std::size_t(2));
    CHECK_EQ(log[0], std::string("a@121500"));
    CHECK_EQ(log[1], std::string("b@364500"));

    log.clear();
    OSAlarm first{};
    OSAlarm second{};
    OSCreateAlarm(&first);
    OSCreateAlarm(&second);
    OSSetAlarm(&first, OSMillisecondsToTicks(1), record_a);
    OSSetAlarm(&second, OSMillisecondsToTicks(1), record_b);
    os::advance_frame();
    REQUIRE_EQ(log.size(), std::size_t(2));
    CHECK_EQ(log[0].substr(0, 1), std::string("a"));
    CHECK_EQ(log[1].substr(0, 1), std::string("b"));
    g_log = nullptr;
}

MELEE_TEST(UpstreamScheduler, HoldsAHandlerOffInsideACriticalSection)
{
    // This is what the game's 225 interrupt-masking calls are for.  video.c
    // masks interrupts to swap a retrace callback pointer; the handler must
    // not run while the pointer is half written.
    std::vector<std::string> log;
    g_log = &log;
    os::reset();

    OSAlarm alarm{};
    OSCreateAlarm(&alarm);
    OSSetAlarm(&alarm, OSMillisecondsToTicks(3), record_a);

    const BOOL level = OSDisableInterrupts();
    CHECK(!os::interrupts_enabled());

    os::advance_frame();
    // The alarm came due, and did not run.
    CHECK_EQ(log.size(), std::size_t(0));
    CHECK_EQ(os::deferred_callbacks(), std::size_t(1));

    OSRestoreInterrupts(level);
    CHECK(os::interrupts_enabled());
    REQUIRE_EQ(log.size(), std::size_t(1));
    CHECK_EQ(os::deferred_callbacks(), std::size_t(0));

    // It ran *late*, and the clock says so.  That is the console's behaviour,
    // not an approximation of it: the decrementer exception was pending while
    // the mask was up, and it is taken when the mask comes down -- at which
    // point OSGetTime() reports the moment of the restore, not the moment the
    // alarm was set for.  A port that rewound the clock to 121500 here would
    // be inventing an accuracy the hardware never had.
    CHECK_EQ(log[0], std::string("a@675675"));
    g_log = nullptr;
}

MELEE_TEST(UpstreamScheduler, KeepsTheScheduledInstantOnTheAlarmItself)
{
    // Because a deferred handler sees a late clock, a handler that needs the
    // instant it was scheduled for has to read it off the alarm.  A one-shot
    // keeps its fire time there after it is disarmed, so that stays available
    // however late the handler runs.
    std::vector<std::string> log;
    g_log = &log;
    os::reset();

    OSAlarm alarm{};
    OSCreateAlarm(&alarm);
    OSSetAlarm(&alarm, OSMillisecondsToTicks(3), record_scheduled);

    const BOOL level = OSDisableInterrupts();
    os::advance_frame();
    OSRestoreInterrupts(level);

    REQUIRE_EQ(log.size(), std::size_t(1));
    CHECK_EQ(log[0], std::string("s@121500"));
    CHECK_EQ(alarm.fire, OSTime(121500));
    g_log = nullptr;
}

MELEE_TEST(UpstreamScheduler, NestsCriticalSectionsTheWayTheSDKDoes)
{
    // The mask is a flag, not a counter, because that is the SDK's contract:
    // OSDisableInterrupts returns the previous state and the caller hands
    // that exact value back.  An inner disable therefore returns FALSE, and
    // its restore correctly leaves interrupts off.
    os::reset();
    CHECK(os::interrupts_enabled());

    const BOOL outer = OSDisableInterrupts();
    CHECK_EQ(outer, BOOL(TRUE));

    const BOOL inner = OSDisableInterrupts();
    CHECK_EQ(inner, BOOL(FALSE));

    OSRestoreInterrupts(inner);
    CHECK(!os::interrupts_enabled());

    OSRestoreInterrupts(outer);
    CHECK(os::interrupts_enabled());
}

MELEE_TEST(UpstreamScheduler, RunsAPeriodicAlarmOncePerPeriod)
{
    // lb_0195.c samples the controller on a periodic alarm and lbmthp.c ticks
    // a movie at 60 Hz on one.  Both pass start == period.
    std::vector<std::string> log;
    g_log = &log;
    os::reset();

    OSAlarm alarm{};
    OSCreateAlarm(&alarm);
    const OSTime period = os::kTicksPerFrame / 4;
    OSSetPeriodicAlarm(&alarm, period, period, record_a);

    os::advance_frame();
    // Starting one period in, four quarters of a frame gives four firings.
    REQUIRE_EQ(log.size(), std::size_t(4));
    CHECK_EQ(log[0], std::string("a@168918"));
    CHECK_EQ(log[3], std::string("a@675672"));

    // And it stays armed, unlike a one-shot.
    CHECK_EQ(os::armed_alarms(), std::size_t(1));
    os::advance_frame();
    CHECK_EQ(log.size(), std::size_t(8));

    OSCancelAlarm(&alarm);
    CHECK_EQ(os::armed_alarms(), std::size_t(0));
    os::advance_frame();
    CHECK_EQ(log.size(), std::size_t(8));
    g_log = nullptr;
}

MELEE_TEST(UpstreamScheduler, LetsAHandlerReArmItselfTheWayTheCopierDoes)
{
    // lbmemory.c copies 0x19000 bytes, re-arms a 3 ms alarm, and copies the
    // next chunk.  That is cooperative time-slicing written by hand, and it
    // is the closest thing in the game to a background thread -- which is the
    // whole argument for not building one.
    std::vector<std::string> log;
    g_log = &log;
    os::reset();

    g_chunks_left = 3;
    OSCreateAlarm(&g_chunked);
    OSSetAlarm(&g_chunked, OSMillisecondsToTicks(3), copy_one_chunk);

    os::advance_frame();
    REQUIRE_EQ(log.size(), std::size_t(3));
    CHECK_EQ(log[0], std::string("chunk@121500"));
    CHECK_EQ(log[1], std::string("chunk@243000"));
    CHECK_EQ(log[2], std::string("chunk@364500"));
    CHECK_EQ(os::armed_alarms(), std::size_t(0));
    CHECK(!os::runaway_alarm_detected());
    g_log = nullptr;
}

MELEE_TEST(UpstreamScheduler, ProducesTheSameSequenceEveryRun)
{
    // The property the whole model exists for.  Determinism is the plan's
    // acceptance criterion, so it is checked directly: two runs of the same
    // schedule produce the same handler calls at the same timebase values.
    auto run = [] {
        std::vector<std::string> log;
        g_log = &log;
        os::reset();

        OSAlarm periodic{};
        OSAlarm once{};
        OSCreateAlarm(&periodic);
        OSCreateAlarm(&once);
        OSSetPeriodicAlarm(&periodic, OSMillisecondsToTicks(5),
                           OSMillisecondsToTicks(5), record_a);
        OSSetAlarm(&once, OSMillisecondsToTicks(12), record_b);

        for (int frame = 0; frame < 3; ++frame) {
            const BOOL level = OSDisableInterrupts();
            os::advance_frame();
            OSRestoreInterrupts(level);
        }
        OSCancelAlarm(&periodic);
        g_log = nullptr;
        return log;
    };

    const std::vector<std::string> first = run();
    const std::vector<std::string> second = run();
    REQUIRE(!first.empty());
    CHECK(first == second);
}
