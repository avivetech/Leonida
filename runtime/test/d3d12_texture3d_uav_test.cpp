// CreateUnorderedAccessView on 3D textures (null desc and D3D12_UAV_DIMENSION_TEXTURE3D) through the Vulkan-backed
// D3D12 device: verifies the GFX10 T# written into the heap slot, GPU ClearUnorderedAccessViewFloat/Uint using
// those views with full-volume readback, and rejection of out-of-range, partial W-slice, sRGB, and mismatched views.
//
// No shaders or game data. Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation; any validation
// error fails the test.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../src/gfx10_descriptors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

namespace g = leonida::gfx10;

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return b;
}

struct Harness {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fence_value = 0;
    std::byte* slots = nullptr;
    UINT step = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE slot(UINT i) const {
        return {reinterpret_cast<SIZE_T>(slots + std::size_t(i) * step)};
    }
    std::byte* bytes(UINT i) const { return slots + std::size_t(i) * step; }

    bool null_at(UINT i) const {
        const std::byte* p = bytes(i);
        for (UINT k = 0; k < step; ++k)
            if (p[k] != std::byte{0}) return false;
        return true;
    }

    bool image_at(UINT i, g::Image& out) const {
        g::ImageWords words{};
        std::memcpy(words.data(), bytes(i), sizeof(words));
        return g::is_image(words) && g::decode(words, out);
    }

    void poison(UINT i) const { std::memset(bytes(i), 0x77, step); }

    Ref<ID3D12Resource> committed(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE type,
                                  D3D12_RESOURCE_STATES state) const {
        Ref<ID3D12Resource> out;
        D3D12_HEAP_PROPERTIES heap{type};
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out.p));
        return out;
    }

    template<class Body>
    leonida::ReplayResult run(HRESULT& close, Body&& body) {
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                             IID_PPV_ARGS(&list.p))))
            return leonida::ReplayResult::unsupported;
        body(list.p);
        close = list->Close();
        if (FAILED(close)) return leonida::ReplayResult::success;
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        const auto result = rt::d3d12::last_execute_result(queue);
        queue->Signal(fence, ++fence_value);
        fence->SetEventOnCompletion(fence_value, nullptr);
        return result;
    }

    // Reads back one mip of a 3D texture (4 bytes per texel) into tightly-packed [z][y][x] order.
    bool readback_3d_mip(ID3D12Resource* tex, DXGI_FORMAT format, UINT width, UINT height, UINT depth, UINT mip,
                         std::vector<std::uint32_t>& out) {
        const UINT mw = std::max(1u, width >> mip);
        const UINT mh = std::max(1u, height >> mip);
        const UINT md = std::max(1u, depth >> mip);
        const UINT pitch = (mw * 4 + 255) / 256 * 256;
        const UINT64 bytes_total = UINT64(pitch) * mh * md;

        D3D12_RESOURCE_DESC rb_desc{};
        rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb_desc.Width = bytes_total;
        rb_desc.Height = 1;
        rb_desc.DepthOrArraySize = 1;
        rb_desc.MipLevels = 1;
        rb_desc.SampleDesc.Count = 1;
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ref<ID3D12Resource> rb = committed(rb_desc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!rb.p) return false;

        HRESULT close = E_FAIL;
        const auto res = run(close, [&](ID3D12GraphicsCommandList* list) {
            const auto to_src = transition(tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_src);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = rb.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = {0, {format, mw, mh, md, pitch}};
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = tex;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = mip;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            const auto back = transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &back);
        });
        if (close != S_OK || res != leonida::ReplayResult::success) return false;

        void* mapped = nullptr;
        if (FAILED(rb->Map(0, nullptr, &mapped)) || !mapped) return false;
        out.resize(std::size_t(mw) * mh * md);
        const auto* base = static_cast<const std::uint8_t*>(mapped);
        for (UINT z = 0; z < md; ++z)
            for (UINT y = 0; y < mh; ++y)
                std::memcpy(&out[(std::size_t(z) * mh + y) * mw],
                            base + (std::size_t(z) * mh + y) * pitch,
                            std::size_t(mw) * 4);
        rb->Unmap(0, nullptr);
        return true;
    }
};

D3D12_RESOURCE_DESC tex3d_desc(DXGI_FORMAT format, UINT width, UINT height, UINT16 depth, UINT16 mips,
                               D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = depth;
    d.MipLevels = mips;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    return d;
}

}  // namespace

