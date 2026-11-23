// acphal.dll: Audio Control Processor HAL. Scalable Hardware Audio Processing Engine
// (SHAPE) and APU (Audio Processing Unit) physical memory management for Xbox Series.
//
// On a PC there is no SHAPE/ACP hardware and APU physical memory does not exist.
// This shim creates the APU heap from process memory and answers queries from it.
// The SHAPE context arrays are allocated on the process heap; the ACP interface
// accepts and discards commands.
//
// Signatures from the public GDK reference (devdocs.xbox.com):
//   AcpHal, APU reference, SHAPE_CONTEXTS, APU_ADDRESS.

#include <windows.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "shims.h"
#include "startup_trace.h"

namespace {

// ---- APU_ADDRESS (from the GDK) ----

// The console APU uses a 40-bit physical address space.
// On a PC we translate to a host pointer.
struct APU_ADDRESS {
    uint64_t value;  // 40-bit physical address
};

// ---- APU heap state (from the GDK) ----

struct ApuHeapState {
    uint32_t version;
    uint32_t flags;
    uint64_t cachedUsed;
    uint64_t cachedTotal;
    uint64_t nonCachedUsed;
    uint64_t nonCachedTotal;
};

// ---- ACP Command structure (from the GDK, simplified) ----

// We only need to accept and drop them.

// ---- APU heap state ----

std::mutex g_apu_mutex;
bool g_apu_heap_created = false;
uint64_t g_apu_cached_size = 0;
void* g_apu_cached_base = nullptr;
uint64_t g_apu_noncached_size = 0;
void* g_apu_noncached_base = nullptr;

// ---- SHAPE context structures (from the public GDK documentation at devdocs.xbox.com) ----
//
// These structures are defined here with their documented GDK field layouts so that
// sizeof() returns the correct per-context byte count. Without the real layouts the
// empty structs would be 1 byte each, and the title's writes (e.g. memset with the
// real stride) would overflow the allocation.

// UINT32 bitfields packed into 32-bit words
struct SHAPE_SRC_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t blocksToSkip : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t samplingIncrement : 21;
    uint32_t reserved0 : 1;
    uint32_t cmd : 2;
    uint32_t mixBufPeakMag2 : 4;
    uint32_t peakMag2 : 4;
    uint32_t sampleCount;
    uint32_t samplePointer : 25;
    uint32_t reserved1 : 7;
    uint32_t samplingIncrementTarget : 21;
    uint32_t reserved2 : 11;
    uint32_t reserved[3];
};
static_assert(sizeof(SHAPE_SRC_CONTEXT) == 32, "SHAPE_SRC_CONTEXT is 32 bytes per GDK");

// The EQ/Comp context is complex. From the GDK header the packed bitfields and full
// UINT32 members span 88 DWORDs. We don't need the internal fields; we just need the
// right allocation size.
// Parsing the GDK definition, the total is: 88 UINT32 male = 352 bytes.
// For simplicity we use a fixed-size opaque buffer.
#pragma pack(push, 1)
struct alignas(4) SHAPE_EQCOMP_CONTEXT {
    uint8_t data[352];
};
#pragma pack(pop)
static_assert(sizeof(SHAPE_EQCOMP_CONTEXT) == 352, "SHAPE_EQCOMP_CONTEXT is 352 bytes per GDK");

struct SHAPE_FILTVOL_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t delay1 : 30;
    uint32_t headroom : 2;
    uint32_t delay2 : 30;
    uint32_t svfMode : 2;
    uint32_t gainTarget : 16;
    uint32_t gain : 16;
    uint32_t qRecip : 24;
    uint32_t reserved1 : 8;
    uint32_t qRecipTarget : 24;
    uint32_t reserved2 : 8;
    uint32_t fc : 24;
    uint32_t reserved3 : 8;
    uint32_t fcTarget : 24;
    uint32_t reserved4 : 8;
};
static_assert(sizeof(SHAPE_FILTVOL_CONTEXT) == 32, "SHAPE_FILTVOL_CONTEXT is 32 bytes per GDK");

