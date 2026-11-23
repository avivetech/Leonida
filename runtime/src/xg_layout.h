#pragma once

// Portable Xbox Series xg_xs.dll texture layout, tiling, and subresource copy model (no Windows or Vulkan dependency).
//
// Struct layouts match the Xbox Series GDK xg_xs.dll / IXGTextureAddressComputer ABI (verified against the public
// XG_RESOURCE_LAYOUT / XG_PLANE_LAYOUT / XG_MIPLEVEL_LAYOUT geometry and observed call shapes: callers allocate
// 0x1B38 bytes for GetResourceLayout, copy 0x6C8-byte plane records from layout+0x18, index 0x70-byte mip records
// from plane+0x38 reading +0x00/+0x10/+0x1C, and write 0x100|hw_mode directly into the XGSuggestSwizzleMode out word):
// - XgResourceDesc (0x38 bytes): identical field layout to D3D12_RESOURCE_DESC, with console layout words 0x100|mode
//   in `layout` (0x100 linear, 0x101 SW_64KB_D, 0x102 SW_4KB_S, 0x10A reserved 64 KiB tiled).
// - XgResourceLayout (0x1B38 bytes): 0x18-byte header + 4 XgPlaneLayout entries (0x6C8 bytes each).
// - XgPlaneLayout (0x6C8 bytes): 0x38-byte header (bytes_per_slice at +0x10) + 15 XgMipLevelLayout entries (0x70 bytes).
// - XgMipLevelLayout (0x70 bytes): size_bytes (+0x00), offset_bytes (+0x08), slice_2d_size_bytes (+0x10),
//   pitch_pixels (+0x18), pitch_bytes (+0x1C), padded/unpadded element counts, sample count, and tile mode.
// - XgPackedMipDesc / XgTileShape / XgSubresourceTiling (0x0C bytes each): identical layout to D3D12's tiling structs.
//
// Scope / simplifications of the current layout model:
// - Mips are placed largest-first with each tiled mip rounded up to whole tiles (no GFX10 mip-tail packing; console
//   mip-tail layout remains open per docs/d3d12_vulkan.md).
// - Subresource copy supports 1D/2D/2D-array/3D linear textures and 2D/2D-array swizzled textures via TiledSurface2D.
//   Swizzled 3D subresource copy is refused (returns false) until 3D tile swizzle addressing is implemented.
// - Unknown desc.layout words are rejected rather than defaulted.

#include "gfx10_tiling.h"
#include "resource_tiling.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace leonida::xg {

inline constexpr std::uint32_t kMaxPlanes = 4;
inline constexpr std::uint32_t kMaxMips = 15;

// Console D3D12_TEXTURE_LAYOUT / XG swizzle mode values (0x100 | hw_mode).
inline constexpr std::uint32_t kLayoutUnknown = 0;
inline constexpr std::uint32_t kLayoutRowMajor = 1;
inline constexpr std::uint32_t kLayout64KbUndefinedSwizzle = 2;
inline constexpr std::uint32_t kLayout64KbStandardSwizzle = 3;
inline constexpr std::uint32_t kLayoutLinear = 0x100;
inline constexpr std::uint32_t kLayoutSw64KbD = 0x101;
inline constexpr std::uint32_t kLayoutSw4KbS = 0x102;
inline constexpr std::uint32_t kLayoutSw64KbReserved = 0x10A;

// D3D12_RESOURCE_DIMENSION values.
inline constexpr std::uint32_t kDimBuffer = 1;
inline constexpr std::uint32_t kDimTexture1D = 2;
inline constexpr std::uint32_t kDimTexture2D = 3;
inline constexpr std::uint32_t kDimTexture3D = 4;

// D3D12 linear placement / row-pitch alignments.
inline constexpr std::uint64_t kLinearPitchAlignment = 256;
inline constexpr std::uint64_t kLinearPlacementAlignment = 512;
inline constexpr std::uint64_t kSmallTileAlignment = 4096;
inline constexpr std::uint64_t kTileAlignment = 65536;

// Matches D3D12_RESOURCE_DESC / XG_RESOURCE_DESC (56 bytes).
struct XgResourceDesc {
    std::uint32_t dimension = 0;
    std::uint32_t pad0 = 0;
    std::uint64_t alignment = 0;
    std::uint64_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t depth_or_array_size = 0;
    std::uint16_t mip_levels = 0;
    std::uint32_t format = 0;
    std::uint32_t sample_count = 1;
    std::uint32_t sample_quality = 0;
    std::uint32_t layout = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(XgResourceDesc) == 0x38);
