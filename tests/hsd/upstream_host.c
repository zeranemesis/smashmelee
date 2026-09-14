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
#include <dolphin/dvd.h>
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



// The symbol initialize.c reaches for that lives in a unit this target does
// not compile.
//
// HSD_LogInit is debug.c's, and debug.c is written against the Metrowerks
// libc's FILE internals -- a host port replaces it rather than compiles it.
// Initializing no log is the correct behavior for a test binary anyway.
void HSD_LogInit(void) {}

// HSD_Synth_804D6018, the audio heap handle, used to be defined here while
// synth.c stayed out of the target.  It arrives with synth.c, and announced
// itself as a duplicate definition the moment it did -- which is what these
// shims are written to do.

// ---------------------------------------------------------------------------
// The rest of the audio boundary.
//
// src/melee_port/upstream/ax_record.cpp answers AX and AXFX, and aram.cpp
// answers ARAM.  What is left is small and belongs here: the three AI calls
// the game makes, the two cache operations, the stereo/mono setting, and the
// four DVD calls the sound-bank loader uses.

// AI carries the analog output stage, and the game touches three things on it:
// the DSP's sample rate (0 is 32 kHz, which is the rate AX runs at) and the
// two stream volumes.  Nothing here mixes, so they are recorded and read back
// by a test rather than acted on.
static unsigned long g_ai_dsp_sample_rate = 0;
static unsigned char g_ai_stream_vol_left = 0;
static unsigned char g_ai_stream_vol_right = 0;

void AIInit(u8* stack) { (void) stack; }
void AISetDSPSampleRate(u32 rate) { g_ai_dsp_sample_rate = rate; }
void AISetStreamVolLeft(u8 volume) { g_ai_stream_vol_left = volume; }
void AISetStreamVolRight(u8 volume) { g_ai_stream_vol_right = volume; }

unsigned long melee_ai_dsp_sample_rate(void) { return g_ai_dsp_sample_rate; }
unsigned char melee_ai_stream_vol_left(void) { return g_ai_stream_vol_left; }
unsigned char melee_ai_stream_vol_right(void) { return g_ai_stream_vol_right; }

// The data cache operations, which docs/PLAN.md's phase 1 lists as no-ops on a
// coherent host.  They are not no-ops on the console: the game flushes a
// display list or a sample buffer out of the cache before handing it to a
// device that reads main memory directly.  x86-64 and ARM64 keep the cache
// coherent with DMA, so there is nothing to do -- and the reason is worth
// having written down, because a port to a platform that does not would have
// to fill these in.
void DCStoreRange(void* address, u32 bytes)
{
    (void) address;
    (void) bytes;
}

void DCInvalidateRange(void* address, u32 bytes)
{
    (void) address;
    (void) bytes;
}

// The stereo/mono/surround setting the console keeps in its real-time clock.
// Melee reads it to decide how to pan, so it has to answer something, and
// stereo is what a host with two speakers is.
#define MELEE_HOST_SOUND_MODE 1
static unsigned long g_sound_mode = MELEE_HOST_SOUND_MODE;
u32 OSGetSoundMode(void) { return g_sound_mode; }
void OSSetSoundMode(u32 mode) { g_sound_mode = mode; }

// The disc.
//
// This target has no disc image by design -- it never touches one, which is
// what lets it run anywhere -- so these report the file as absent and record
// what was asked for.  Absent is a path the game already handles: a sound bank
// that does not open leaves the bank unloaded rather than crashing.  A test
// reads melee_dvd_last_request() to check that the loader asked for the file
// it should have.
static char g_dvd_last_path[256];
static int g_dvd_requests = 0;

s32 DVDConvertPathToEntrynum(const char* path)
{
    if (path != NULL) {
        size_t index = 0;
        while (index + 1 < sizeof g_dvd_last_path && path[index] != '\0') {
            g_dvd_last_path[index] = path[index];
            ++index;
        }
        g_dvd_last_path[index] = '\0';
        ++g_dvd_requests;
    }
    return -1;
}

const char* melee_dvd_last_request(void) { return g_dvd_last_path; }
int melee_dvd_request_count(void) { return g_dvd_requests; }

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* info)
{
    (void) entrynum;
    (void) info;
    return FALSE;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* info, void* address, s32 length, s32 offset,
                      DVDCallback callback, s32 priority)
{
    (void) address;
    (void) offset;
    (void) priority;
    // A read on a file that never opened completes with a failure, which the
    // callback has to be told about -- dropping it would hang a loader that
    // waits for completion.
    if (callback != NULL) {
        callback(-1, info);
    }
    (void) length;
    return FALSE;
}

BOOL DVDClose(DVDFileInfo* info)
{
    (void) info;
    return FALSE;
}