struct SHAPE_DMA_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 3;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t readPointer : 5;
    uint32_t reserved1 : 3;
    uint32_t writePointer : 5;
    uint32_t reserved2 : 3;
    uint32_t full : 1;
    uint32_t reserved3 : 15;
    uint32_t reserved4 : 9;
    uint32_t address : 23;
    uint32_t numFrames : 5;
    uint32_t reserved5 : 3;
    uint32_t floatConvert : 1;
    uint32_t reserved6 : 7;
    uint32_t numChannels : 3;
    uint32_t reserved7 : 5;
    uint32_t channel : 3;
    uint32_t reserved8 : 5;
};
static_assert(sizeof(SHAPE_DMA_CONTEXT) == 16, "SHAPE_DMA_CONTEXT is 16 bytes per GDK");

struct SHAPE_XMA_CONTEXT {
    uint32_t sizeRead0 : 12;
    uint32_t numLoops : 8;
    uint32_t validBuffer : 2;
    uint32_t sizeWrite : 5;
    uint32_t offsetWrite : 5;
    uint32_t sizeRead1 : 12;
    uint32_t loopSubframeEnd : 2;
    uint32_t reserved0 : 3;
    uint32_t loopSubframeSkip : 3;
    uint32_t numSubframesToDecode : 4;
    uint32_t numSubframesToSkip : 3;
    uint32_t sampleRate : 2;
    uint32_t numChannels : 1;
    uint32_t reserved1 : 1;
    uint32_t validWrite : 1;
    uint32_t offsetRead : 26;
    uint32_t errorStatus : 5;
    uint32_t errorSet : 1;
    uint32_t loopStartOffset : 26;
    uint32_t parserErrorStatus : 5;
    uint32_t parserErrorSet : 1;
    uint32_t loopEndOffset : 26;
    uint32_t packetMetaData : 5;
    uint32_t currentBuffer : 1;
    uint32_t ptrRead0;
    uint32_t ptrRead1;
    uint32_t ptrWrite;
    uint32_t ptrOverlapAdd;
    uint32_t writeBufferOffsetRead : 5;
    uint32_t reserved2 : 25;
    uint32_t stopWhenDone : 1;
    uint32_t interruptWhenDone : 1;
    uint32_t reserved[6];
};
static_assert(sizeof(SHAPE_XMA_CONTEXT) == 64, "SHAPE_XMA_CONTEXT is 64 bytes per GDK");

struct SHAPE_PCM_CONTEXT {
    uint32_t bufferStart;
    uint32_t bufferLength : 21;
    uint32_t reserved0 : 11;
    uint32_t loopCount : 8;
    uint32_t reserved1 : 8;
    uint32_t full : 1;
    uint32_t reserved2 : 15;
    uint32_t readPointer : 21;
    uint32_t reserved3 : 11;
    uint32_t loopStartWritePointer : 21;
    uint32_t reserved4 : 11;
    uint32_t loopEnd : 21;
    uint32_t reserved5 : 11;
    uint32_t mode : 1;
    uint32_t reserved6 : 15;
    uint32_t format : 2;
    uint32_t reserved7 : 14;
    uint32_t reserved[1];
};
static_assert(sizeof(SHAPE_PCM_CONTEXT) == 32, "SHAPE_PCM_CONTEXT is 32 bytes per GDK");

// The SHAPE_CONTEXTS structure (from the GDK)
struct SHAPE_CONTEXTS {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    SHAPE_SRC_CONTEXT* srcContextArray;
    SHAPE_EQCOMP_CONTEXT* eqCompContextArray;
    SHAPE_FILTVOL_CONTEXT* filtVolContextArray;
    SHAPE_DMA_CONTEXT* dmaContextArray;
    SHAPE_XMA_CONTEXT* xmaContextArray;
    SHAPE_PCM_CONTEXT* pcmContextArray;
    APU_ADDRESS apuSrcContextArray;
    APU_ADDRESS apuEqCompContextArray;
    APU_ADDRESS apuFiltVolContextArray;
    APU_ADDRESS apuDmaContextArray;
    APU_ADDRESS apuXmaContextArray;
    APU_ADDRESS apuPcmContextArray;
};

static_assert(sizeof(SHAPE_CONTEXTS) == 9 * 8 + 6 * 8, "SHAPE_CONTEXTS size check");

// ---- Startup trace helper ----