static_assert(offsetof(XgResourceDesc, alignment) == 0x08);
static_assert(offsetof(XgResourceDesc, width) == 0x10);
static_assert(offsetof(XgResourceDesc, height) == 0x18);
static_assert(offsetof(XgResourceDesc, depth_or_array_size) == 0x1C);
static_assert(offsetof(XgResourceDesc, mip_levels) == 0x1E);
static_assert(offsetof(XgResourceDesc, format) == 0x20);
static_assert(offsetof(XgResourceDesc, sample_count) == 0x24);
static_assert(offsetof(XgResourceDesc, layout) == 0x2C);
static_assert(offsetof(XgResourceDesc, flags) == 0x30);

// Matches XG_MIPLEVEL_LAYOUT (112 bytes = 0x70).
struct XgMipLevelLayout {
    std::uint64_t size_bytes = 0;                // +0x00
    std::uint64_t offset_bytes = 0;              // +0x08
    std::uint64_t slice_2d_size_bytes = 0;       // +0x10
    std::uint32_t pitch_pixels = 0;              // +0x18
    std::uint32_t pitch_bytes = 0;               // +0x1C
    std::uint32_t padded_width_elements = 0;     // +0x20
    std::uint32_t padded_height_elements = 0;    // +0x24
    std::uint32_t padded_depth_or_array = 0;     // +0x28
    std::uint32_t width_elements = 0;            // +0x2C
    std::uint32_t height_elements = 0;           // +0x30
    std::uint32_t depth_or_array = 0;            // +0x34
    std::uint32_t sample_count = 0;              // +0x38
    std::uint32_t tile_mode = 0;                 // +0x3C
    std::uint64_t reserved[6] = {};              // +0x40..0x6F
};
static_assert(sizeof(XgMipLevelLayout) == 0x70);
static_assert(offsetof(XgMipLevelLayout, size_bytes) == 0x00);
static_assert(offsetof(XgMipLevelLayout, offset_bytes) == 0x08);
static_assert(offsetof(XgMipLevelLayout, slice_2d_size_bytes) == 0x10);
static_assert(offsetof(XgMipLevelLayout, pitch_bytes) == 0x1C);

// Matches XG_PLANE_LAYOUT (1736 bytes = 0x6C8).
struct XgPlaneLayout {
    std::uint32_t usage = 0;                     // +0x00 (0=default/color, 1=depth, 2=stencil)
    std::uint32_t pad0 = 0;                      // +0x04
    std::uint64_t size_bytes = 0;                // +0x08
    std::uint64_t bytes_per_slice = 0;           // +0x10 (array slice stride)
    std::uint64_t base_offset_bytes = 0;         // +0x18
    std::uint64_t base_alignment_bytes = 0;      // +0x20
    std::uint32_t bytes_per_element = 0;         // +0x28
    std::uint32_t pad1 = 0;                      // +0x2C
    std::uint64_t reserved = 0;                  // +0x30
    XgMipLevelLayout mip_layout[kMaxMips] = {};  // +0x38
};
static_assert(sizeof(XgPlaneLayout) == 0x6C8);
static_assert(offsetof(XgPlaneLayout, size_bytes) == 0x08);
static_assert(offsetof(XgPlaneLayout, bytes_per_slice) == 0x10);
static_assert(offsetof(XgPlaneLayout, mip_layout) == 0x38);

// Matches XG_RESOURCE_LAYOUT (6968 bytes = 0x1B38).
struct XgResourceLayout {
    std::uint64_t size_bytes = 0;                // +0x00
    std::uint64_t base_alignment_bytes = 0;      // +0x08
    std::uint32_t mip_levels = 0;                // +0x10
    std::uint32_t planes = 0;                    // +0x14
    XgPlaneLayout plane[kMaxPlanes] = {};        // +0x18
};
static_assert(sizeof(XgResourceLayout) == 0x1B38);
static_assert(offsetof(XgResourceLayout, plane) == 0x18);

// Matches D3D12_PACKED_MIP_INFO / XG_PACKED_MIP_DESC (12 bytes).
struct XgPackedMipDesc {
    std::uint8_t num_standard_mips = 0;
    std::uint8_t num_packed_mips = 0;
    std::uint16_t pad0 = 0;
    std::uint32_t num_tiles_for_packed_mips = 0;
    std::uint32_t start_tile_index_in_overall_resource = 0;
};
static_assert(sizeof(XgPackedMipDesc) == 0x0C);

// Matches D3D12_TILE_SHAPE / XG_TILE_SHAPE (12 bytes).
struct XgTileShape {
    std::uint32_t width_in_texels = 0;
    std::uint32_t height_in_texels = 0;
    std::uint32_t depth_in_texels = 0;
};
static_assert(sizeof(XgTileShape) == 0x0C);

