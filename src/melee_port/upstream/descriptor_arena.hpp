#pragma once

// Storage placed so that the low word of every address in it is unique.
//
// Upstream keys its ID table on a joint descriptor's address, truncated to 32
// bits: HSD_JObjLoadJoint registers `(u32) joint`, and five places look one
// back up the same way -- jobj.c for a child, pobj.c for a rigid primitive's
// joint and for every envelope weight, robj.c for a constraint's target and
// for an rvalue.  On a 64-bit host that truncation loses information.  Two
// descriptors whose addresses differ only above bit 31 share a key, and the
// table answers with whichever was registered last; the symptom is not a
// crash but a constraint or a skinning weight quietly following the wrong
// bone.  tests/hsd/test_upstream_core.cpp demonstrates it directly.
//
// The port could not change the key without changing upstream.  So it changes
// where the descriptors live instead: **one** allocation whose size is a power
// of two and whose base is aligned to that size cannot straddle a 4 GiB
// boundary, so the low word of any address inside it is that address's offset
// within it -- distinct by construction.  One allocation, not a chain: two
// separately aligned blocks could collide with each other.
//
// Only HSD_Joint needs this.  It is the only structure whose address upstream
// ever truncates.

#include <cstddef>
#include <cstdint>

namespace meleeboard::hsd {

class DescriptorArena {
public:
    // Sixteen mebibytes holds tens of thousands of joints, which is more than
    // any one archive carries.  The size is rounded up to a power of two.
    static constexpr std::size_t kDefaultCapacity = 16u * 1024u * 1024u;

    explicit DescriptorArena(std::size_t capacity = kDefaultCapacity);
    ~DescriptorArena();

    DescriptorArena(const DescriptorArena&) = delete;
    DescriptorArena& operator=(const DescriptorArena&) = delete;

    // Zeroed storage for `bytes`, aligned to `alignment`, or null when the
    // arena is full.  Nothing is ever released individually: the arena's
    // whole point is that the addresses it hands out stay distinct in their
    // low word for as long as anything holds one.
    void* allocate(std::size_t bytes, std::size_t alignment);

    template <typename T> T* allocate_one()
    {
        return static_cast<T*>(allocate(sizeof(T), alignof(T)));
    }

    std::size_t capacity() const { return capacity_; }
    std::size_t used() const { return used_; }

    // True when every address this arena can hand out has a distinct low
    // word -- which is the whole reason it exists, so it is checkable.
    bool low_words_are_distinct() const;

private:
    unsigned char* base_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t used_ = 0;
};

} // namespace meleeboard::hsd