void trace_call(const char* api, const std::string& detail) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        std::lock_guard lock(mutex);
        if (!seen.insert(std::string(api) + detail).second) return;
    }
    rt::startup_trace::event("acphal",
                             std::string("\"api\":\"") + api + "\"" + detail);
}

// ---- ACP implementation methods ----

// ---- AcpHalAllocateShapeContexts ----
//
// HRESULT AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* contextArrays)

HRESULT __stdcall AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* ctx) {
    if (!ctx) return E_POINTER;

    // The title calls this with numSrcContexts=0, numXmaContexts=350, numPcmContexts=0
    // (and the other counts unspecified). The GDK documents that SRC count should equal
    // PCM + XMA, but the title does not follow that rule (it allocates XMA contexts
    // independently). Accept any combination rather than rejecting the title's real call.
    // Each context array is zero-initialised so the title can use them immediately.

    auto alloc = [](uint32_t count, size_t elemSize) -> void* {
        if (count == 0) return nullptr;
        return std::calloc(count, elemSize);
    };

    ctx->srcContextArray = static_cast<SHAPE_SRC_CONTEXT*>(
        alloc(ctx->numSrcContexts, sizeof(SHAPE_SRC_CONTEXT)));
    ctx->eqCompContextArray = static_cast<SHAPE_EQCOMP_CONTEXT*>(
        alloc(ctx->numEqCompContexts, sizeof(SHAPE_EQCOMP_CONTEXT)));
    ctx->filtVolContextArray = static_cast<SHAPE_FILTVOL_CONTEXT*>(
        alloc(ctx->numFiltVolContexts, sizeof(SHAPE_FILTVOL_CONTEXT)));
    ctx->dmaContextArray = static_cast<SHAPE_DMA_CONTEXT*>(
        alloc(ctx->numDmaContexts, sizeof(SHAPE_DMA_CONTEXT)));
    ctx->xmaContextArray = static_cast<SHAPE_XMA_CONTEXT*>(
        alloc(ctx->numXmaContexts, sizeof(SHAPE_XMA_CONTEXT)));
    ctx->pcmContextArray = static_cast<SHAPE_PCM_CONTEXT*>(
        alloc(ctx->numPcmContexts, sizeof(SHAPE_PCM_CONTEXT)));

    // APU physical addresses: not meaningful on PC, zero them
    ctx->apuSrcContextArray = {};
    ctx->apuEqCompContextArray = {};
    ctx->apuFiltVolContextArray = {};
    ctx->apuDmaContextArray = {};
    ctx->apuXmaContextArray = {};
    ctx->apuPcmContextArray = {};

    trace_call("AcpHalAllocateShapeContexts",
               ",\"src\":" + std::to_string(ctx->numSrcContexts) +
               ",\"pcm\":" + std::to_string(ctx->numPcmContexts) +
               ",\"xma\":" + std::to_string(ctx->numXmaContexts));
    return S_OK;
}

// ---- AcpHalReleaseShapeContexts ----
//
// HRESULT AcpHalReleaseShapeContexts(SHAPE_CONTEXTS* contextArrays)

HRESULT __stdcall AcpHalReleaseShapeContexts(SHAPE_CONTEXTS* ctx) {
    if (!ctx) return E_POINTER;

    auto free_array = [](void* ptr) {
        if (ptr) std::free(ptr);
    };
    free_array(ctx->srcContextArray);
    free_array(ctx->eqCompContextArray);
    free_array(ctx->filtVolContextArray);
    free_array(ctx->dmaContextArray);
    free_array(ctx->xmaContextArray);
    free_array(ctx->pcmContextArray);

    std::memset(ctx, 0, sizeof(*ctx));

    trace_call("AcpHalReleaseShapeContexts", "");
    return S_OK;
}

// ---- IAcpHal vtable-backed interface (COM-like C++ class) ----
//
// Measured call sites (PR #6885, 1.0.2.1):
//   Slot 0: (0x1000, 0x1000) — Connect-style, result checked (fatal on failure)
//   Slot 1: no arguments at shutdown — Release-style, result checked
//   Slot 2: (cmd_id, dword*, dword, ptr*, dword) — command submit, result checked (fatal on failure)
//   Slot 5: last call before object is set to NULL — Release-style final
//   Slots 3 and 4: never called
//
// Return S_OK for slots 0 and 2 (the title treats failure of these as fatal at boot).
// Slot 1 is Release (returns 1). Slot 5 returns S_OK (frees nothing, no-op).
// Uncalled slots 3 and 4 return 0 (if they are a message poll, 0 = "no message").
// All other slots log once and return E_NOTIMPL.