int main() {
    vktest::Device d;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) {
        std::printf("SKIP: no Vulkan GPU\n");
        return 77;
    }
    if (status != vktest::DeviceStatus::ready) return 1;

    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;

        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p))));
        Ref<ID3D12CommandQueue> queue;
        const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        Ref<ID3D12DescriptorHeap> heap;
        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16};
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));

        Harness h{device.p, queue.p, fence.p, 0,
                  reinterpret_cast<std::byte*>(heap->GetCPUDescriptorHandleForHeapStart().ptr),
                  device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
        EXPECT(h.step >= sizeof(g::ImageWords));

        // 1. Typed RGBA8_UNORM 3D texture (4x4x4, 2 mips) with ALLOW_UNORDERED_ACCESS.
        Ref<ID3D12Resource> vol = h.committed(
            tex3d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 4, 2, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        EXPECT(vol.p);

        // 1a. Null desc creates a whole-mip-0 image_3d UAV.
        h.poison(0);
        device->CreateUnorderedAccessView(vol.p, nullptr, nullptr, h.slot(0));
        g::Image img{};
        EXPECT(h.image_at(0, img));
        EXPECT(img.type == g::ImageType::image_3d);
        EXPECT(img.width == 4 && img.height == 4 && img.depth == 4);
        EXPECT(img.first_mip == 0 && img.mip_count == 1 && img.resource_mips == 2);
        EXPECT(img.first_layer == 0 && img.layer_count == 1);

        // Clear mip 0 through the null-desc UAV and verify every texel in all 4 depth slices.
        HRESULT close = E_FAIL;
        const float rgba0[4] = {1.0f, 0.0f, 0.5f, 1.0f};  // {0xff, 0x00, 0x80, 0xff} -> 0xff8000ff
        EXPECT(h.run(close, [&](ID3D12GraphicsCommandList* list) {
            list->ClearUnorderedAccessViewFloat({0}, h.slot(0), vol.p, rgba0, 0, nullptr);
        }) == leonida::ReplayResult::success && close == S_OK);
        std::vector<std::uint32_t> pixels;
        EXPECT(h.readback_3d_mip(vol.p, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 4, 0, pixels));
        EXPECT(pixels.size() == 64);
        for (std::uint32_t p : pixels) EXPECT(p == 0xff8000ffu);

        // 1b. Explicit D3D12_UAV_DIMENSION_TEXTURE3D on mip 0 with WSize = UINT_MAX.
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav3d{};
        uav3d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav3d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uav3d.Texture3D = {0, 0, UINT_MAX};
        h.poison(1);
        device->CreateUnorderedAccessView(vol.p, nullptr, &uav3d, h.slot(1));
        EXPECT(h.image_at(1, img));
        EXPECT(img.type == g::ImageType::image_3d && img.first_mip == 0 && img.mip_count == 1 && img.depth == 4);

        const UINT uval0[4] = {0x11, 0x22, 0x33, 0x44};
        EXPECT(h.run(close, [&](ID3D12GraphicsCommandList* list) {
            list->ClearUnorderedAccessViewUint({0}, h.slot(1), vol.p, uval0, 0, nullptr);
        }) == leonida::ReplayResult::success && close == S_OK);
        EXPECT(h.readback_3d_mip(vol.p, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 4, 0, pixels));
        for (std::uint32_t p : pixels) EXPECT(p == 0x44332211u);

        // 1c. Explicit D3D12_UAV_DIMENSION_TEXTURE3D on mip 1 with exact mip-1 depth WSize = 2.
        uav3d.Texture3D = {1, 0, 2};
        h.poison(2);
        device->CreateUnorderedAccessView(vol.p, nullptr, &uav3d, h.slot(2));
        EXPECT(h.image_at(2, img));
        EXPECT(img.type == g::ImageType::image_3d && img.first_mip == 1 && img.mip_count == 1 && img.depth == 4);

        const UINT uval1[4] = {0xaa, 0xbb, 0xcc, 0xdd};
        EXPECT(h.run(close, [&](ID3D12GraphicsCommandList* list) {
            list->ClearUnorderedAccessViewUint({0}, h.slot(2), vol.p, uval1, 0, nullptr);
        }) == leonida::ReplayResult::success && close == S_OK);
        EXPECT(h.readback_3d_mip(vol.p, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 4, 1, pixels));
        EXPECT(pixels.size() == 8);
        for (std::uint32_t p : pixels) EXPECT(p == 0xddccbbaau);
        // Mip 0 must be untouched by the mip-1 clear.
        EXPECT(h.readback_3d_mip(vol.p, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 4, 0, pixels));
        for (std::uint32_t p : pixels) EXPECT(p == 0x44332211u);

        // 2. R32_UINT 3D texture (uint storage binding class).
        Ref<ID3D12Resource> vol_u32 = h.committed(
            tex3d_desc(DXGI_FORMAT_R32_UINT, 4, 2, 2, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        EXPECT(vol_u32.p);
        D3D12_UNORDERED_ACCESS_VIEW_DESC u32_uav{};
        u32_uav.Format = DXGI_FORMAT_R32_UINT;
        u32_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        u32_uav.Texture3D = {0, 0, 2};
        h.poison(3);
        device->CreateUnorderedAccessView(vol_u32.p, nullptr, &u32_uav, h.slot(3));
        EXPECT(h.image_at(3, img) && img.type == g::ImageType::image_3d);
        const UINT u32_val[4] = {0x12345678u, 0, 0, 0};
        EXPECT(h.run(close, [&](ID3D12GraphicsCommandList* list) {
            list->ClearUnorderedAccessViewUint({0}, h.slot(3), vol_u32.p, u32_val, 0, nullptr);
        }) == leonida::ReplayResult::success && close == S_OK);
        EXPECT(h.readback_3d_mip(vol_u32.p, DXGI_FORMAT_R32_UINT, 4, 2, 2, 0, pixels));
        EXPECT(pixels.size() == 16);
        for (std::uint32_t p : pixels) EXPECT(p == 0x12345678u);

        // 3. Typeless R8G8B8A8_TYPELESS 3D texture: UNORM succeeds; null desc and SRGB are rejected.
        Ref<ID3D12Resource> typeless_vol = h.committed(
            tex3d_desc(DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, 4, 2, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        EXPECT(typeless_vol.p);
        h.poison(4);
        device->CreateUnorderedAccessView(typeless_vol.p, nullptr, nullptr, h.slot(4));
        EXPECT(h.null_at(4));
        D3D12_UNORDERED_ACCESS_VIEW_DESC typeless_uav{};
        typeless_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        typeless_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        typeless_uav.Texture3D = {0, 0, UINT_MAX};
        h.poison(4);
        device->CreateUnorderedAccessView(typeless_vol.p, nullptr, &typeless_uav, h.slot(4));
        EXPECT(h.null_at(4));
        typeless_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        h.poison(4);
        device->CreateUnorderedAccessView(typeless_vol.p, nullptr, &typeless_uav, h.slot(4));
        EXPECT(h.image_at(4, img) && img.type == g::ImageType::image_3d);
        const UINT tval[4] = {0x55, 0x66, 0x77, 0x88};
        EXPECT(h.run(close, [&](ID3D12GraphicsCommandList* list) {
            list->ClearUnorderedAccessViewUint({0}, h.slot(4), typeless_vol.p, tval, 0, nullptr);
        }) == leonida::ReplayResult::success && close == S_OK);
        EXPECT(h.readback_3d_mip(typeless_vol.p, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 2, 0, pixels));
        EXPECT(pixels.size() == 32);
        for (std::uint32_t p : pixels) EXPECT(p == 0x88776655u);

        // 4. Rejections: bad mip, zero WSize, out-of-range W slices, partial W-slice ranges, and dimension mismatch.
        const D3D12_TEX3D_UAV bad_ranges[] = {
            {2, 0, UINT_MAX},  // MipSlice past MipLevels (2)
            {0, 0, 0},         // WSize == 0
            {0, 4, 1},         // FirstWSlice >= depth (4)
            {0, 0, 5},         // WSize > depth (4)
            {1, 0, 3},         // WSize > mip-1 depth (2)
            {0, 1, 2},         // partial W-slice subrange
            {0, 0, 2},         // partial W-slice subrange from 0
            {0, 1, UINT_MAX},  // partial W-slice tail
        };
        for (const auto& range : bad_ranges) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC bad{};
            bad.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            bad.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            bad.Texture3D = range;
            h.poison(5);
            device->CreateUnorderedAccessView(vol.p, nullptr, &bad, h.slot(5));
            EXPECT(h.null_at(5));
        }

        // Dimension mismatch: TEXTURE3D UAV on a 2D texture must be rejected.
        D3D12_RESOURCE_DESC tex2d_desc = tex3d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        tex2d_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Ref<ID3D12Resource> tex2d = h.committed(tex2d_desc, D3D12_HEAP_TYPE_DEFAULT,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        EXPECT(tex2d.p);
        uav3d.Texture3D = {0, 0, UINT_MAX};
        h.poison(5);
        device->CreateUnorderedAccessView(tex2d.p, nullptr, &uav3d, h.slot(5));
        EXPECT(h.null_at(5));

        // Resource without ALLOW_UNORDERED_ACCESS must be rejected.
        Ref<ID3D12Resource> readonly_vol = h.committed(
            tex3d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 2, 1, D3D12_RESOURCE_FLAG_NONE),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        EXPECT(readonly_vol.p);
        h.poison(5);
        device->CreateUnorderedAccessView(readonly_vol.p, nullptr, &uav3d, h.slot(5));
        EXPECT(h.null_at(5));
    }

    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("CreateUnorderedAccessView TEXTURE3D: null desc, explicit mips, R32_UINT, typeless, and rejections "
                "all passed\n");
    return 0;
}
