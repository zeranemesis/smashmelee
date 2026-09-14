#include "os_arena.hpp"

#include <cstdlib>
#include <cstring>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace {

// The SDK's allocator granularity.  Every cell starts on a 32-byte boundary
// and every cell is a whole number of them, which is also what GX wants of
// the vertex and display-list buffers HSD carves out of this heap.
constexpr std::uint32_t kAlignment = 32;
constexpr std::uint32_t kHeaderSize = 32;

// The smallest cell worth leaving behind when a request splits one: header
// plus one alignment unit of payload.  A remainder under this is handed to
// the caller instead, which is what stops the free list filling with cells
// too small to ever satisfy a request.
constexpr std::uint32_t kMinObjectSize = 64;

struct HeapDesc;

struct alignas(kAlignment) Cell {
    Cell* prev;
    Cell* next;
    s32 size;
    HeapDesc* owner;
};
static_assert(sizeof(Cell) == kHeaderSize,
              "the cell header has to stay exactly one alignment unit: the "
              "payload address is the cell address plus kHeaderSize, and both "
              "have to be 32-byte aligned");

struct HeapDesc {
    s32 size;
    Cell* freeList;
    Cell* allocated;
};

// The host block, and the arena inside it.
unsigned char* g_block = nullptr;
std::size_t g_block_bytes = 0;
std::uint32_t g_physical_bytes = 0;

HeapDesc* g_heaps = nullptr;
int g_heap_count = 0;
unsigned char* g_arena_start = nullptr; // allocatable range, after OSInitAlloc
unsigned char* g_arena_end = nullptr;

// What OSGetArenaLo/Hi answer.  OSInitAlloc does not move these; upstream
// moves them itself with OSSetArenaLo once it knows where the descriptor
// array ended.
unsigned char* g_arena_lo = nullptr;
unsigned char* g_arena_hi = nullptr;