constexpr size_t kAcpHalSlots = 8;

ULONG __stdcall acphal_add_ref(void*) { return 1; }
ULONG __stdcall acphal_release(void*) { return 1; }

// Boot-critical: the title treats a failed slot 2 as fatal (int3).
// Accept and drop the command.
HRESULT __stdcall acphal_submit_command(void*) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] acphal: IAcpHal::SubmitCommand called; command dropped (no ACP hardware, logged once)\n");
    }
    return S_OK;
}
HRESULT __stdcall acphal_final_release(void*) { return S_OK; }
// Slots 3 and 4 are never called. Return 0 (if they are a message poll, 0 = "no message").
HRESULT __stdcall acphal_noop(void*) { return 0; }

template <size_t Slot>
HRESULT __stdcall acphal_unsupported(void*) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] acphal: IAcpHal::slot%zu called without console ACP hardware; E_NOTIMPL (logged once)\n",
                     Slot);
    }
    return E_NOTIMPL;
}

template <size_t... Slot>
constexpr auto make_acphal_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{
        (Slot == 0 ? reinterpret_cast<void*>(&acphal_add_ref)
         : Slot == 1 ? reinterpret_cast<void*>(&acphal_release)
         : Slot == 2 ? reinterpret_cast<void*>(&acphal_submit_command)
         : Slot == 3 ? reinterpret_cast<void*>(&acphal_noop)
         : Slot == 4 ? reinterpret_cast<void*>(&acphal_noop)
         : Slot == 5 ? reinterpret_cast<void*>(&acphal_final_release)
         : reinterpret_cast<void*>(&acphal_unsupported<Slot>))...};
}

const auto kAcpHalVtable = make_acphal_vtable(std::make_index_sequence<kAcpHalSlots>{});
struct AcpHalObject {
    const void* const* vtable;
};
AcpHalObject g_acphal{kAcpHalVtable.data()};

// ---- AcpHalCreate ----
//
// HRESULT AcpHalCreate(IAcpHal** acp)

HRESULT __stdcall AcpHalCreate(void** acp) {
    if (!acp) return E_POINTER;
    // Return a static object with a proper vtable so any method call through the
    // interface pointer is safe (logs once and returns E_NOTIMPL).
    *acp = &g_acphal;

    trace_call("AcpHalCreate", "");
    return S_OK;
}

// ---- ApuCreateHeap ----
//
// HRESULT ApuCreateHeap(UINT32 cachedSizeInBytes, UINT32 nonCachedSizeInBytes)

HRESULT __stdcall ApuCreateHeap(uint32_t cachedSizeInBytes,
                                 uint32_t nonCachedSizeInBytes) {
    std::lock_guard lock(g_apu_mutex);
    if (g_apu_heap_created)
        return 0x8004BA01;  // APU_E_HEAP_ALREADY_ALLOCATED

    if (cachedSizeInBytes == 0)
        return E_INVALIDARG;

    // Allocate the APU heap from VirtualAlloc
    g_apu_cached_size = cachedSizeInBytes;
    g_apu_cached_base = VirtualAlloc(nullptr, cachedSizeInBytes,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_apu_cached_base) return E_OUTOFMEMORY;

    if (nonCachedSizeInBytes > 0) {
        g_apu_noncached_size = nonCachedSizeInBytes;
        g_apu_noncached_base = VirtualAlloc(nullptr, nonCachedSizeInBytes,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_apu_noncached_base) {
            VirtualFree(g_apu_cached_base, 0, MEM_RELEASE);
            g_apu_cached_base = nullptr;
            g_apu_cached_size = 0;
            return E_OUTOFMEMORY;
        }
    }

    g_apu_heap_created = true;
    trace_call("ApuCreateHeap",
               ",\"cachedSize\":" + std::to_string(cachedSizeInBytes));
    return S_OK;
}

