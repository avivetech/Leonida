// xg_xs.dll: the console's texture layout library. The game executable (1.0.2.1) imports two functions from it.
//
// Measured from the call sites in the executable, read statically (shapes and checks only):
//   HRESULT XGSuggestSwizzleMode(const XG_RESOURCE_DESC* desc, uint32_t flags, uint32_t* mode)
//     2 call sites, both in the helper that fills a 0x38-byte D3D12_RESOURCE_DESC before XGCreateTextureComputer.
//     The out pointer is the layout field at +0x2C (which the helper sets to 0x100|mode when known).
//   HRESULT XGCreateTextureComputer(const XG_RESOURCE_DESC* desc, XGTextureAddressComputer** out)
//     4 call sites. Two test the HRESULT and fail when negative; two ignore it and call through *out at once.
//     Callers use vtable slots:
//       - slot 0 / 1: AddRef / Release
//       - slot 2: GetResourceLayout(XG_RESOURCE_LAYOUT* out) (0x1B38 bytes: 4 planes of 0x6C8, 15 mips of 0x70)
//       - slot 8: CopyIntoSubresource(dst, plane, subresource, src, srcRowPitch, srcSlicePitch)
//       - slot 9: CopyFromSubresource(dst, plane, subresource, flags, src, dstRowPitch, dstSlicePitch)
//       - slot 10: GetResourceTiling(numTiles, packedMipDesc, tileShape, numSubresourceTilings, first, tilings)
//
// Layout, tiling and GFX10 swizzling live in the portable header runtime/src/xg_layout.h (tested by xg_layout_test).
// Unimplemented slots up to kSlots still log once and return E_NOTIMPL without touching caller outputs.
// When XGCreateTextureComputer receives an unsupported/invalid description, it returns E_INVALIDARG and still stores
// a safe static fallback computer in *out so callers that ignore the HRESULT do not dereference null.
#include <windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <new>
#include <utility>

#include "../src/xg_layout.h"
#include "shims.h"

namespace {

constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr size_t kSlots = 32;

// Prints once per entry point; `slot` is the vtable index for a computer method, or -1 for an export.
void warn_once(std::atomic<bool>& warned, const char* what, int slot, const char* detail, const void* caller) {
    if (warned.exchange(true, std::memory_order_relaxed)) return;
    char name[64];
    if (slot < 0) std::snprintf(name, sizeof(name), "%s", what);
    else std::snprintf(name, sizeof(name), "%s%d", what, slot);
    std::fprintf(stderr, "[rt] xg_xs: %s: %s (caller %s, logged once)\n",
                 name, detail, rt::shims::module_offset(reinterpret_cast<uint64_t>(caller)).c_str());
}

struct XgComputer {
    const void* const* vtable;
    std::atomic<ULONG> ref_count;
    bool is_static_fallback;
    leonida::xg::XgResourceDesc desc;
    leonida::xg::XgResourceLayout layout;
};

ULONG __stdcall computer_add_ref(void* self_ptr) {
    auto* self = static_cast<XgComputer*>(self_ptr);
    if (!self || self->is_static_fallback) return 1;
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG __stdcall computer_release(void* self_ptr) {
    auto* self = static_cast<XgComputer*>(self_ptr);
    if (!self || self->is_static_fallback) return 1;
    const ULONG remaining = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) delete self;
    return remaining;
}

HRESULT __stdcall computer_get_resource_layout(void* self_ptr, void* out_layout) {
    auto* self = static_cast<XgComputer*>(self_ptr);
    if (!self || self->is_static_fallback) return kNotImpl;
    if (!out_layout) return E_POINTER;
    *static_cast<leonida::xg::XgResourceLayout*>(out_layout) = self->layout;
    return S_OK;
}

HRESULT __stdcall computer_copy_into_subresource(void* self_ptr, void* destination, uint32_t plane,
                                                 uint32_t subresource, const void* source,
                                                 uint32_t source_row_pitch, uint32_t source_slice_pitch) {
    auto* self = static_cast<XgComputer*>(self_ptr);
    if (!self || self->is_static_fallback) return kNotImpl;
    return leonida::xg::copy_into_subresource(self->desc, self->layout, destination, plane, subresource, source,
                                              source_row_pitch, source_slice_pitch)
               ? S_OK
               : E_INVALIDARG;
}

HRESULT __stdcall computer_copy_from_subresource(void* self_ptr, void* destination, uint32_t plane,
                                                 uint32_t subresource, uint32_t flags, const void* source,
                                                 uint32_t destination_row_pitch, uint32_t destination_slice_pitch) {
    (void)flags;
    auto* self = static_cast<XgComputer*>(self_ptr);
    if (!self || self->is_static_fallback) return kNotImpl;
    return leonida::xg::copy_from_subresource(self->desc, self->layout, destination, plane, subresource, source,
                                              destination_row_pitch, destination_slice_pitch)
               ? S_OK
               : E_INVALIDARG;
}

HRESULT __stdcall computer_get_resource_tiling(void* self_ptr, uint32_t* num_tiles_for_entire_resource,
                                               void* packed_mip_desc, void* standard_tile_shape,
                                               uint32_t* num_subresource_tilings,
                                               uint32_t first_subresource_tiling_to_get, void* subresource_tilings) {
    auto* self = static_cast<XgComputer*>(self_ptr);
    if (!self || self->is_static_fallback) return kNotImpl;
    return leonida::xg::compute_resource_tiling(
               self->desc, num_tiles_for_entire_resource,
               static_cast<leonida::xg::XgPackedMipDesc*>(packed_mip_desc),
               static_cast<leonida::xg::XgTileShape*>(standard_tile_shape), num_subresource_tilings,
               first_subresource_tiling_to_get,
               static_cast<leonida::xg::XgSubresourceTiling*>(subresource_tilings))
               ? S_OK
               : E_INVALIDARG;
}

template <size_t Slot>
HRESULT __stdcall computer_unimplemented_slot(void*) {
    static std::atomic<bool> warned{false};
    warn_once(warned, "texture computer slot ", static_cast<int>(Slot), "E_NOTIMPL", _ReturnAddress());
    return kNotImpl;
}

template <size_t... Slot>
constexpr auto make_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{
        (Slot == 0    ? reinterpret_cast<void*>(&computer_add_ref)
         : Slot == 1  ? reinterpret_cast<void*>(&computer_release)
         : Slot == 2  ? reinterpret_cast<void*>(&computer_get_resource_layout)
         : Slot == 8  ? reinterpret_cast<void*>(&computer_copy_into_subresource)
         : Slot == 9  ? reinterpret_cast<void*>(&computer_copy_from_subresource)
         : Slot == 10 ? reinterpret_cast<void*>(&computer_get_resource_tiling)
                      : reinterpret_cast<void*>(&computer_unimplemented_slot<Slot>))...};
}

