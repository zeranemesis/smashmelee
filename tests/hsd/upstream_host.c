// The host surface upstream's HSD object allocator needs in order to run.
//
// These are the first pieces of the Dolphin SDK boundary described in phase 1
// of docs/PLAN.md.  In the shipping build Aurora provides OSAllocFromHeap and
// friends; this file exists so the conformance target can link and run without
// the graphics stack behind it.

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _MSC_VER
#include <malloc.h>
#endif

#include <melee/port/dolphin_compat.h>
#include <sysdolphin/baselib/video.h>

// The OS arena, the heap allocator and __OSCurrHeap used to be hand-written
// here: a posix_memalign per allocation, a free per release, and an
// OSCheckHeap that always answered zero.  They now come from
// src/melee_port/upstream/os_arena.cpp, which is a real first-fit heap over a
// real arena -- so initialize.c can carve the audio and main heaps out of it
// the way HSD_OSInit means to, and objalloc.c's trimming path sees an honest
// answer when it asks how much room is left.

void OSReport(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

// A failed HSD_ASSERT is a test failure, and aborting reports it as one.
void __assert(char* file, unsigned int line, char* message)
{
    fprintf(stderr, "upstream assertion failed: %s:%u: %s\n", file, line,
            message);
    abort();
}

void HSD_Panic(char* file, unsigned int line, char* message)
{
    fprintf(stderr, "upstream panic: %s:%u: %s\n", file, line, message);
    abort();
}

void OSPanic(const char* file, int line, const char* message, ...)
{
    va_list arguments;
    fprintf(stderr, "OSPanic: %s:%d: ", file, line);
    va_start(arguments, message);
    vfprintf(stderr, message, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    abort();
}

// OSGetTime used to be a bare counter here.  It now comes from
// src/melee_port/upstream/os_scheduler.cpp, with a timebase that advances one
// exact NTSC frame at a time -- so perf.c's statistics are measured against
// the same clock the alarms fire on.

// cobj.c asks which field is next when it jitters the viewport for an
// interlaced mode.  Answering zero every time keeps the jitter deterministic;
// a test that wants the other field sets the render mode's field rendering
// off instead.
unsigned int VIGetNextField(void) { return 0; }

// lb_0195.c sets the controller polling period with this, then arms a periodic
// alarm at the same rate.  The alarm is what actually drives sampling in this
// port, so the rate is recorded rather than acted on.
static unsigned long g_pad_sampling_rate_msec = 0;
void PADSetSamplingRate(unsigned long msec) { g_pad_sampling_rate_msec = msec; }
unsigned long melee_pad_sampling_rate(void) { return g_pad_sampling_rate_msec; }



// The two symbols initialize.c reaches for that live in units this target
// does not compile.
//
// HSD_LogInit is debug.c's, and debug.c is written against the Metrowerks
// libc's FILE internals -- a host port replaces it rather than compiles it.
// Initializing no log is the correct behavior for a test binary anyway.
void HSD_LogInit(void) {}

// The audio heap handle is synth.c's, and synth.c is phase 6.  HSD_OSInit
// assigns it the heap it carves for audio, so the storage has to exist even
// though nothing reads it yet; -1 is what synth.static.h initializes it to.
OSHeapHandle HSD_Synth_804D6018 = -1;