// ---- ApuAlloc ----
//
// Measured (PR #6885): ApuAlloc(void** va, uint32_t* apu, UINT32 size,
//                                UINT32 align, UINT32 flags)
//   apu is NULL, align=0x800, flags=2.  The flags 2 is probably "non-cached";
//   accept any value since the title uses 2, not the GDK's documented 0/1.

HRESULT __stdcall ApuAlloc(void** virtualAddress, uint32_t* physicalAddress,
                            uint32_t sizeInBytes, uint32_t alignmentInBytes,
                            uint32_t flags) {
    if (!virtualAddress) return E_POINTER;
    *virtualAddress = nullptr;
    if (physicalAddress) *physicalAddress = {};

    if (sizeInBytes == 0 || alignmentInBytes == 0 ||
        (alignmentInBytes & 3) != 0 ||
        (alignmentInBytes & (alignmentInBytes - 1)) != 0)
        return E_INVALIDARG;

    std::lock_guard lock(g_apu_mutex);

    // The title always passes flags=2 (non-cached per #6885), but the public GDK
    // docs define 0=cached and 1=non-cached.  Treat anything non-zero as
    // non-cached so both work.
    bool cached = (flags == 0);
    void* heap_base = cached ? g_apu_cached_base : g_apu_noncached_base;
    uint64_t heap_size = cached ? g_apu_cached_size : g_apu_noncached_size;

    if (!heap_base) {
        // No heap was created for this type.  If ApuCreateHeap was never called at all
        // (no cached heap either), create a default cached heap.  Otherwise a non-cached
        // allocation when there is no non-cached part is a caller error.
        if (cached) {
            // No cached heap either → create a default cached heap.
            uint32_t default_cached = 64 * 1024 * 1024;
            heap_base = VirtualAlloc(nullptr, default_cached,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!heap_base) return E_OUTOFMEMORY;
            g_apu_cached_base = heap_base;
            g_apu_cached_size = default_cached;
            g_apu_heap_created = true;
            heap_size = default_cached;
        } else {
            // Non-cached request but no non-cached heap was created.
            return E_OUTOFMEMORY;
        }
    }

    // Best-effort small bump allocator, one offset per heap so cached and non-cached
    // allocations don't steal each other's space.
    // This is a simplified version; the real console uses a proper heap.
    static uint64_t cached_bump_offset = 0;
    static uint64_t noncached_bump_offset = 0;
    uint64_t& bump_offset = cached ? cached_bump_offset : noncached_bump_offset;
    uint64_t aligned = (bump_offset + alignmentInBytes - 1) & ~(static_cast<uint64_t>(alignmentInBytes) - 1);
    if (aligned + sizeInBytes > heap_size) return E_OUTOFMEMORY;
    bump_offset = aligned + sizeInBytes;

    *virtualAddress = static_cast<uint8_t*>(heap_base) + aligned;
    if (physicalAddress) {
        // The console uses 32-bit APU physical addresses (PR #6885 measurement).
        // Assign disjoint ranges so cached and non-cached addresses never collide:
        //   cached:    0x40000000 .. 0x40000000 + cached_size - 1
        //   non-cached: 0x40000000 + cached_size .. 0x40000000 + cached_size + noncached_size - 1
        // This lets ApuMapApuAddress determine the heap from the offset alone.
        uint32_t base_addr = 0x40000000u;
        uintptr_t offset;
        if (cached || !g_apu_noncached_base) {
            offset = static_cast<uint8_t*>(*virtualAddress) - static_cast<uint8_t*>(g_apu_cached_base);
        } else {
            offset = static_cast<uint8_t*>(*virtualAddress) - static_cast<uint8_t*>(g_apu_noncached_base);
            base_addr += static_cast<uint32_t>(g_apu_cached_size);
        }
        *physicalAddress = static_cast<uint32_t>(base_addr + offset);
    }

    return S_OK;
}

// ---- ApuFree ----
//
// void ApuFree(void* virtualAddress)

void __stdcall ApuFree(void* virtualAddress) {
    if (!virtualAddress) return;
    // The bump allocator doesn't support free. No-op. The real APU heap
    // lives for the process lifetime anyway.
    trace_call("ApuFree", "");
}

