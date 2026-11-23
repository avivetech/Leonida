// Portable C++20 tests for runtime/src/xg_layout.h (no Windows or Vulkan dependency).
#include "../src/xg_layout.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

namespace {

using namespace leonida::xg;

XgResourceDesc make_2d(std::uint64_t w, std::uint32_t h, std::uint16_t array_size, std::uint16_t mips,
                       std::uint32_t format, std::uint32_t layout) {
    XgResourceDesc d{};
    d.dimension = kDimTexture2D;
    d.width = w;
    d.height = h;
    d.depth_or_array_size = array_size;
    d.mip_levels = mips;
    d.format = format;
    d.sample_count = 1;
    d.layout = layout;
    return d;
}

}  // namespace

int main() {
    // 1. XGSuggestSwizzleMode: 2D -> 0x101 (SW_64KB_D), 1D / linear -> 0x100, invalid -> false.
    {
        std::uint32_t mode = 0;
        const auto d2d = make_2d(512, 512, 1, 1, 28 /* R8G8B8A8_UNORM */, kLayoutUnknown);
        CHECK(suggest_swizzle_mode(d2d, 0, mode) && mode == kLayoutSw64KbD);

        auto dlin = d2d;
        dlin.layout = kLayoutLinear;
        CHECK(suggest_swizzle_mode(dlin, 0, mode) && mode == kLayoutLinear);

        auto d1d = d2d;
        d1d.dimension = kDimTexture1D;
        d1d.height = 1;
        CHECK(suggest_swizzle_mode(d1d, 0, mode) && mode == kLayoutLinear);

        auto d4k = d2d;
        d4k.layout = kLayoutSw4KbS;
        CHECK(suggest_swizzle_mode(d4k, 0, mode) && mode == kLayoutSw4KbS);

        auto bad = d2d;
        bad.format = 9999;
        CHECK(!suggest_swizzle_mode(bad, 0, mode));
    }

    // 2. Linear 2D layout with mips + array slices: verify pitch alignment (256), offset alignment (512),
    //    and non-overlapping CopyIntoSubresource / CopyFromSubresource round-trip.
    {
        const auto desc = make_2d(30, 10, 2, 3, 28 /* RGBA8 */, kLayoutLinear);
        XgResourceLayout layout{};
        CHECK(compute_resource_layout(desc, layout));
        CHECK(layout.planes == 1 && layout.mip_levels == 3);
        const auto& p0 = layout.plane[0];
        CHECK(p0.bytes_per_element == 4);
        CHECK(p0.mip_layout[0].pitch_bytes == 256);
        CHECK(p0.mip_layout[0].offset_bytes == 0);
        CHECK(p0.mip_layout[0].size_bytes == 256u * 10u);
        CHECK(p0.mip_layout[1].offset_bytes % kLinearPlacementAlignment == 0);
        CHECK(p0.mip_layout[1].offset_bytes >= p0.mip_layout[0].size_bytes);
        CHECK(p0.mip_layout[1].width_elements == 15 && p0.mip_layout[1].height_elements == 5);
        CHECK(p0.mip_layout[2].width_elements == 7 && p0.mip_layout[2].height_elements == 2);
        CHECK(p0.size_bytes == p0.bytes_per_slice * 2u);
        CHECK(layout.size_bytes >= p0.size_bytes);

        std::vector<std::byte> res(static_cast<std::size_t>(layout.size_bytes), std::byte{0});
        // Copy into slice 1, mip 1 (subresource = 1*3 + 1 = 4) with extra source row pitch padding.
        const std::uint32_t src_pitch = 15 * 4 + 12;
        std::vector<std::byte> src(src_pitch * 5, std::byte{0});
        for (std::uint32_t y = 0; y < 5; ++y) {
            for (std::uint32_t x = 0; x < 15 * 4; ++x) {
                src[y * src_pitch + x] = static_cast<std::byte>((y * 31u + x * 7u + 13u) & 0xFFu);
            }
        }
        CHECK(copy_into_subresource(desc, layout, res.data(), 0, 4, src.data(), src_pitch, 0));

        // Verify direct offset formula matches caller math: slice * bytes_per_slice + mip.offset_bytes.
        const std::size_t expected_off = static_cast<std::size_t>(p0.bytes_per_slice + p0.mip_layout[1].offset_bytes);
        for (std::uint32_t y = 0; y < 5; ++y) {
            CHECK(std::memcmp(res.data() + expected_off + y * p0.mip_layout[1].pitch_bytes,
                              src.data() + y * src_pitch, 15 * 4) == 0);
        }

        // Round-trip back via CopyFromSubresource.
        std::vector<std::byte> out(15 * 4 * 5, std::byte{0});
        CHECK(copy_from_subresource(desc, layout, out.data(), 0, 4, res.data(), 15 * 4, 0));
        for (std::uint32_t y = 0; y < 5; ++y) {
            CHECK(std::memcmp(out.data() + y * 15 * 4, src.data() + y * src_pitch, 15 * 4) == 0);
        }
    }

    // 3. Swizzled 64 KiB (0x101) BC7 2D texture with mips + array slices: verify exact layout and
    //    cross-check CopyIntoSubresource output against leonida::gfx10::TiledSurface2D::detile().
    {
        const auto desc = make_2d(512, 256, 2, 0 /* auto full chain */, 98 /* BC7_UNORM */, kLayoutSw64KbD);
        XgResourceLayout layout{};
        CHECK(compute_resource_layout(desc, layout));
        CHECK(layout.planes == 1 && layout.mip_levels == 10);
        const auto& p0 = layout.plane[0];
        CHECK(p0.bytes_per_element == 16);
        // BC7 512x256 -> 128x64 blocks; 64 KiB tile shape for 16B bpe is 64x64 blocks -> 2x1 tiles = 128 KiB.
        CHECK(p0.mip_layout[0].width_elements == 128 && p0.mip_layout[0].height_elements == 64);
        CHECK(p0.mip_layout[0].size_bytes == 2u * 65536u);
        CHECK(p0.mip_layout[0].offset_bytes == 0);
        CHECK(p0.mip_layout[1].offset_bytes == 2u * 65536u);
        CHECK(p0.mip_layout[1].width_elements == 64 && p0.mip_layout[1].height_elements == 32);
        CHECK(p0.mip_layout[1].size_bytes == 65536u);

        std::vector<std::byte> res(static_cast<std::size_t>(layout.size_bytes), std::byte{0});
        const std::uint32_t row_bytes = 128 * 16;
        std::vector<std::byte> src(row_bytes * 64);
        for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::byte>((i * 19u + 5u) & 0xFFu);

        // Copy into array slice 1, mip 0 (subresource = 10).
        CHECK(copy_into_subresource(desc, layout, res.data(), 0, 10, src.data(), row_bytes, 0));

        // Detile slice 1, mip 0 independently with TiledSurface2D and confirm byte-for-byte identity.
        leonida::gfx10::TiledSurface2D surf{};
        CHECK(leonida::gfx10::TiledSurface2D::make(leonida::gfx10::TileMode::sw_64kb_d, 16, 128, 64, surf));
        std::vector<std::byte> detiled(src.size(), std::byte{0});
        const std::size_t off = static_cast<std::size_t>(p0.bytes_per_slice + p0.mip_layout[0].offset_bytes);
        CHECK(surf.detile(std::span<const std::byte>(res.data() + off, surf.tiled_size()),
                          std::span<std::byte>(detiled.data(), detiled.size()), row_bytes));
        CHECK(detiled == src);

        // Also round-trip via copy_from_subresource.
        std::vector<std::byte> roundtrip(src.size(), std::byte{0});
        CHECK(copy_from_subresource(desc, layout, roundtrip.data(), 0, 10, res.data(), row_bytes, 0));
        CHECK(roundtrip == src);
    }

    // 4. Swizzled 4 KiB (0x102) R16G16B16A16_FLOAT round-trip.
    {
        const auto desc = make_2d(64, 32, 1, 2, 10 /* RGBA16F */, kLayoutSw4KbS);
        XgResourceLayout layout{};
        CHECK(compute_resource_layout(desc, layout));
        CHECK(layout.plane[0].mip_layout[0].tile_mode == kLayoutSw4KbS);
        CHECK(layout.base_alignment_bytes == kSmallTileAlignment);

        std::vector<std::byte> res(static_cast<std::size_t>(layout.size_bytes), std::byte{0});
        std::vector<std::byte> src(64 * 8 * 32);
        for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::byte>((i * 13u + 3u) & 0xFFu);
        CHECK(copy_into_subresource(desc, layout, res.data(), 0, 0, src.data(), 64 * 8, 0));
        std::vector<std::byte> dst(src.size(), std::byte{0});
        CHECK(copy_from_subresource(desc, layout, dst.data(), 0, 0, res.data(), 64 * 8, 0));
        CHECK(dst == src);
    }

    // 5. 3D volume texture (linear + swizzled) and depth-stencil multi-plane (D32_FLOAT_S8X24_UINT).
    {
        XgResourceDesc d3d{};
        d3d.dimension = kDimTexture3D;
        d3d.width = 16;
        d3d.height = 8;
        d3d.depth_or_array_size = 4;
        d3d.mip_levels = 2;
        d3d.format = 28;  // RGBA8
        d3d.sample_count = 1;
        d3d.layout = kLayoutLinear;
        XgResourceLayout layout{};
        CHECK(compute_resource_layout(d3d, layout));
        CHECK(layout.planes == 1 && layout.mip_levels == 2);
        CHECK(layout.plane[0].mip_layout[0].depth_or_array == 4);
        CHECK(layout.plane[0].mip_layout[1].depth_or_array == 2);

        std::vector<std::byte> res(static_cast<std::size_t>(layout.size_bytes), std::byte{0});
        const std::uint32_t row_pitch = 16 * 4;
        const std::uint32_t slice_pitch = row_pitch * 8;
        std::vector<std::byte> src(slice_pitch * 4);
        for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::byte>((i * 7u + 1u) & 0xFFu);
        CHECK(copy_into_subresource(d3d, layout, res.data(), 0, 0, src.data(), row_pitch, slice_pitch));
        std::vector<std::byte> out(src.size(), std::byte{0});
        CHECK(copy_from_subresource(d3d, layout, out.data(), 0, 0, res.data(), row_pitch, slice_pitch));
        CHECK(out == src);

        // Two-plane depth-stencil.
        const auto ds = make_2d(128, 128, 1, 1, 20 /* D32_FLOAT_S8X24_UINT */, kLayoutSw64KbD);
        XgResourceLayout ds_layout{};
        CHECK(compute_resource_layout(ds, ds_layout));
        CHECK(ds_layout.planes == 2);
        CHECK(ds_layout.plane[0].usage == 1 && ds_layout.plane[0].bytes_per_element == 4);
        CHECK(ds_layout.plane[1].usage == 2 && ds_layout.plane[1].bytes_per_element == 1);
        CHECK(ds_layout.plane[1].base_offset_bytes >= ds_layout.plane[0].size_bytes);
    }

    // 6. GetResourceTiling (slot 10): matches resource_tiling.h for 1024x1024 RGBA8 and 2048x1024x2 BC7,
    //    plus FirstSubresourceTilingToGet clamping and null optional outputs.
    {
        const auto desc = make_2d(1024, 1024, 1, 11, 28 /* RGBA8 */, kLayoutSw64KbReserved);
        std::uint32_t total_tiles = 0;
        XgPackedMipDesc packed{};
        XgTileShape shape{};
        std::uint32_t count = 16;
        XgSubresourceTiling tilings[16] = {};
        CHECK(compute_resource_tiling(desc, &total_tiles, &packed, &shape, &count, 0, tilings));
        CHECK(total_tiles == 86);
        CHECK(shape.width_in_texels == 128 && shape.height_in_texels == 128 && shape.depth_in_texels == 1);
        CHECK(packed.num_standard_mips == 4 && packed.num_packed_mips == 7);
        CHECK(packed.num_tiles_for_packed_mips == 1 && packed.start_tile_index_in_overall_resource == 85);
        CHECK(count == 11);
        CHECK(tilings[0].width_in_tiles == 8 && tilings[0].height_in_tiles == 8 && tilings[0].start_tile_index_in_overall_resource == 0);
        CHECK(tilings[3].width_in_tiles == 1 && tilings[3].height_in_tiles == 1 && tilings[3].start_tile_index_in_overall_resource == 84);
        CHECK(tilings[4].start_tile_index_in_overall_resource == leonida::packed_tile);

        // Partial slice query starting at subresource 2 with buffer of 2 entries.
        std::uint32_t partial = 2;
        XgSubresourceTiling sub[2] = {};
        CHECK(compute_resource_tiling(desc, nullptr, nullptr, nullptr, &partial, 2, sub));
        CHECK(partial == 2);
        CHECK(sub[0].width_in_tiles == 2 && sub[0].start_tile_index_in_overall_resource == 80);
        CHECK(sub[1].width_in_tiles == 1 && sub[1].start_tile_index_in_overall_resource == 84);

        // Out-of-bounds first index writes 0 entries and succeeds.
        std::uint32_t past = 4;
        CHECK(compute_resource_tiling(desc, nullptr, nullptr, nullptr, &past, 99, sub));
        CHECK(past == 0);
    }

    // 7. Error handling: invalid dimensions, unknown format, out-of-range plane/subresource, small pitch.
    {
        XgResourceLayout layout{};
        CHECK(!compute_resource_layout(make_2d(0, 64, 1, 1, 28, kLayoutLinear), layout));
        CHECK(!compute_resource_layout(make_2d(64, 0, 1, 1, 28, kLayoutLinear), layout));
        CHECK(!compute_resource_layout(make_2d(64, 64, 1, 1, 0, kLayoutLinear), layout));
        CHECK(!compute_resource_layout(make_2d(64, 64, 1, 16, 28, kLayoutLinear), layout));

        const auto ok = make_2d(16, 16, 1, 1, 28, kLayoutLinear);
        CHECK(compute_resource_layout(ok, layout));
        std::vector<std::byte> res(static_cast<std::size_t>(layout.size_bytes));
        std::vector<std::byte> src(16 * 16 * 4);
        CHECK(!copy_into_subresource(ok, layout, nullptr, 0, 0, src.data(), 64, 0));
        CHECK(!copy_into_subresource(ok, layout, res.data(), 1, 0, src.data(), 64, 0));
        CHECK(!copy_into_subresource(ok, layout, res.data(), 0, 1, src.data(), 64, 0));
        CHECK(!copy_into_subresource(ok, layout, res.data(), 0, 0, src.data(), 32, 0));  // pitch < 64
    }

    std::printf("xg_layout: swizzle suggestion, linear/4K/64K layouts, BC7/3D/DS, tiling and round-trips passed\n");
    return 0;
}
