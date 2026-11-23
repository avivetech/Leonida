// Checks the registered xg_xs.dll shim against the call shapes measured in the game executable (runtime/shims/xg_xs.cpp).
//
// Both exports are resolved through rt::shims::find, so this is the table the runtime installs. The checks:
// - XGSuggestSwizzleMode returns S_OK and writes 0x101 for 2D textures (0x100 for linear/1D), E_INVALIDARG on bad input.
// - XGCreateTextureComputer returns S_OK with a ref-counted computer whose slots 2 (GetResourceLayout), 8/9
//   (CopyIntoSubresource / CopyFromSubresource) and 10 (GetResourceTiling) match runtime/src/xg_layout.h.
// - Invalid descriptions return E_INVALIDARG while still storing a safe fallback computer in *out for callers that do
//   not check HRESULT, and unimplemented slots (e.g. slot 31) still return E_NOTIMPL without writing caller buffers.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/xg_layout.h"
#include "shims.h"

namespace {

using CreateFn = HRESULT(__stdcall*)(const void*, void**);
using SuggestFn = HRESULT(__stdcall*)(const void*, uint32_t, uint32_t*);
using AddRefFn = ULONG(__stdcall*)(void*);
using ReleaseFn = ULONG(__stdcall*)(void*);
using GetLayoutFn = HRESULT(__stdcall*)(void*, void*);
using CopyIntoSubresourceFn = HRESULT(__stdcall*)(void*, void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
using CopyFromSubresourceFn =
    HRESULT(__stdcall*)(void*, void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
using GetTilingFn = HRESULT(__stdcall*)(void*, uint32_t*, void*, void*, uint32_t*, uint32_t, void*);
constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

}  // namespace

int main() {
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto suggest = reinterpret_cast<SuggestFn>(rt::shims::find("xg_xs.dll", "XGSuggestSwizzleMode"));
    check(create != nullptr, "XGCreateTextureComputer is registered");
    check(suggest != nullptr, "XGSuggestSwizzleMode is registered");
    check(rt::shims::owns_dll("xg_xs.dll"), "the registry owns xg_xs.dll");
    if (!create || !suggest) return 1;

    leonida::xg::XgResourceDesc desc{};
    desc.dimension = leonida::xg::kDimTexture2D;
    desc.width = 512;
    desc.height = 512;
    desc.depth_or_array_size = 1;
    desc.mip_levels = 10;
    desc.format = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    desc.sample_count = 1;
    desc.layout = leonida::xg::kLayoutUnknown;

    uint32_t mode = 0;
    check(suggest(&desc, 0, &mode) == S_OK, "XGSuggestSwizzleMode returns S_OK for a valid 2D texture");
    check(mode == leonida::xg::kLayoutSw64KbD, "XGSuggestSwizzleMode suggests 0x101 (SW_64KB_D) for 2D");
    check(suggest(nullptr, 0, &mode) == E_INVALIDARG, "XGSuggestSwizzleMode(nullptr, ...) is E_INVALIDARG");

    desc.layout = mode;
    check(create(&desc, nullptr) == E_POINTER, "XGCreateTextureComputer(desc, nullptr) is E_POINTER");

    void* computer = nullptr;
    check(create(&desc, &computer) == S_OK && computer != nullptr,
          "XGCreateTextureComputer returns S_OK with a computer");
    if (!computer) return 1;

    void* const* vtable = *static_cast<void* const* const*>(computer);

    // Slot 2: GetResourceLayout
    leonida::xg::XgResourceLayout layout{};
    check(reinterpret_cast<GetLayoutFn>(vtable[2])(computer, &layout) == S_OK, "slot 2 (GetResourceLayout) succeeds");
    check(layout.planes == 1 && layout.mip_levels == 10 && layout.size_bytes > 0,
          "slot 2 populates planes, mip_levels and size_bytes");
    check(layout.plane[0].mip_layout[0].size_bytes == 16u * 65536u,
          "slot 2 mip 0 size matches 512x512 RGBA8 in 64 KiB tiles (16 tiles)");

    // Slot 8 & 9: CopyIntoSubresource / CopyFromSubresource round-trip
    std::vector<uint8_t> tiled_buf(static_cast<size_t>(layout.size_bytes), 0);
    std::vector<uint8_t> src(512 * 512 * 4);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
    check(reinterpret_cast<CopyIntoSubresourceFn>(vtable[8])(computer, tiled_buf.data(), 0, 0, src.data(), 512 * 4, 0) == S_OK,
          "slot 8 (CopyIntoSubresource) succeeds");
    std::vector<uint8_t> roundtrip(src.size(), 0);
    check(reinterpret_cast<CopyFromSubresourceFn>(vtable[9])(computer, roundtrip.data(), 0, 0, 0, tiled_buf.data(),
                                                             512 * 4, 0) == S_OK,
          "slot 9 (CopyFromSubresource) succeeds");
    check(roundtrip == src, "slots 8 and 9 round-trip a 512x512 RGBA8 swizzled surface");

    // Slot 10: GetResourceTiling
    uint32_t total_tiles = 0;
    leonida::xg::XgPackedMipDesc packed{};
    leonida::xg::XgTileShape shape{};
    uint32_t sub_count = 16;
    leonida::xg::XgSubresourceTiling tilings[16] = {};
    check(reinterpret_cast<GetTilingFn>(vtable[10])(computer, &total_tiles, &packed, &shape, &sub_count, 0, tilings) == S_OK,
          "slot 10 (GetResourceTiling) succeeds");
    check(total_tiles == 22 && packed.num_standard_mips == 3 && packed.num_packed_mips == 7 && sub_count == 10,
          "slot 10 matches expected tile count and mip split for 512x512 RGBA8");

    // Unimplemented slot still logs and returns E_NOTIMPL.
    check(reinterpret_cast<GetLayoutFn>(vtable[31])(computer, &layout) == kNotImpl,
          "unimplemented slot (31) returns E_NOTIMPL");

    // Ref counting: AddRef -> 2, Release -> 1, Release -> 0.
    auto add_ref = reinterpret_cast<AddRefFn>(vtable[0]);
    auto release = reinterpret_cast<ReleaseFn>(vtable[1]);
    check(add_ref(computer) == 2, "AddRef increments refcount to 2");
    check(release(computer) == 1, "first Release drops refcount to 1");
    check(release(computer) == 0, "final Release destroys the computer");

    // Invalid desc still returns E_INVALIDARG and hands out the safe static fallback computer.
    leonida::xg::XgResourceDesc bad{};
    void* fallback = nullptr;
    check(create(&bad, &fallback) == E_INVALIDARG && fallback != nullptr,
          "invalid desc returns E_INVALIDARG with a non-null fallback computer");
    if (fallback) {
        void* const* fb_vt = *static_cast<void* const* const*>(fallback);
        check(reinterpret_cast<GetLayoutFn>(fb_vt[2])(fallback, &layout) == kNotImpl,
              "fallback computer slot 2 returns E_NOTIMPL");
        reinterpret_cast<ReleaseFn>(fb_vt[1])(fallback);
        reinterpret_cast<ReleaseFn>(fb_vt[1])(fallback);
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
