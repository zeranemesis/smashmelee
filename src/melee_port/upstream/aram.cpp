#include "aram.hpp"

#include "os_scheduler.hpp"

#include <cstdlib>
#include <cstring>

namespace {

unsigned char* g_aram = nullptr;

// The console keeps its allocation stack in the first 0x4000 bytes of ARAM, so
// ARAlloc's first block does not start at zero.  Reproducing that keeps an
// ARAM offset of zero distinguishable from a real block, the same distinction
// Archive::kNullOffset draws for data-section offsets.
constexpr std::uint32_t kStackBytes = 0x4000;

std::uint32_t g_next = kStackBytes;
std::uint32_t* g_stack = nullptr;
std::uint32_t g_stack_entries = 0;
std::uint32_t g_stack_depth = 0;

std::vector<meleeboard::os::AramTransfer> g_transfers;

// A posted request, waiting for the interrupt that completes it.
struct Pending {
    meleeboard::os::AramTransfer transfer;
    ARQRequest* request = nullptr;
    ARQCallback callback = nullptr;
};
std::vector<Pending> g_queue;
bool g_initialized = false;
bool g_queue_initialized = false;
ARQCallback g_dma_callback = nullptr;
std::uint32_t g_chunk_size = 4096;

bool ensure_aram()
{
    if (g_aram != nullptr) {
        return true;
    }
    g_aram = static_cast<unsigned char*>(
        std::calloc(1, meleeboard::os::kAramBytes));
    return g_aram != nullptr;
}

// ARAM offsets are offsets, not addresses: the CPU cannot address ARAM, which
// is why a transfer is the only way in or out.
unsigned char* host_of(std::uint32_t offset, std::uint32_t length)
{
    if (g_aram == nullptr || length > meleeboard::os::kAramBytes ||
        offset > meleeboard::os::kAramBytes - length) {
        return nullptr;
    }
    return g_aram + offset;
}

// One copy, immediately.  See aram.hpp for why the transfer is synchronous.
//
// The main-memory end is a real host pointer rather than a u32, which is the
// whole reason ARStartDMA is not defined in this file: its main-memory
// parameter is a 32-bit physical address, and there is no host address that
// fits.  Aurora widened ARQPostRequest's endpoints to uintptr_t, so the queue
// -- which is the only way the game moves anything -- carries a host pointer
// intact.  Nothing in Melee calls ARStartDMA, and leaving it undefined means
// a future caller gets a link error rather than a silently truncated copy.
void transfer(u32 type, unsigned char* main, u32 aram_offset, u32 length)
{
    unsigned char* aram = host_of(aram_offset, length);
    if (aram == nullptr || main == nullptr || length == 0) {
        return;
    }
    if (type == ARAM_DIR_MRAM_TO_ARAM) {
        std::memcpy(aram, main, length);
    } else {
        std::memcpy(main, aram, length);
    }
}

} // namespace

namespace meleeboard::os {

void reset_aram()
{
    if (g_aram != nullptr) {
        std::free(g_aram);
        g_aram = nullptr;
    }
    g_next = kStackBytes;
    g_stack = nullptr;
    g_stack_entries = 0;
    g_stack_depth = 0;
    g_transfers.clear();
    g_queue.clear();
    g_initialized = false;
    g_queue_initialized = false;
    g_dma_callback = nullptr;
    g_chunk_size = 4096;
}

const std::vector<AramTransfer>& aram_transfers() { return g_transfers; }

std::size_t aram_queue_depth() { return g_queue.size(); }

std::size_t service_aram_queue()
{
    std::size_t completed = 0;
    while (!g_queue.empty() && interrupts_enabled()) {
        const Pending pending = g_queue.front();
        g_queue.erase(g_queue.begin());

        // ARQ names its endpoints by direction rather than by argument
        // position: a write to ARAM has `source` in main memory and `dest` an
        // ARAM offset, and a read has them the other way round.
        if (pending.transfer.type == ARQ_TYPE_MRAM_TO_ARAM) {
            transfer(ARAM_DIR_MRAM_TO_ARAM,
                     reinterpret_cast<unsigned char*>(pending.transfer.source),
                     static_cast<u32>(pending.transfer.dest),
                     pending.transfer.length);
        } else {
            transfer(ARAM_DIR_ARAM_TO_MRAM,
                     reinterpret_cast<unsigned char*>(pending.transfer.dest),
                     static_cast<u32>(pending.transfer.source),
                     pending.transfer.length);
        }
        g_transfers.push_back(pending.transfer);
        ++completed;

        if (pending.callback != nullptr) {
            pending.callback(reinterpret_cast<uintptr_t>(pending.request));
        }
    }
    return completed;
}

std::uint32_t aram_used() { return g_next - kStackBytes; }

const unsigned char* aram_bytes(std::uint32_t offset, std::uint32_t length)
{
    return host_of(offset, length);
}

} // namespace meleeboard::os

