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

// OSAlloc.h declares this as the heap OSAlloc()/OSFree() use.
//
// These four used to be declared here with hand-written signatures, which
// nothing checked until the prelude started pulling <dolphin/os.h> in.  They
// now match Aurora's declarations exactly -- OSHeapHandle rather than int, u32
// rather than unsigned long, const char* rather than char* -- which is what a
// build against Aurora's real OS would have required anyway.
volatile int __OSCurrHeap = 0;

// HSD pools hand their objects to GX, which wants 32-byte alignment.
#define MELEE_HOST_ALIGNMENT 32

void* OSAllocFromHeap(OSHeapHandle heap, u32 size)
{
    const size_t bytes = size != 0 ? (size_t) size : MELEE_HOST_ALIGNMENT;
    (void) heap;
#ifdef _MSC_VER
    return _aligned_malloc(bytes, MELEE_HOST_ALIGNMENT);
#else
    {
        void* memory = NULL;
        if (posix_memalign(&memory, MELEE_HOST_ALIGNMENT, bytes) != 0) {
            return NULL;
        }
        return memory;
    }
#endif
}

void OSFreeToHeap(OSHeapHandle heap, void* pointer)
{
    (void) heap;
#ifdef _MSC_VER
    _aligned_free(pointer);
#else
    free(pointer);
#endif
}

s32 OSCheckHeap(OSHeapHandle heap)
{
    (void) heap;
    return 0;
}

// HSD_GetHeap lives in initialize.c, which is not part of this target: it
// would pull in VI, GX and the framebuffers.
int HSD_GetHeap(void) { return 0; }

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



// The two pieces of HSD that live in units still at the boot boundary.
//
// HSD_GetCurrentRenderPass is defined in initialize.c, which cannot come over
// until the OS arena and heap do -- it is phase 1 of docs/PLAN.md, not phase
// 2.  cobj.c reads it to choose which camera setup to run, so answering
// HSD_RP_SCREEN here is what puts the camera on the ordinary path.
HSD_RenderPass HSD_GetCurrentRenderPass(void) { return HSD_RP_SCREEN; }

