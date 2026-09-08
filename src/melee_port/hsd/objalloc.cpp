#include <melee/sysdolphin/baselib/objalloc.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>

struct HSD_HostAllocBlock {
    void* raw;
    HSD_HostAllocBlock* next;
};

namespace {

struct HostObjectHeap {
    std::byte* current = nullptr;
    std::byte* end = nullptr;
};

HostObjectHeap gObjectHeap;
HSD_ObjAllocData* gAllocators = nullptr;

bool is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

size_t next_power_of_two(size_t value)
{
    size_t result = 1;
    while (result < value && result <= std::numeric_limits<size_t>::max() / 2) {
        result <<= 1;
    }
    return result;
}

uintptr_t align_up(uintptr_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
}

void release_blocks(HSD_ObjAllocData* data)
{
    HSD_HostAllocBlock* block = data->host_blocks;
    while (block != nullptr) {
        HSD_HostAllocBlock* next = block->next;
        std::free(block->raw);
        std::free(block);
        block = next;
    }
    data->host_blocks = nullptr;
}

bool unregister_allocator(HSD_ObjAllocData* data)
{
    HSD_ObjAllocData** current = &gAllocators;
    while (*current != nullptr) {
        if (*current == data) {
            *current = data->next;
            return true;
        }
        current = &(*current)->next;
    }
    return false;
}

void* allocate_pool(HSD_ObjAllocData* data, size_t size)
{
    const size_t alignment = data->align + 1;
    if (gObjectHeap.current != nullptr) {
        const uintptr_t start = align_up(
            reinterpret_cast<uintptr_t>(gObjectHeap.current), alignment);
        const uintptr_t end = reinterpret_cast<uintptr_t>(gObjectHeap.end);
        if (start > end || size > end - start) {
            return nullptr;
        }
        gObjectHeap.current = reinterpret_cast<std::byte*>(start + size);
        return reinterpret_cast<void*>(start);
    }

    if (size > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        return nullptr;
    }
    void* raw = std::malloc(size + alignment - 1);
    if (raw == nullptr) {
        return nullptr;
    }
    auto* block = static_cast<HSD_HostAllocBlock*>(
        std::malloc(sizeof(HSD_HostAllocBlock)));
    if (block == nullptr) {
        std::free(raw);
        return nullptr;
    }
    block->raw = raw;
    block->next = data->host_blocks;
    data->host_blocks = block;
    return reinterpret_cast<void*>(align_up(reinterpret_cast<uintptr_t>(raw), alignment));
}

size_t available_heap_bytes()
{
    if (gObjectHeap.current == nullptr) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(gObjectHeap.end - gObjectHeap.current);
}

} // namespace

extern "C" void HSD_ObjSetHeap(size_t size, void* ptr)
{
    gObjectHeap.current = static_cast<std::byte*>(ptr);
    gObjectHeap.end = ptr != nullptr ? gObjectHeap.current + size : nullptr;
}

extern "C" int32_t HSD_ObjAllocAddFree(HSD_ObjAllocData* data, uint32_t num)
{
    if (data == nullptr || num == 0 || data->size == 0 ||
        num > std::numeric_limits<size_t>::max() / data->size) {
        return 0;
    }

    const size_t pool_size = data->size * num;
    auto* pool = static_cast<std::byte*>(allocate_pool(data, pool_size));
    if (pool == nullptr) {
        return 0;
    }

    for (uint32_t i = 0; i + 1 < num; ++i) {
        auto* link = reinterpret_cast<HSD_ObjAllocLink*>(pool + data->size * i);
        link->next = reinterpret_cast<HSD_ObjAllocLink*>(
            pool + data->size * (i + 1));
    }
    auto* tail = reinterpret_cast<HSD_ObjAllocLink*>(
        pool + data->size * (num - 1));
    tail->next = data->freehead;
    data->freehead = reinterpret_cast<HSD_ObjAllocLink*>(pool);
    data->free += num;
    return static_cast<int32_t>(num);
}

extern "C" void* HSD_ObjAlloc(HSD_ObjAllocData* data)
{
    if (data == nullptr) {
        return nullptr;
    }
    if (data->num_limit_flag != 0 && data->used >= data->num_limit) {
        return nullptr;
    }
    if (data->heap_limit_flag != 0) {
        const size_t available = available_heap_bytes();
        if (data->heap_limit_num == std::numeric_limits<uint32_t>::max()) {
            if (available <= data->heap_limit_size) {
                data->heap_limit_num = data->used + data->free;
            }
        } else if (available > data->heap_limit_size) {
            data->heap_limit_num = std::numeric_limits<uint32_t>::max();
        }
        if (data->used >= data->heap_limit_num) {
            return nullptr;
        }
    }
    if (data->free == 0 && HSD_ObjAllocAddFree(data, 1) == 0) {
        return nullptr;
    }

    HSD_ObjAllocLink* result = data->freehead;
    data->freehead = result->next;
    ++data->used;
    --data->free;
    data->peak = std::max(data->peak, data->used);
    return result;
}

extern "C" void HSD_ObjFree(HSD_ObjAllocData* data, void* obj)
{
    if (data == nullptr || obj == nullptr || data->used == 0) {
        return;
    }
    auto* link = static_cast<HSD_ObjAllocLink*>(obj);
    link->next = data->freehead;
    data->freehead = link;
    ++data->free;
    --data->used;
}

extern "C" void HSD_ObjAllocInit(HSD_ObjAllocData* data, size_t size,
                                   size_t alignment)
{
    if (data == nullptr) {
        return;
    }
    if (unregister_allocator(data)) {
        release_blocks(data);
    }

    std::memset(data, 0, sizeof(*data));
    alignment = std::max(alignment, alignof(HSD_ObjAllocLink));
    if (!is_power_of_two(alignment)) {
        alignment = next_power_of_two(alignment);
    }
    data->align = alignment - 1;
    data->size = std::max(size, sizeof(HSD_ObjAllocLink));
    data->size = align_up(data->size, alignment);
    data->num_limit = std::numeric_limits<uint32_t>::max();
    data->heap_limit_num = std::numeric_limits<uint32_t>::max();
    data->next = gAllocators;
    gAllocators = data;
}

extern "C" void HSD_ObjAllocShutdown(HSD_ObjAllocData* data)
{
    if (data == nullptr || !unregister_allocator(data)) {
        return;
    }
    release_blocks(data);
    std::memset(data, 0, sizeof(*data));
}

extern "C" void _HSD_ObjAllocForgetMemory(void*, void*)
{
    HSD_ObjAllocData* current = gAllocators;
    while (current != nullptr) {
        HSD_ObjAllocData* next = current->next;
        release_blocks(current);
        current->freehead = nullptr;
        current->used = 0;
        current->free = 0;
        current->peak = 0;
        current->next = nullptr;
        current = next;
    }
    gAllocators = nullptr;
    gObjectHeap = {};
}