extern "C" {

u32 ARInit(u32* stack_index_addr, u32 num_entries)
{
    if (!ensure_aram()) {
        return 0;
    }
    g_stack = stack_index_addr;
    g_stack_entries = num_entries;
    g_stack_depth = 0;
    g_next = kStackBytes;
    g_initialized = true;
    return kStackBytes;
}

BOOL ARCheckInit(void) { return g_initialized ? TRUE : FALSE; }

void ARReset(void) { g_initialized = false; }

u32 ARGetBaseAddress(void) { return 0; }

u32 ARGetSize(void) { return meleeboard::os::kAramBytes; }

u32 ARGetInternalSize(void) { return meleeboard::os::kAramBytes; }

void ARSetSize(void) {}

void ARClear(u32 flag)
{
    (void) flag;
    if (g_aram != nullptr) {
        std::memset(g_aram, 0, meleeboard::os::kAramBytes);
    }
}

// A bump allocator, as on the console: blocks come off the top of what is
// already handed out and the stack remembers where each one started, so only
// the most recent can be released.
u32 ARAlloc(u32 length)
{
    if (!g_initialized || !ensure_aram()) {
        return 0;
    }
    const u32 rounded = (length + 31u) & ~31u;
    if (rounded > meleeboard::os::kAramBytes - g_next) {
        return 0;
    }
    const u32 block = g_next;
    g_next += rounded;
    if (g_stack != nullptr && g_stack_depth < g_stack_entries) {
        g_stack[g_stack_depth++] = block;
    }
    return block;
}

u32 ARFree(u32* length)
{
    if (!g_initialized || g_stack == nullptr || g_stack_depth == 0) {
        return 0;
    }
    const u32 block = g_stack[--g_stack_depth];
    if (length != NULL) {
        *length = g_next - block;
    }
    g_next = block;
    return block;
}

void* ARGetStorageAddress(void) { return g_aram; }

ARQCallback ARRegisterDMACallback(ARQCallback callback)
{
    ARQCallback previous = g_dma_callback;
    g_dma_callback = callback;
    return previous;
}

u32 ARGetDMAStatus(void) { return 0; }

void ARQInit(void)
{
    ensure_aram();
    g_queue_initialized = true;
}

void ARQReset(void) { g_queue_initialized = false; }

BOOL ARQCheckInit(void) { return g_queue_initialized ? TRUE : FALSE; }

void ARQSetChunkSize(u32 size) { g_chunk_size = size; }

u32 ARQGetChunkSize(void) { return g_chunk_size; }

void ARQFlushQueue(void) {}

void ARQRemoveRequest(ARQRequest* request) { (void) request; }

void ARQRemoveOwnerRequest(u32 owner) { (void) owner; }

void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority,
                    uintptr_t source, uintptr_t dest, u32 length,
                    ARQCallback callback)
{
    if (request != NULL) {
        request->next = NULL;
        request->owner = owner;
        request->type = type;
        request->priority = priority;
        // ARQRequest's endpoints are u32 in the SDK's own structure, so the
        // host address written back here is truncated.  Nothing reads it --
        // the queue completes the transfer itself -- and aram_transfers()
        // keeps the untruncated pair for a test.
        request->source = static_cast<u32>(source);
        request->dest = static_cast<u32>(dest);
        request->length = length;
        request->callback = callback;
    }

    // Queued, not performed.  See aram.hpp: the caller's next statement may
    // still be using the bookkeeping this transfer's callback tears down.
    g_queue.push_back(Pending{
        meleeboard::os::AramTransfer{ type, owner, priority, source, dest,
                                      length },
        request, callback });
}

} // extern "C"