// ---- ApuIsVirtualAddressValid ----
//
// Measured (PR #6885): bool ApuIsVirtualAddressValid(void* va, UINT32 align)
//   align is 0x40, 0x80 or 0x800 at the four call sites.  Return TRUE when
//   the address is in one of the APU heaps and its offset is aligned.

BOOL __stdcall ApuIsVirtualAddressValid(void* virtualAddress, uint32_t alignment) {
    if (!virtualAddress) return FALSE;
    if (alignment == 0) alignment = 1;
    std::lock_guard lock(g_apu_mutex);
    // Check if the address falls within either APU heap
    uintptr_t addr = reinterpret_cast<uintptr_t>(virtualAddress);
    uintptr_t cached_start = reinterpret_cast<uintptr_t>(g_apu_cached_base);
    uintptr_t noncached_start = reinterpret_cast<uintptr_t>(g_apu_noncached_base);

    auto in_range = [&](uintptr_t start, uint64_t size) -> BOOL {
        if (!start) return FALSE;
        if (addr < start || addr >= start + size) return FALSE;
        return ((addr - start) & (alignment - 1)) == 0 ? TRUE : FALSE;
    };

    if (cached_start && in_range(cached_start, g_apu_cached_size)) return TRUE;
    if (noncached_start && in_range(noncached_start, g_apu_noncached_size)) return TRUE;
    return FALSE;
}

// ---- ApuMapApuAddress ----
//
// Measured (PR #6885): void* ApuMapApuAddress(UINT32 apu)
//   Translates a 32-bit APU physical address back to a host pointer.
//   0 returns NULL.

void* __stdcall ApuMapApuAddress(uint32_t apu) {
    if (apu == 0) return nullptr;
    std::lock_guard lock(g_apu_mutex);
    // Disjoint APU address ranges (see ApuAlloc):
    //   cached:    0x40000000 .. 0x40000000 + cached_size - 1
    //   non-cached: 0x40000000 + cached_size .. 0x40000000 + cached_size + noncached_size - 1
    uint32_t offset = apu - 0x40000000u;
    uint32_t cached_base = 0;
    if (g_apu_cached_base && offset < g_apu_cached_size) {
        return static_cast<uint8_t*>(g_apu_cached_base) + offset;
    }
    if (g_apu_noncached_base) {
        cached_base = static_cast<uint32_t>(g_apu_cached_size);
        if (offset >= cached_base && offset - cached_base < g_apu_noncached_size)
            return static_cast<uint8_t*>(g_apu_noncached_base) + (offset - cached_base);
    }
    return nullptr;
}

// ---- ApuMapVirtualAddress ----
//
// Measured (PR #6885): UINT32 ApuMapVirtualAddress(void* va)
//   Returns a 32-bit APU address for the host pointer.  A PC has no real
//   APU address space, so encode the heap offset into the APU address.
//   NULL maps to 0.

uint32_t __stdcall ApuMapVirtualAddress(void* virtualAddress) {
    if (!virtualAddress) return 0;
    std::lock_guard lock(g_apu_mutex);
    uintptr_t addr = reinterpret_cast<uintptr_t>(virtualAddress);
    uintptr_t cached_start = reinterpret_cast<uintptr_t>(g_apu_cached_base);
    uintptr_t noncached_start = reinterpret_cast<uintptr_t>(g_apu_noncached_base);

    if (cached_start && addr >= cached_start &&
        addr < cached_start + g_apu_cached_size)
        return 0x40000000u + static_cast<uint32_t>(addr - cached_start);
    if (noncached_start && addr >= noncached_start &&
        addr < noncached_start + g_apu_noncached_size)
        return 0x40000000u + static_cast<uint32_t>(g_apu_cached_size) +
               static_cast<uint32_t>(addr - noncached_start);
    return 0;
}

}  // namespace

SHIM_TABLE(acphal,
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts)},
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&AcpHalCreate)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&AcpHalReleaseShapeContexts)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&ApuAlloc)},
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&ApuCreateHeap)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&ApuFree)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&ApuIsVirtualAddressValid)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&ApuMapApuAddress)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&ApuMapVirtualAddress)},
);