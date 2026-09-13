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
volatile int __OSCurrHeap = 0;

// HSD pools hand their objects to GX, which wants 32-byte alignment.
#define MELEE_HOST_ALIGNMENT 32

void* OSAllocFromHeap(int heap, unsigned long size)
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

void OSFreeToHeap(int heap, void* pointer)
{
    (void) heap;
#ifdef _MSC_VER
    _aligned_free(pointer);
#else
    free(pointer);
#endif
}

long OSCheckHeap(int heap)
{
    (void) heap;
    return 0;
}

// HSD_GetHeap lives in initialize.c, which is not part of this target: it
// would pull in VI, GX and the framebuffers.
int HSD_GetHeap(void) { return 0; }

void OSReport(char* format, ...)
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

// perf.c times its statistics with this.  A counter rather than a clock: the
// point of the offline suite is that the same input produces the same output,
// and a real clock would put the wall time into a recorded frame.
long long OSGetTime(void)
{
    static long long ticks = 0;
    return ++ticks;
}

// cobj.c asks which field is next when it jitters the viewport for an
// interlaced mode.  Answering zero every time keeps the jitter deterministic;
// a test that wants the other field sets the render mode's field rendering
// off instead.
unsigned int VIGetNextField(void) { return 0; }



// The two pieces of HSD that live in units still at the boot boundary.
//
// HSD_GetCurrentRenderPass is defined in initialize.c, which cannot come over
// until the OS arena and heap do -- it is phase 1 of docs/PLAN.md, not phase
// 2.  cobj.c reads it to choose which camera setup to run, so answering
// HSD_RP_SCREEN here is what puts the camera on the ordinary path.
HSD_RenderPass HSD_GetCurrentRenderPass(void) { return HSD_RP_SCREEN; }

// HSD_VIData is defined in video.c, which needs three SDK symbols Aurora does
// not have (GXInitFogAdjTable is fog.c's; video.c's own are
// VIPadFrameBufferWidth, GXWaitDrawDone and the GXNtsc480IntDf render mode).
// Defining it here is deliberate and self-announcing: the day video.c joins
// the target, the linker reports a duplicate and this goes away.
//
// It is zero, which means a render mode of all zeroes.  A test that cares
// what cobj computes from the viewport fills the fields it needs first --
// which is the useful arrangement anyway, because it makes the video state an
// input to the test rather than a global the test has to work around.
HSD_VIInfo HSD_VIData;