std::size_t round_up_to_power_of_two(std::size_t value)
{
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

void* allocate_aligned_block(std::size_t bytes)
{
#ifdef _MSC_VER
    return _aligned_malloc(bytes, bytes);
#else
    // aligned_alloc wants a size that is a multiple of the alignment, which a
    // block aligned to its own size satisfies by construction.
    return std::aligned_alloc(bytes, bytes);
#endif
}

void free_aligned_block(void* pointer)
{
#ifdef _MSC_VER
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

std::uintptr_t round_up_32(std::uintptr_t value)
{
    return (value + (kAlignment - 1)) & ~static_cast<std::uintptr_t>(kAlignment - 1);
}

std::uintptr_t round_down_32(std::uintptr_t value)
{
    return value & ~static_cast<std::uintptr_t>(kAlignment - 1);
}

bool in_arena(const void* pointer)
{
    if (g_arena_start == nullptr || g_arena_end == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return address >= reinterpret_cast<std::uintptr_t>(g_arena_start) &&
           address < reinterpret_cast<std::uintptr_t>(g_arena_end);
}

bool valid_heap(OSHeapHandle heap)
{
    return g_heaps != nullptr && heap >= 0 && heap < g_heap_count &&
           g_heaps[heap].size >= 0;
}

Cell* add_front(Cell* list, Cell* cell)
{
    cell->prev = nullptr;
    cell->next = list;
    if (list != nullptr) {
        list->prev = cell;
    }
    return cell;
}

Cell* extract(Cell* list, Cell* cell)
{
    if (cell->next != nullptr) {
        cell->next->prev = cell->prev;
    }
    if (cell->prev != nullptr) {
        cell->prev->next = cell->next;
        return list;
    }
    return cell->next;
}

bool contains_cell(Cell* list, const Cell* cell)
{
    for (Cell* it = list; it != nullptr; it = it->next) {
        if (it == cell) {
            return true;
        }
    }
    return false;
}

// The free list is kept in address order, which is what makes coalescing a
// local check: a freed cell can only ever merge with the neighbours it lands
// between.
Cell* insert_and_coalesce(Cell* list, Cell* cell)
{
    Cell* prev = nullptr;
    Cell* next = list;
    while (next != nullptr && next < cell) {
        prev = next;
        next = next->next;
    }

    cell->prev = prev;
    cell->next = next;
    if (prev != nullptr) {
        prev->next = cell;
    } else {
        list = cell;
    }
    if (next != nullptr) {
        next->prev = cell;
    }

    if (cell->next != nullptr) {
        Cell* right = cell->next;
        if (reinterpret_cast<unsigned char*>(cell) + cell->size ==
            reinterpret_cast<unsigned char*>(right)) {
            cell->size += right->size;
            cell->next = right->next;
            if (right->next != nullptr) {
                right->next->prev = cell;
            }
        }
    }

    if (cell->prev != nullptr) {
        Cell* left = cell->prev;
        if (reinterpret_cast<unsigned char*>(left) + left->size ==
            reinterpret_cast<unsigned char*>(cell)) {
            left->size += cell->size;
            left->next = cell->next;
            if (cell->next != nullptr) {
                cell->next->prev = left;
            }
        }
    }

    return list;
}

bool valid_block_range(std::uintptr_t start, std::uintptr_t end)
{
    if (start >= end || g_arena_start == nullptr || g_arena_end == nullptr) {
        return false;
    }
    return start >= reinterpret_cast<std::uintptr_t>(g_arena_start) &&
           end <= reinterpret_cast<std::uintptr_t>(g_arena_end) &&
           (end - start) >= kMinObjectSize;
}

void forget_arena()
{
    g_heaps = nullptr;
    g_heap_count = 0;
    g_arena_start = nullptr;
    g_arena_end = nullptr;
    g_arena_lo = nullptr;
    g_arena_hi = nullptr;
    g_physical_bytes = 0;
}

} // namespace

// OSAlloc.h declares this as the heap OSAlloc() and OSFree() reach for.  It
// lives here rather than in the host shims because OSSetCurrentHeap owns it.
extern "C" volatile OSHeapHandle __OSCurrHeap = -1;

namespace meleeboard::os {

void reset_arena(std::uint32_t physical_bytes)
{
    release_arena();

    // One block, sized to the next power of two and aligned to itself: the
    // low word of every address inside it is then that address's offset from
    // the base, and no two are equal.
    const std::size_t block_bytes = round_up_to_power_of_two(physical_bytes);
    auto* block = static_cast<unsigned char*>(allocate_aligned_block(block_bytes));
    if (block == nullptr) {
        return;
    }

    g_block = block;
    g_block_bytes = block_bytes;
    g_physical_bytes = physical_bytes;

    // The console does not hand the whole of memory to the game: the OS
    // globals sit below the arena and the boot stack above it.
    g_arena_lo = block + kSystemReserveLowBytes;
    g_arena_hi = block + physical_bytes - kSystemReserveHighBytes;
}

void release_arena()
{
    if (g_block != nullptr) {
        free_aligned_block(g_block);
    }
    g_block = nullptr;
    g_block_bytes = 0;
    __OSCurrHeap = -1;
    forget_arena();
}

const void* arena_base() { return g_block; }

std::uint32_t arena_bytes() { return g_physical_bytes; }

bool low_words_are_distinct()
{
    if (g_block == nullptr) {
        return false;
    }
    // Distinct low words means the block cannot straddle a 4 GiB boundary,
    // which holds exactly when it is no larger than 4 GiB and its base is
    // aligned to its own size.  Written against unsigned long long rather
    // than a shift, which would be undefined where size_t is 32 bits wide.
    if (static_cast<unsigned long long>(g_block_bytes) > 0x100000000ULL) {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(g_block);
    return (base & (g_block_bytes - 1)) == 0;
}

struct SavedArena {
    unsigned char* block;
    std::size_t block_bytes;
    std::uint32_t physical_bytes;
    HeapDesc* heaps;
    int heap_count;
    unsigned char* arena_start;
    unsigned char* arena_end;
    unsigned char* arena_lo;
    unsigned char* arena_hi;
    OSHeapHandle current_heap;
};

ArenaScope::ArenaScope(std::uint32_t physical_bytes)
    : saved_(new SavedArena{ g_block, g_block_bytes, g_physical_bytes, g_heaps,
                             g_heap_count, g_arena_start, g_arena_end,
                             g_arena_lo, g_arena_hi, __OSCurrHeap })
{
    // Detach rather than release: the saved state owns the outgoing block now,
    // and reset_arena() must not free it.
    g_block = nullptr;
    g_block_bytes = 0;
    forget_arena();
    reset_arena(physical_bytes);
}

ArenaScope::~ArenaScope()
{
    release_arena();

    g_block = saved_->block;
    g_block_bytes = saved_->block_bytes;
    g_physical_bytes = saved_->physical_bytes;
    g_heaps = saved_->heaps;
    g_heap_count = saved_->heap_count;
    g_arena_start = saved_->arena_start;
    g_arena_end = saved_->arena_end;
    g_arena_lo = saved_->arena_lo;
    g_arena_hi = saved_->arena_hi;
    __OSCurrHeap = saved_->current_heap;

    delete saved_;
}

int live_heap_count()
{
    int live = 0;
    for (int heap = 0; heap < g_heap_count; ++heap) {
        if (g_heaps[heap].size >= 0) {
            ++live;
        }
    }
    return live;
}

} // namespace meleeboard::os

extern "C" {

u32 OSGetPhysicalMemSize(void)
{
    return g_physical_bytes != 0 ? g_physical_bytes
                                 : meleeboard::os::kPhysicalMemBytes;
}

void* OSGetArenaLo(void) { return g_arena_lo; }

void* OSGetArenaHi(void) { return g_arena_hi; }

void OSSetArenaLo(void* newLo) { g_arena_lo = static_cast<unsigned char*>(newLo); }

void OSSetArenaHi(void* newHi) { g_arena_hi = static_cast<unsigned char*>(newHi); }

// Carves the heap-descriptor array out of the bottom of the arena and returns
// the first address past it, which the caller is expected to install as the
// new arenaLo.  Upstream's HSD_OSInit does exactly that.
void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps)
{
    if (arenaStart == nullptr || arenaEnd == nullptr || maxHeaps <= 0) {
        return nullptr;
    }

    const auto start = reinterpret_cast<std::uintptr_t>(arenaStart);
    const auto end = reinterpret_cast<std::uintptr_t>(arenaEnd);
    if (start >= end) {
        return nullptr;
    }

    const auto array_bytes =
        static_cast<std::uintptr_t>(maxHeaps) * sizeof(HeapDesc);
    if ((end - start) < array_bytes + kMinObjectSize) {
        return nullptr;
    }

    g_heaps = reinterpret_cast<HeapDesc*>(arenaStart);
    g_heap_count = maxHeaps;
    for (int heap = 0; heap < g_heap_count; ++heap) {
        g_heaps[heap].size = -1;
        g_heaps[heap].freeList = nullptr;
        g_heaps[heap].allocated = nullptr;
    }

    __OSCurrHeap = -1;
    g_arena_start = reinterpret_cast<unsigned char*>(round_up_32(start + array_bytes));
    g_arena_end = reinterpret_cast<unsigned char*>(round_down_32(end));
    if (g_arena_end <= g_arena_start ||
        static_cast<std::uintptr_t>(g_arena_end - g_arena_start) < kMinObjectSize) {
        g_heaps = nullptr;
        g_heap_count = 0;
        g_arena_start = nullptr;
        g_arena_end = nullptr;
        return nullptr;
    }

    return g_arena_start;
}

OSHeapHandle OSCreateHeap(void* start, void* end)
{
    if (g_heaps == nullptr) {
        return -1;
    }

    const auto block_start = round_up_32(reinterpret_cast<std::uintptr_t>(start));
    const auto block_end = round_down_32(reinterpret_cast<std::uintptr_t>(end));
    if (!valid_block_range(block_start, block_end)) {
        return -1;
    }

    for (OSHeapHandle heap = 0; heap < g_heap_count; ++heap) {
        HeapDesc& descriptor = g_heaps[heap];
        if (descriptor.size >= 0) {
            continue;
        }

        descriptor.size = static_cast<s32>(block_end - block_start);
        descriptor.allocated = nullptr;
        descriptor.freeList = reinterpret_cast<Cell*>(block_start);
        descriptor.freeList->prev = nullptr;
        descriptor.freeList->next = nullptr;
        descriptor.freeList->size = descriptor.size;
        descriptor.freeList->owner = nullptr;
        return heap;
    }

    return -1;
}

void OSDestroyHeap(OSHeapHandle heap)
{
    if (!valid_heap(heap)) {
        return;
    }

    HeapDesc& descriptor = g_heaps[heap];
    descriptor.size = -1;
    descriptor.freeList = nullptr;
    descriptor.allocated = nullptr;
    if (__OSCurrHeap == heap) {
        __OSCurrHeap = -1;
    }
}

void OSAddToHeap(OSHeapHandle heap, void* start, void* end)
{
    if (!valid_heap(heap)) {
        return;
    }

    const auto block_start = round_up_32(reinterpret_cast<std::uintptr_t>(start));
    const auto block_end = round_down_32(reinterpret_cast<std::uintptr_t>(end));
    if (!valid_block_range(block_start, block_end)) {
        return;
    }

    HeapDesc& descriptor = g_heaps[heap];
    auto* cell = reinterpret_cast<Cell*>(block_start);
    cell->prev = nullptr;
    cell->next = nullptr;
    cell->size = static_cast<s32>(block_end - block_start);
    cell->owner = nullptr;
    descriptor.freeList = insert_and_coalesce(descriptor.freeList, cell);
    descriptor.size += cell->size;
}

void* OSAllocFromHeap(OSHeapHandle heap, u32 size)
{
    if (!valid_heap(heap) || size == 0) {
        return nullptr;
    }

    HeapDesc& descriptor = g_heaps[heap];
    const auto requested = static_cast<s32>(
        round_up_32(static_cast<std::uintptr_t>(size) + kHeaderSize));

    Cell* cell = descriptor.freeList;
    while (cell != nullptr && cell->size < requested) {
        cell = cell->next;
    }
    if (cell == nullptr) {
        return nullptr;
    }

    const s32 leftover = cell->size - requested;
    if (leftover < static_cast<s32>(kMinObjectSize)) {
        descriptor.freeList = extract(descriptor.freeList, cell);
    } else {
        auto* split =
            reinterpret_cast<Cell*>(reinterpret_cast<unsigned char*>(cell) + requested);
        split->size = leftover;
        split->owner = nullptr;
        split->prev = cell->prev;
        split->next = cell->next;
        if (split->prev != nullptr) {
            split->prev->next = split;
        } else {
            descriptor.freeList = split;
        }
        if (split->next != nullptr) {
            split->next->prev = split;
        }
        cell->size = requested;
    }

    cell->owner = &descriptor;
    descriptor.allocated = add_front(descriptor.allocated, cell);
    return reinterpret_cast<unsigned char*>(cell) + kHeaderSize;
}

void OSFreeToHeap(OSHeapHandle heap, void* ptr)
{
    if (!valid_heap(heap) || ptr == nullptr) {
        return;
    }
    if (!in_arena(ptr) ||
        (reinterpret_cast<std::uintptr_t>(ptr) & (kAlignment - 1)) != 0) {
        return;
    }

    HeapDesc& descriptor = g_heaps[heap];
    auto* cell =
        reinterpret_cast<Cell*>(reinterpret_cast<unsigned char*>(ptr) - kHeaderSize);
    if (cell->owner != &descriptor || !contains_cell(descriptor.allocated, cell)) {
        return;
    }

    descriptor.allocated = extract(descriptor.allocated, cell);
    cell->owner = nullptr;
    descriptor.freeList = insert_and_coalesce(descriptor.freeList, cell);
}

OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap)
{
    const OSHeapHandle previous = __OSCurrHeap;
    if (heap == -1 || valid_heap(heap)) {
        __OSCurrHeap = heap;
    }
    return previous;
}

// The heap's free bytes, or -1 when walking it finds anything inconsistent.
//
// objalloc.c branches on this -- it stops trimming its pools when the heap
// reports no room -- so a stub that always answers zero would take the port
// down a path the console never takes.
s32 OSCheckHeap(OSHeapHandle heap)
{
    if (!valid_heap(heap)) {
        return -1;
    }

    HeapDesc& descriptor = g_heaps[heap];
    s32 total = 0;
    s32 free_bytes = 0;

    if (descriptor.allocated != nullptr && descriptor.allocated->prev != nullptr) {
        return -1;
    }

    for (Cell* cell = descriptor.allocated; cell != nullptr; cell = cell->next) {
        if (!in_arena(cell) ||
            (reinterpret_cast<std::uintptr_t>(cell) & (kAlignment - 1)) != 0 ||
            cell->size < static_cast<s32>(kMinObjectSize) ||
            (cell->size & static_cast<s32>(kAlignment - 1)) != 0 ||
            cell->owner != &descriptor ||
            (cell->next != nullptr && cell->next->prev != cell)) {
            return -1;
        }
        total += cell->size;
        if (total <= 0 || total > descriptor.size) {
            return -1;
        }
    }

    if (descriptor.freeList != nullptr && descriptor.freeList->prev != nullptr) {
        return -1;
    }

    for (Cell* cell = descriptor.freeList; cell != nullptr; cell = cell->next) {
        if (!in_arena(cell) ||
            (reinterpret_cast<std::uintptr_t>(cell) & (kAlignment - 1)) != 0 ||
            cell->size < static_cast<s32>(kMinObjectSize) ||
            (cell->size & static_cast<s32>(kAlignment - 1)) != 0 ||
            cell->owner != nullptr ||
            (cell->next != nullptr && cell->next->prev != cell)) {
            return -1;
        }
        // Address order, and no overlap with the cell that follows.
        if (cell->next != nullptr &&
            reinterpret_cast<std::uintptr_t>(cell) +
                    static_cast<std::uintptr_t>(cell->size) >
                reinterpret_cast<std::uintptr_t>(cell->next)) {
            return -1;
        }

        total += cell->size;
        free_bytes += cell->size - static_cast<s32>(kHeaderSize);
        if (total <= 0 || total > descriptor.size) {
            return -1;
        }
    }

    if (total != descriptor.size) {
        return -1;
    }
    return free_bytes;
}

u32 OSReferentSize(void* ptr)
{
    if (ptr == nullptr || !in_arena(ptr) ||
        (reinterpret_cast<std::uintptr_t>(ptr) & (kAlignment - 1)) != 0) {
        return 0;
    }
    auto* cell =
        reinterpret_cast<Cell*>(reinterpret_cast<unsigned char*>(ptr) - kHeaderSize);
    if (cell->owner == nullptr) {
        return 0;
    }
    return static_cast<u32>(cell->size - static_cast<s32>(kHeaderSize));
}

} // extern "C"