// Matches D3D12_SUBRESOURCE_TILING / XG_SUBRESOURCE_TILING (12 bytes).
struct XgSubresourceTiling {
    std::uint32_t width_in_tiles = 0;
    std::uint16_t height_in_tiles = 0;
    std::uint16_t depth_in_tiles = 0;
    std::uint32_t start_tile_index_in_overall_resource = 0;
};
static_assert(sizeof(XgSubresourceTiling) == 0x0C);

struct FormatInfo {
    std::uint32_t bytes_per_element = 0;   // bytes per texel, or per 4x4 block for BC formats
    std::uint32_t block_dim = 1;           // 1 for uncompressed, 4 for BC1..BC7
    std::uint32_t planes = 1;              // 2 for depth+stencil (plane 1 is 1-byte stencil)
    std::uint32_t plane1_bpe = 0;          // bytes per element for plane 1 when planes == 2
    bool depth = false;
};

// Standard DXGI_FORMAT geometry (numeric IDs from public dxgiformat.h).
constexpr FormatInfo dxgi_format_info(std::uint32_t dxgi_format) {
    switch (dxgi_format) {
    // 16 bytes per texel (128 bpp)
    case 1: case 2: case 3: case 4:  // R32G32B32A32_*
        return {16, 1, 1, 0, false};

    // 12 bytes per texel (96 bpp)
    case 5: case 6: case 7: case 8:  // R32G32B32_*
        return {12, 1, 1, 0, false};

    // 8 bytes per texel (64 bpp)
    case 9: case 10: case 11: case 12: case 13: case 14:  // R16G16B16A16_*
    case 15: case 16: case 17: case 18:                    // R32G32_*
        return {8, 1, 1, 0, false};

    // 32-bit depth + 8-bit stencil (2 planes: 4-byte depth, 1-byte stencil)
    case 19: case 20: case 21: case 22:  // R32G8X24_TYPELESS, D32_FLOAT_S8X24_UINT, R32_FLOAT_X8X24, X32_TYPELESS_G8X24
        return {4, 1, 2, 1, true};

    // 4 bytes per texel (32 bpp)
    case 23: case 24: case 25: case 26:                    // R10G10B10A2_*
    case 27: case 28: case 29: case 30: case 31: case 32:  // R8G8B8A8_*
    case 33: case 34: case 35: case 36: case 37: case 38:  // R16G16_*
    case 39: case 41: case 42: case 43:                    // R32_TYPELESS, R32_FLOAT, R32_UINT, R32_SINT
    case 67:                                                // R9G9B9E5_SHAREDEXP
    case 87: case 88: case 90: case 91: case 92: case 93:  // B8G8R8A8_*, B8G8R8X8_*
        return {4, 1, 1, 0, false};

    // 32-bit depth only
    case 40:  // D32_FLOAT
        return {4, 1, 1, 0, true};

    // 24-bit depth + 8-bit stencil (2 planes: 4-byte packed depth, 1-byte stencil)
    case 44: case 45: case 46: case 47:  // R24G8_TYPELESS, D24_UNORM_S8_UINT, R24_UNORM_X8, X24_TYPELESS_G8
        return {4, 1, 2, 1, true};

    // Xbox 16-bit depth + 8-bit stencil (2 planes: 2-byte depth, 1-byte stencil)
    case 117: case 118: case 119: case 120:  // R16G8_TYPELESS, D16_UNORM_S8_UINT, R16_UNORM_X8, X16_TYPELESS_G8
        return {2, 1, 2, 1, true};

    // 2 bytes per texel (16 bpp)
    case 48: case 49: case 50: case 51: case 52:           // R8G8_*
    case 53: case 54: case 56: case 57: case 58: case 59:  // R16_* (except D16)
    case 85: case 86: case 115:                             // B5G6R5, B5G5R5A1, B4G4R4A4
        return {2, 1, 1, 0, false};

    case 55:  // D16_UNORM
        return {2, 1, 1, 0, true};

    // 1 byte per texel (8 bpp)
    case 60: case 61: case 62: case 63: case 64: case 65:  // R8_*, A8_UNORM
        return {1, 1, 1, 0, false};

    // 8 bytes per 4x4 block (BC1, BC4)
    case 70: case 71: case 72:  // BC1_TYPELESS, BC1_UNORM, BC1_UNORM_SRGB
    case 79: case 80: case 81:  // BC4_TYPELESS, BC4_UNORM, BC4_SNORM
        return {8, 4, 1, 0, false};

    // 16 bytes per 4x4 block (BC2, BC3, BC5, BC6H, BC7)
    case 73: case 74: case 75:  // BC2_*
    case 76: case 77: case 78:  // BC3_*
    case 82: case 83: case 84:  // BC5_*
    case 94: case 95: case 96:  // BC6H_*
    case 97: case 98: case 99:  // BC7_*
        return {16, 4, 1, 0, false};

    default:
        return {};
    }
}