const auto kVtable = make_vtable(std::make_index_sequence<kSlots>{});
XgComputer g_fallback_computer{kVtable.data(), {1}, true, {}, {}};

HRESULT __stdcall XGCreateTextureComputer(const void* desc_ptr, void** out) {
    if (!out) return E_POINTER;
    if (!desc_ptr) {
        *out = &g_fallback_computer;
        return E_INVALIDARG;
    }
    const auto& desc = *static_cast<const leonida::xg::XgResourceDesc*>(desc_ptr);
    leonida::xg::XgResourceLayout layout{};
    if (!leonida::xg::compute_resource_layout(desc, layout)) {
        static std::atomic<bool> warned{false};
        warn_once(warned, "XGCreateTextureComputer", -1, "unsupported desc; returning E_INVALIDARG", _ReturnAddress());
        *out = &g_fallback_computer;  // two measured callers use *out without checking HRESULT
        return E_INVALIDARG;
    }
    auto* comp = new (std::nothrow) XgComputer{kVtable.data(), {1}, false, desc, layout};
    if (!comp) {
        *out = &g_fallback_computer;
        return E_OUTOFMEMORY;
    }
    *out = comp;
    return S_OK;
}

HRESULT __stdcall XGSuggestSwizzleMode(const void* desc_ptr, uint32_t flags, uint32_t* mode) {
    if (!desc_ptr || !mode) return E_INVALIDARG;
    const auto& desc = *static_cast<const leonida::xg::XgResourceDesc*>(desc_ptr);
    uint32_t suggested = 0;
    if (!leonida::xg::suggest_swizzle_mode(desc, flags, suggested)) {
        static std::atomic<bool> warned{false};
        warn_once(warned, "XGSuggestSwizzleMode", -1, "unsupported desc; returning E_INVALIDARG", _ReturnAddress());
        return E_INVALIDARG;
    }
    *mode = suggested;
    return S_OK;
}

}  // namespace

SHIM_TABLE(xg_xs,
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&XGCreateTextureComputer)},
    {"xg_xs.dll", "XGSuggestSwizzleMode", reinterpret_cast<void*>(&XGSuggestSwizzleMode)},
);
