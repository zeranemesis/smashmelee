#include "descriptor_arena.hpp"

#include <cstdlib>
#include <cstring>
#include <new>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace meleeboard::hsd {
namespace {

std::size_t round_up_to_power_of_two(std::size_t value)
{
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

void* allocate_aligned(std::size_t bytes, std::size_t alignment)
{
#ifdef _MSC_VER
    return _aligned_malloc(bytes, alignment);
#else
    // aligned_alloc requires the size to be a multiple of the alignment,
    // which it is here: both are the same power of two.
    return std::aligned_alloc(alignment, bytes);
#endif
}

void release_aligned(void* pointer)
{
#ifdef _MSC_VER
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

} // namespace

DescriptorArena::DescriptorArena(std::size_t capacity)
{
    // The alignment has to equal the size: that is what keeps the block from
    // straddling a boundary its own size, and in particular a 4 GiB one.
    capacity_ = round_up_to_power_of_two(capacity);
    base_ = static_cast<unsigned char*>(allocate_aligned(capacity_, capacity_));
    if (base_ == nullptr) {
        throw std::bad_alloc();
    }
}

DescriptorArena::~DescriptorArena() { release_aligned(base_); }

void* DescriptorArena::allocate(std::size_t bytes, std::size_t alignment)
{
    const std::size_t start = (used_ + alignment - 1) & ~(alignment - 1);
    if (bytes > capacity_ - start) {
        return nullptr;
    }
    used_ = start + bytes;
    unsigned char* result = base_ + start;
    std::memset(result, 0, bytes);
    return result;
}

bool DescriptorArena::low_words_are_distinct() const
{
    // A block of size S aligned to S never crosses a multiple of S.  When S
    // is at most 4 GiB, it therefore never crosses a multiple of 4 GiB, so
    // the low word increases monotonically across the whole block and no two
    // addresses in it share one.
    if (capacity_ > 0x100000000ULL) {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(base_);
    return (base & (capacity_ - 1)) == 0;
}

} // namespace meleeboard::hsd