namespace detail {

constexpr bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if (a != 0 && b > (std::numeric_limits<std::uint64_t>::max)() / a) return false;
    out = a * b;
    return true;
}

constexpr bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if (b > (std::numeric_limits<std::uint64_t>::max)() - a) return false;
    out = a + b;
    return true;
}

constexpr bool align_up(std::uint64_t value, std::uint64_t alignment, std::uint64_t& out) {
    if (alignment == 0) {
        out = value;
        return true;
    }
    const std::uint64_t rem = value % alignment;
    if (rem == 0) {
        out = value;
        return true;
    }
    return checked_add(value, alignment - rem, out);
}

constexpr std::uint64_t ceil_div(std::uint64_t value, std::uint64_t by) {
    return value ? 1u + (value - 1u) / by : 0u;
}

constexpr std::uint32_t resolve_mip_count(const XgResourceDesc& desc) {
    if (desc.mip_levels > 0) return desc.mip_levels;
    std::uint64_t max_dim = desc.width;
    if (desc.dimension == kDimTexture2D || desc.dimension == kDimTexture3D) {
        max_dim = std::max<std::uint64_t>(max_dim, desc.height);
    }
    if (desc.dimension == kDimTexture3D) {
        max_dim = std::max<std::uint64_t>(max_dim, desc.depth_or_array_size);
    }
    std::uint32_t mips = 1;
    while (max_dim > 1 && mips < kMaxMips) {
        max_dim >>= 1;
        ++mips;
    }
    return mips;
}

constexpr bool is_known_layout(std::uint32_t layout) {
    switch (layout) {
    case kLayoutUnknown:
    case kLayoutRowMajor:
    case kLayout64KbUndefinedSwizzle:
    case kLayout64KbStandardSwizzle:
    case kLayoutLinear:
    case kLayoutSw64KbD:
    case kLayoutSw4KbS:
    case kLayoutSw64KbReserved:
        return true;
    default:
        return false;
    }
}

constexpr bool is_linear_layout(std::uint32_t layout, std::uint32_t dimension) {
    return dimension == kDimBuffer || dimension == kDimTexture1D ||
           layout == kLayoutRowMajor || layout == kLayoutLinear;
}

constexpr bool is_4kb_layout(std::uint32_t layout) {
    return layout == kLayoutSw4KbS;
}

constexpr std::uint32_t effective_tile_mode(const XgResourceDesc& desc) {
    if (is_linear_layout(desc.layout, desc.dimension)) return kLayoutLinear;
    if (desc.layout == kLayoutSw4KbS) return kLayoutSw4KbS;
    if (desc.layout == kLayoutSw64KbReserved) return kLayoutSw64KbReserved;
    return kLayoutSw64KbD;
}

constexpr bool validate_texture_desc(const XgResourceDesc& desc, FormatInfo& fmt, std::uint32_t& mips,
                                     std::uint32_t& height, std::uint32_t& depth_or_array) {
    if (desc.dimension != kDimTexture1D && desc.dimension != kDimTexture2D && desc.dimension != kDimTexture3D) {
        return false;
    }
    if (!is_known_layout(desc.layout)) return false;
    if (desc.width == 0 || desc.width > 65536u) return false;
    height = (desc.dimension == kDimTexture1D) ? 1u : desc.height;
    if (height == 0 || height > 65536u) return false;
    depth_or_array = desc.depth_or_array_size ? desc.depth_or_array_size : 1u;
    if (depth_or_array > 2048u) return false;
    const std::uint32_t samples = desc.sample_count ? desc.sample_count : 1u;
    if (samples != 1u && samples != 2u && samples != 4u && samples != 8u) return false;
    if (samples != 1u && desc.dimension != kDimTexture2D) return false;
    fmt = dxgi_format_info(desc.format);
    if (fmt.bytes_per_element == 0) return false;
    mips = resolve_mip_count(desc);
    if (mips == 0 || mips > kMaxMips) return false;
    if (samples != 1u && mips > 1u) return false;
    return true;
}

constexpr bool valid_tiling_bpe(std::uint32_t bpe) {
    return bpe == 1 || bpe == 2 || bpe == 4 || bpe == 8 || bpe == 16;
}

}  // namespace detail

