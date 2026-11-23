// Checks the registered acphal.dll shim against the measured call shapes
// (docs/audio_hal_exe_calls.md, PR #6885 measurements).
//
// All 9 exports are resolved through rt::shims::find so the test exercises
// the same shim table and calling convention the runtime installs.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

using AcpHalCreateFn = HRESULT(__stdcall*)(void**);
using AcpHalAllocShapeCtxFn = HRESULT(__stdcall*)(void*);
using AcpHalReleaseShapeCtxFn = HRESULT(__stdcall*)(void*);
using ApuCreateHeapFn = HRESULT(__stdcall*)(uint32_t, uint32_t);
// Measured: flags=2, apu=NULL, align=0x800 (PR #6885)
using ApuAllocFn = HRESULT(__stdcall*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
using ApuFreeFn = void(__stdcall*)(void*);
// Measured: ApuIsVirtualAddressValid(void* va, uint32_t align)
using ApuIsValidFn = BOOL(__stdcall*)(void*, uint32_t);
using ApuMapApuAddrFn = void*(__stdcall*)(uint32_t);
using ApuMapVirtualAddrFn = uint32_t(__stdcall*)(void*);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

}  // namespace

int main() {
    std::printf("acphal_test: registered acphal.dll shim\n");

    const auto create = reinterpret_cast<AcpHalCreateFn>(
        rt::shims::find("acphal.dll", "AcpHalCreate"));
    const auto alloc_ctx = reinterpret_cast<AcpHalAllocShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalAllocateShapeContexts"));
    const auto release_ctx = reinterpret_cast<AcpHalReleaseShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalReleaseShapeContexts"));
    const auto create_heap = reinterpret_cast<ApuCreateHeapFn>(
        rt::shims::find("acphal.dll", "ApuCreateHeap"));
    const auto alloc = reinterpret_cast<ApuAllocFn>(
        rt::shims::find("acphal.dll", "ApuAlloc"));
    const auto free_mem = reinterpret_cast<ApuFreeFn>(
        rt::shims::find("acphal.dll", "ApuFree"));
    const auto is_valid = reinterpret_cast<ApuIsValidFn>(
        rt::shims::find("acphal.dll", "ApuIsVirtualAddressValid"));
    const auto map_apu = reinterpret_cast<ApuMapApuAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapApuAddress"));
    const auto map_virtual = reinterpret_cast<ApuMapVirtualAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapVirtualAddress"));

    check(create != nullptr, "AcpHalCreate is registered");
    check(alloc_ctx != nullptr, "AcpHalAllocateShapeContexts is registered");
    check(release_ctx != nullptr, "AcpHalReleaseShapeContexts is registered");
    check(create_heap != nullptr, "ApuCreateHeap is registered");
    check(alloc != nullptr, "ApuAlloc is registered");
    check(free_mem != nullptr, "ApuFree is registered");
    check(is_valid != nullptr, "ApuIsVirtualAddressValid is registered");
    check(map_apu != nullptr, "ApuMapApuAddress is registered");
    check(map_virtual != nullptr, "ApuMapVirtualAddress is registered");
    check(rt::shims::owns_dll("acphal.dll"), "the registry owns acphal.dll");
    if (!create || !alloc_ctx || !release_ctx || !create_heap || !alloc ||
        !free_mem || !is_valid || !map_apu || !map_virtual)
        return 1;

    // AcpHalCreate(IAcpHal** out): returns a dereferenceable vtable-backed object.
    void* acp = nullptr;
    check(create(&acp) == S_OK && acp != nullptr,
          "AcpHalCreate returns S_OK with a non-null object");
    if (acp) {
        // The returned object has a vtable. Slots 0/1 are AddRef/Release; slot 2
        // (SubmitCommand) and slot 5 (final Release) return S_OK because the title
        // treats their failure as fatal at boot. Slots 3/4 return 0 (message poll),
        // never called. Use a void(__stdcall*)(void) alias to call raw slots.
        using SlotFn = HRESULT(__stdcall*)(void*);
        auto vtable = *static_cast<void* const* const*>(acp);
        check(vtable[0] != nullptr, "IAcpHal vtable slot 0 (AddRef) is present");
        check(vtable[1] != nullptr, "IAcpHal vtable slot 1 (Release) is present");
        check(vtable[2] != nullptr, "IAcpHal vtable slot 2 (SubmitCommand) is present");
        // Slot 2 must return S_OK, the title treats a failure as fatal.
        check(reinterpret_cast<SlotFn>(vtable[2])(acp) == S_OK,
              "IAcpHal slot 2 returns S_OK (title treats failure as fatal)");
    }
    check(create(nullptr) == E_POINTER,
          "AcpHalCreate(nullptr) returns E_POINTER");

    // AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* ctx): accepts any combination of counts.
    // The title calls with numSrcContexts=0, numXmaContexts=350, numPcmContexts=0.
    // Build a SHAPE_CONTEXTS-like buffer to pass (we need the exact struct layout).
    // The struct has 6 x uint32_t + 6 x pointer + 6 x APU_ADDRESS (uint64_t).
    uint64_t shape_ctx[(6 + 6 + 6) * 8 / 8] = {};
    // Set some context counts.
    ((uint32_t*)shape_ctx)[0] = 0;   // numSrcContexts
    ((uint32_t*)shape_ctx)[1] = 0;   // numEqCompContexts
    ((uint32_t*)shape_ctx)[2] = 0;   // numFiltVolContexts
    ((uint32_t*)shape_ctx)[3] = 0;   // numDmaContexts
    ((uint32_t*)shape_ctx)[4] = 350; // numXmaContexts
    ((uint32_t*)shape_ctx)[5] = 0;   // numPcmContexts
    check(alloc_ctx(shape_ctx) == S_OK,
          "AcpHalAllocateShapeContexts succeeds with the title's counts");
    // The xmaContextArray pointer should be non-null (350 elements * 64 bytes = 22400 bytes).
    // Struct layout: 6 uint32_t (24 B), then 6 pointers starting at offset 24.
    // xmaContextArray is the 5th pointer (offset 56 = uint64_t index 7).
    void* xma_array = ((void**)shape_ctx)[7];
    check(xma_array != nullptr,
          "AcpHalAllocateShapeContexts returns a non-null XMA context array");

    // The XMA contexts are 64 bytes each: the first use is memset(ptr, 0, 350*64).
    if (xma_array) {
        std::memset(xma_array, 0, 350 * 64);
        check(true, "the XMA context array can be written at its full stride");
    }

    // Release the contexts.
    check(release_ctx(shape_ctx) == S_OK,
          "AcpHalReleaseShapeContexts succeeds");

    // AcpHalCreate returns a dereferenceable object once more (second call works too).
    void* acp2 = nullptr;
    check(create(&acp2) == S_OK && acp2 != nullptr,
          "AcpHalCreate second call returns S_OK with a non-null object");

    // ApuCreateHeap(cachedSize, nonCachedSize): the title passes (size, 0x226e00).
    check(create_heap(4 * 1024 * 1024, 0x226e00) == S_OK,
          "ApuCreateHeap succeeds");
    check(create_heap(4 * 1024 * 1024, 0) != S_OK,
          "ApuCreateHeap second call returns an error (heap already created)");

    // ApuAlloc(virtualAddress*, physicalAddress*, size, alignment, flags).
    // Measured: flags=2 at one call site, flags=1 at another (PR #6885, confirmed
    // by static read of 1.0.2.1).  Test the real boot flow: ApuCreateHeap creates
    // a large cached heap and a smaller non-cached heap; flags=2 goes to non-cached,
    // flags=1 goes to cached.
    //
    // The title's boot sequence:
    //   ApuCreateHeap(cached_size, 0x226e00)
    //   ApuAlloc(0x222e00, flags=2)   -- needs the non-cached heap, fits because 0x226e00 > 0x222e00
    //   ApuAlloc(small, flags=1)      -- needs the cached heap
    void* va_nc = nullptr;
    // 0x222e00 is the title's real non-cached allocation size, must fit in the 0x226e00 heap.
    check(alloc(&va_nc, nullptr, 0x222e00, 0x800, 2) == S_OK && va_nc != nullptr,
          "ApuAlloc flags=2 at title's size 0x222e00 succeeds (within the 0x226e00 non-cached heap)");
    void* va_c = nullptr;
    // flags=1 goes to the large cached heap.
    check(alloc(&va_c, nullptr, 4096, 0x800, 1) == S_OK && va_c != nullptr,
          "ApuAlloc flags=1 succeeds on the cached heap (even after non-cached is mostly full)");
    if (va_nc && va_c) {
        check(is_valid(va_nc, 0x800) == TRUE,
              "flags=2 allocation is in a valid heap");
        check(is_valid(va_c, 0x800) == TRUE,
              "flags=1 allocation is in a valid heap");
        check(va_nc != va_c,
              "flags=1 and flags=2 allocations are in different heaps");
        // APU addresses use disjoint ranges.
        const uint32_t apu_nc = map_virtual(va_nc);
        const uint32_t apu_c = map_virtual(va_c);
        check(apu_nc != apu_c,
              "APU addresses are distinct for flags=1 and flags=2");
        check(map_apu(apu_nc) == va_nc,
              "ApuMapApuAddress round-trips the flags=2 block");
        check(map_apu(apu_c) == va_c,
              "ApuMapApuAddress round-trips the flags=1 block");
    }

    // Regression: cached (flags=0) and non-cached (flags=2) use separate bump offsets.
    void* cached_big = nullptr;
    void* noncached_block = nullptr;
    // 3 MiB fits in the 4 MiB cached heap but exceeds the ~2.2 MiB non-cached heap.
    const HRESULT cr_cached = alloc(&cached_big, nullptr, 3 * 1024 * 1024, 0x800, 0);
    check(cr_cached == S_OK && cached_big != nullptr,
          "ApuAlloc cached (flags=0) 3 MiB succeeds (within the 4 MiB cached heap)");
    const HRESULT cr_noncached = alloc(&noncached_block, nullptr, 0x1000, 0x800, 2);
    check(cr_noncached == S_OK && noncached_block != nullptr,
          "ApuAlloc non-cached (flags=2) 4 KiB still succeeds after the 0x222e00 one");
    if (cached_big && noncached_block) {
        check(is_valid(cached_big, 0x800) == TRUE,
              "cached 3 MiB block is in a valid heap");
        check(is_valid(noncached_block, 0x800) == TRUE,
              "non-cached 4 KiB block is in a valid heap");
        check(cached_big != noncached_block,
              "cached and non-cached allocations return different addresses");
        const uint32_t cached_apu = map_virtual(cached_big);
        const uint32_t noncached_apu = map_virtual(noncached_block);
        check(cached_apu != noncached_apu,
              "cached and non-cached APU addresses are distinct");
        check(map_apu(cached_apu) == cached_big,
              "ApuMapApuAddress round-trips the cached block (disjoint APU ranges)");
        check(map_apu(noncached_apu) == noncached_block,
              "ApuMapApuAddress round-trips the non-cached block (disjoint APU ranges)");
    }

    // ApuFree(void*): void return, no-op.
    free_mem(va_nc);
    free_mem(nullptr);
    check(true, "ApuFree accepts both a valid pointer and nullptr without crashing");

    // ApuIsVirtualAddressValid on null or invalid addresses returns FALSE.
    check(is_valid(nullptr, 0x800) == FALSE,
          "ApuIsVirtualAddressValid(nullptr, *any*) returns FALSE");
    check(is_valid((void*)1, 0x800) == FALSE,
          "ApuIsVirtualAddressValid(invalid) returns FALSE");

    // ApuMapApuAddress(uint32_t apu): translate 32-bit APU address to virtual.
    check(map_apu(0) == nullptr,
          "ApuMapApuAddress(0) returns nullptr");

    // ApuMapVirtualAddress(void*): returns uint32_t APU address.
    check(map_virtual(nullptr) == 0,
          "ApuMapVirtualAddress(nullptr) returns 0");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}