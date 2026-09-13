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