// Suggests a console swizzle layout word (0x100 linear, 0x101 SW_64KB_D) for a texture description.
inline bool suggest_swizzle_mode(const XgResourceDesc& desc, std::uint32_t /*flags*/, std::uint32_t& out_mode) {
    FormatInfo fmt{};
    std::uint32_t mips = 0, height = 0, depth_or_array = 0;
    if (!detail::validate_texture_desc(desc, fmt, mips, height, depth_or_array)) return false;
    if (detail::is_linear_layout(desc.layout, desc.dimension) || !detail::valid_tiling_bpe(fmt.bytes_per_element)) {
        out_mode = kLayoutLinear;
        return true;
    }
    if (desc.layout == kLayoutSw4KbS || desc.layout == kLayoutSw64KbReserved) {
        out_mode = desc.layout;
        return true;
    }
    out_mode = kLayoutSw64KbD;
    return true;
}

// Computes XgResourceLayout for 1D, 2D, 2D-array, and 3D textures (linear or GFX10 4 KiB / 64 KiB tiled).
inline bool compute_resource_layout(const XgResourceDesc& desc, XgResourceLayout& out) {
    out = {};
    FormatInfo fmt{};
    std::uint32_t mips = 0, height = 0, depth_or_array = 0;
    if (!detail::validate_texture_desc(desc, fmt, mips, height, depth_or_array)) return false;

    const bool is_3d = (desc.dimension == kDimTexture3D);
    const std::uint32_t array_slices = is_3d ? 1u : depth_or_array;
    const std::uint32_t base_depth = is_3d ? depth_or_array : 1u;
    const std::uint32_t samples = desc.sample_count ? desc.sample_count : 1u;
    const bool use_linear = detail::is_linear_layout(desc.layout, desc.dimension) ||
                            !detail::valid_tiling_bpe(fmt.bytes_per_element);
    const bool use_4kb = !use_linear && detail::is_4kb_layout(desc.layout);
    const std::uint32_t tile_mode = use_linear ? kLayoutLinear : detail::effective_tile_mode(desc);

    XgResourceLayout result{};
    result.mip_levels = mips;
    result.planes = fmt.planes;

    std::uint64_t total_offset = 0;
    std::uint64_t max_alignment = use_linear ? kLinearPlacementAlignment : (use_4kb ? kSmallTileAlignment : kTileAlignment);

    for (std::uint32_t p = 0; p < fmt.planes; ++p) {
        const std::uint32_t bpe = (p == 0) ? fmt.bytes_per_element : fmt.plane1_bpe;
        const std::uint32_t block = (p == 0) ? fmt.block_dim : 1u;
        const std::uint64_t plane_align = use_linear ? kLinearPlacementAlignment
                                                     : (use_4kb ? kSmallTileAlignment : kTileAlignment);
        if (!detail::align_up(total_offset, plane_align, total_offset)) return false;

        XgPlaneLayout& plane = result.plane[p];
        plane.usage = fmt.depth ? (p == 0 ? 1u : 2u) : 0u;
        plane.base_offset_bytes = total_offset;
        plane.base_alignment_bytes = plane_align;
        plane.bytes_per_element = bpe;

        const TileShape tile_shape =
            use_linear ? TileShape{1, 1, 1}
                       : (use_4kb ? small_tile_shape(is_3d, bpe)
                                  : element_tile_shape(is_3d, bpe, samples == 4 ? 4u : 1u));
        if (!tile_shape.width || !tile_shape.height || !tile_shape.depth) return false;

        std::uint64_t slice_cursor = 0;
        for (std::uint32_t mip = 0; mip < mips; ++mip) {
            const std::uint64_t mip_w = std::max<std::uint64_t>(1u, desc.width >> mip);
            const std::uint32_t mip_h = std::max<std::uint32_t>(1u, height >> mip);
            const std::uint32_t mip_d = is_3d ? std::max<std::uint32_t>(1u, base_depth >> mip) : 1u;
            const std::uint64_t elem_w = detail::ceil_div(mip_w, block);
            const std::uint64_t elem_h = detail::ceil_div(mip_h, block);
            if (elem_w > 0xffffffffu || elem_h > 0xffffffffu) return false;

            XgMipLevelLayout& ml = plane.mip_layout[mip];
            ml.width_elements = static_cast<std::uint32_t>(elem_w);
            ml.height_elements = static_cast<std::uint32_t>(elem_h);
            ml.depth_or_array = is_3d ? mip_d : array_slices;
            ml.sample_count = samples;
            ml.tile_mode = tile_mode;

            if (use_linear) {
                std::uint64_t raw_row = 0, pitch = 0, slice2d = 0, mip_size = 0;
                if (!detail::checked_mul(elem_w, bpe, raw_row) ||
                    !detail::checked_mul(raw_row, samples, raw_row) ||
                    !detail::align_up(raw_row, kLinearPitchAlignment, pitch) ||
                    !detail::checked_mul(pitch, elem_h, slice2d) ||
                    !detail::checked_mul(slice2d, mip_d, mip_size) ||
                    !detail::align_up(slice_cursor, kLinearPlacementAlignment, slice_cursor)) {
                    return false;
                }
                if (pitch > 0xffffffffu) return false;
                ml.offset_bytes = slice_cursor;
                ml.size_bytes = mip_size;
                ml.slice_2d_size_bytes = slice2d;
                ml.pitch_bytes = static_cast<std::uint32_t>(pitch);
                ml.pitch_pixels = static_cast<std::uint32_t>((pitch / bpe) * block);
                ml.padded_width_elements = static_cast<std::uint32_t>(pitch / bpe);
                ml.padded_height_elements = static_cast<std::uint32_t>(elem_h);
                ml.padded_depth_or_array = ml.depth_or_array;
                if (!detail::checked_add(slice_cursor, mip_size, slice_cursor)) return false;
            } else {
                const std::uint64_t tiles_x = detail::ceil_div(elem_w, tile_shape.width);
                const std::uint64_t tiles_y = detail::ceil_div(elem_h, tile_shape.height);
                const std::uint64_t tiles_z = is_3d ? detail::ceil_div(mip_d, tile_shape.depth) : 1u;
                std::uint64_t pad_w = 0, pad_h = 0, pitch = 0, slice2d = 0, mip_size = 0;
                const std::uint64_t tile_bytes = use_4kb ? kSmallTileAlignment : kTileAlignment;
                if (!detail::checked_mul(tiles_x, tile_shape.width, pad_w) ||
                    !detail::checked_mul(tiles_y, tile_shape.height, pad_h) ||
                    !detail::checked_mul(pad_w, bpe, pitch) ||
                    !detail::checked_mul(tiles_x, tiles_y, slice2d) ||
                    !detail::checked_mul(slice2d, tile_bytes, slice2d) ||
                    !detail::checked_mul(slice2d, is_3d ? tiles_z : 1u, mip_size) ||
                    !detail::align_up(slice_cursor, tile_bytes, slice_cursor)) {
                    return false;
                }
                if (pad_w > 0xffffffffu || pad_h > 0xffffffffu || pitch > 0xffffffffu) return false;
                ml.offset_bytes = slice_cursor;
                ml.size_bytes = mip_size;
                ml.slice_2d_size_bytes = slice2d;
                ml.pitch_bytes = static_cast<std::uint32_t>(pitch);
                ml.pitch_pixels = static_cast<std::uint32_t>(pad_w * block);
                ml.padded_width_elements = static_cast<std::uint32_t>(pad_w);
                ml.padded_height_elements = static_cast<std::uint32_t>(pad_h);
                ml.padded_depth_or_array = is_3d ? static_cast<std::uint32_t>(tiles_z * tile_shape.depth) : array_slices;
                if (!detail::checked_add(slice_cursor, mip_size, slice_cursor)) return false;
            }
        }

        if (!detail::align_up(slice_cursor, plane_align, slice_cursor)) return false;
        plane.bytes_per_slice = slice_cursor;
        if (!detail::checked_mul(slice_cursor, array_slices, plane.size_bytes)) return false;
        if (!detail::checked_add(total_offset, plane.size_bytes, total_offset)) return false;
        max_alignment = std::max(max_alignment, plane_align);
    }

    if (!detail::align_up(total_offset, max_alignment, result.size_bytes)) return false;
    result.base_alignment_bytes = max_alignment;
    out = result;
    return true;
}

// Computes XGTextureAddressComputer::GetResourceTiling (slot 10) by delegating to leonida::resource_tiling().
inline bool compute_resource_tiling(const XgResourceDesc& desc,
                                    std::uint32_t* num_tiles_for_entire_resource,
                                    XgPackedMipDesc* packed_mip_desc,
                                    XgTileShape* standard_tile_shape,
                                    std::uint32_t* num_subresource_tilings,
                                    std::uint32_t first_subresource_tiling_to_get,
                                    XgSubresourceTiling* subresource_tilings) {
    FormatInfo fmt{};
    std::uint32_t mips = 0, height = 0, depth_or_array = 0;
    if (!detail::validate_texture_desc(desc, fmt, mips, height, depth_or_array)) return false;
    if (desc.dimension == kDimTexture1D) return false;

    TilingDesc td{};
    td.kind = (desc.dimension == kDimTexture3D) ? TiledKind::texture3d : TiledKind::texture2d;
    td.width = desc.width;
    td.height = height;
    td.depth_or_layers = depth_or_array;
    td.mips = mips;
    td.element_bytes = fmt.bytes_per_element;
    td.block = fmt.block_dim;
    td.samples = desc.sample_count ? desc.sample_count : 1u;

    ResourceTiling rt{};
    if (!resource_tiling(td, rt)) return false;

    if (num_tiles_for_entire_resource) *num_tiles_for_entire_resource = rt.total_tiles;
    if (packed_mip_desc) {
        packed_mip_desc->num_standard_mips = static_cast<std::uint8_t>(rt.standard_mips);
        packed_mip_desc->num_packed_mips = static_cast<std::uint8_t>(rt.packed_mips);
        packed_mip_desc->pad0 = 0;
        packed_mip_desc->num_tiles_for_packed_mips = rt.packed_tiles;
        packed_mip_desc->start_tile_index_in_overall_resource = rt.packed_start;
    }
    if (standard_tile_shape) {
        standard_tile_shape->width_in_texels = rt.shape.width;
        standard_tile_shape->height_in_texels = rt.shape.height;
        standard_tile_shape->depth_in_texels = rt.shape.depth;
    }
    if (num_subresource_tilings) {
        const std::uint32_t total = static_cast<std::uint32_t>(rt.subresources.size());
        const std::uint32_t available =
            (first_subresource_tiling_to_get < total) ? (total - first_subresource_tiling_to_get) : 0u;
        const std::uint32_t to_write = std::min(*num_subresource_tilings, available);
        if (subresource_tilings && to_write > 0) {
            for (std::uint32_t i = 0; i < to_write; ++i) {
                const SubresourceTiling& src = rt.subresources[first_subresource_tiling_to_get + i];
                XgSubresourceTiling& dst = subresource_tilings[i];
                dst.width_in_tiles = src.width;
                dst.height_in_tiles = static_cast<std::uint16_t>(src.height);
                dst.depth_in_tiles = static_cast<std::uint16_t>(src.depth);
                dst.start_tile_index_in_overall_resource = src.start;
            }
        }
        *num_subresource_tilings = to_write;
    }
    return true;
}

namespace detail {

inline bool transfer_subresource(const XgResourceDesc& desc, const XgResourceLayout& layout,
                                 std::byte* resource_base, std::uint32_t plane_index,
                                 std::uint32_t subresource, std::byte* linear_base,
                                 std::uint32_t linear_row_pitch, std::uint32_t linear_slice_pitch,
                                 bool into_resource) {
    if (!resource_base || !linear_base) return false;
    if (plane_index >= layout.planes || layout.mip_levels == 0 || layout.mip_levels > kMaxMips) return false;

    const bool is_3d = (desc.dimension == kDimTexture3D);
    const std::uint32_t array_slices = is_3d ? 1u : (desc.depth_or_array_size ? desc.depth_or_array_size : 1u);
    const std::uint32_t total_subresources = layout.mip_levels * array_slices;
    if (subresource >= total_subresources) return false;

    const std::uint32_t mip = subresource % layout.mip_levels;
    const std::uint32_t array_slice = subresource / layout.mip_levels;
    const XgPlaneLayout& plane = layout.plane[plane_index];
    const XgMipLevelLayout& ml = plane.mip_layout[mip];
    const std::uint32_t bpe = plane.bytes_per_element;
    if (bpe == 0 || ml.width_elements == 0 || ml.height_elements == 0) return false;

    const std::uint32_t z_slices = is_3d ? ml.depth_or_array : 1u;
    if (z_slices == 0) return false;
    const std::uint64_t row_bytes = std::uint64_t{ml.width_elements} * bpe;
    const std::uint64_t eff_row_pitch = (linear_row_pitch == 0 && ml.height_elements == 1) ? row_bytes : linear_row_pitch;
    if (eff_row_pitch < row_bytes) return false;

    const std::uint64_t tight_slice = (ml.height_elements > 1)
                                          ? eff_row_pitch * (ml.height_elements - 1u) + row_bytes
                                          : row_bytes;
    const std::uint64_t eff_slice_pitch = (linear_slice_pitch == 0 && z_slices == 1) ? tight_slice : linear_slice_pitch;
    if (z_slices > 1 && eff_slice_pitch < tight_slice) return false;

    const std::uint64_t res_offset =
        plane.base_offset_bytes + std::uint64_t{array_slice} * plane.bytes_per_slice + ml.offset_bytes;
    if (res_offset > layout.size_bytes || ml.size_bytes > layout.size_bytes - res_offset) return false;

    std::byte* res_mip = resource_base + res_offset;
    const bool linear = (ml.tile_mode == kLayoutLinear);

    if (linear) {
        if (ml.pitch_bytes < row_bytes) return false;
        std::uint64_t required_slice = 0;
        std::uint64_t last_slice_offset = 0;
        std::uint64_t total_needed = 0;
        if (!checked_mul(std::uint64_t{ml.height_elements}, ml.pitch_bytes, required_slice)) return false;
        if (z_slices > 1 && ml.slice_2d_size_bytes < required_slice) return false;
        if (!checked_mul(std::uint64_t{z_slices - 1u}, ml.slice_2d_size_bytes, last_slice_offset) ||
            !checked_add(last_slice_offset, required_slice, total_needed) ||
            total_needed > ml.size_bytes) {
            return false;
        }
        for (std::uint32_t z = 0; z < z_slices; ++z) {
            std::byte* res_slice = res_mip + std::uint64_t{z} * ml.slice_2d_size_bytes;
            std::byte* lin_slice = linear_base + std::uint64_t{z} * eff_slice_pitch;
            for (std::uint32_t y = 0; y < ml.height_elements; ++y) {
                std::byte* r_row = res_slice + std::uint64_t{y} * ml.pitch_bytes;
                std::byte* l_row = lin_slice + std::uint64_t{y} * eff_row_pitch;
                if (into_resource) std::memcpy(r_row, l_row, static_cast<std::size_t>(row_bytes));
                else std::memcpy(l_row, r_row, static_cast<std::size_t>(row_bytes));
            }
        }
        return true;
    }

    // Swizzled 3D volumes pack 3D tiles (depth > 1 per tile) rather than independent 2D slices;
    // TiledSurface2D cannot address 3D tiles and stepping z * slice_2d_size_bytes would overrun ml.size_bytes.
    if (is_3d) return false;

    // Swizzled 2D slices (GFX10 SW_64KB_D or SW_4KB_S) via TiledSurface2D.
    const gfx10::TileMode mode = (ml.tile_mode == kLayoutSw4KbS) ? gfx10::TileMode::sw_4kb_s : gfx10::TileMode::sw_64kb_d;
    gfx10::TiledSurface2D surf{};
    if (!gfx10::TiledSurface2D::make(mode, bpe, ml.width_elements, ml.height_elements, surf)) return false;
    if (surf.tiled_size() > ml.size_bytes || surf.tiled_size() > ml.slice_2d_size_bytes) return false;

    if (into_resource) {
        return surf.tile(std::span<const std::byte>(linear_base, static_cast<std::size_t>(tight_slice)),
                         std::span<std::byte>(res_mip, surf.tiled_size()),
                         static_cast<std::size_t>(eff_row_pitch));
    }
    return surf.detile(std::span<const std::byte>(res_mip, surf.tiled_size()),
                       std::span<std::byte>(linear_base, static_cast<std::size_t>(tight_slice)),
                       static_cast<std::size_t>(eff_row_pitch));
}

}  // namespace detail

// Slot 8: XGTextureAddressComputer::CopyIntoSubresource.
inline bool copy_into_subresource(const XgResourceDesc& desc, const XgResourceLayout& layout,
                                  void* destination, std::uint32_t plane, std::uint32_t subresource,
                                  const void* source, std::uint32_t source_row_pitch,
                                  std::uint32_t source_slice_pitch) {
    return detail::transfer_subresource(desc, layout, static_cast<std::byte*>(destination), plane, subresource,
                                        const_cast<std::byte*>(static_cast<const std::byte*>(source)),
                                        source_row_pitch, source_slice_pitch, true);
}

// Slot 9: XGTextureAddressComputer::CopyFromSubresource.
inline bool copy_from_subresource(const XgResourceDesc& desc, const XgResourceLayout& layout,
                                  void* destination, std::uint32_t plane, std::uint32_t subresource,
                                  const void* source, std::uint32_t destination_row_pitch,
                                  std::uint32_t destination_slice_pitch) {
    return detail::transfer_subresource(desc, layout,
                                        const_cast<std::byte*>(static_cast<const std::byte*>(source)),
                                        plane, subresource, static_cast<std::byte*>(destination),
                                        destination_row_pitch, destination_slice_pitch, false);
}

}  // namespace leonida::xg
