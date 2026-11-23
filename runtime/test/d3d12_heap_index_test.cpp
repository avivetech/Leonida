// Direct heap indexing in compute (docs/d3d12_vulkan.md sections 3.4 and 3.5): a root signature with
// CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED makes the list write the bound CBV/SRV/UAV heap's base, the heap SetDescriptorHeaps
// bound, into the user SGPR right after the root arguments at each Dispatch and dispatch ExecuteIndirect. The shader is
// translator output (test/shaders/cs_heap_index.asm, hand-written RDNA2 in the shape of the game's heap-indexing
// shaders): it loads the V# at heap slot k (k a root constant, a run-time offset) and at heap slot 1 (a static offset)
// from that base and writes through both.
//
// One list, four dispatches, each of one 64-lane group:
//   1. Dispatch, heap A bound, k = 2: A[2] is buffer a2, A[1] buffer a1
//   2. Dispatch, heap B bound, k = 2: B[2] is buffer b2, B[1] buffer b1
//   3. Dispatch, heap A bound, k = 3: A[3] is buffer a3, A[1] buffer a1 again
//   4. ExecuteIndirect of one DISPATCH (1, 1, 1), heap B bound, k = 3: B[3] is buffer b3, B[1] buffer b1 again
// Every buffer starts as a sentinel and is compared word by word with the CPU reference. Negative control: dispatch 2
// differs from dispatch 1 only in the heap SetDescriptorHeaps bound, so its writes land in heap B's buffers only if
// the base the shader reads follows the binding; a base left at 0 or at the first heap would leave b2 and b1 at the
// sentinel (or write a2 and a1 again, which the reference checks too). The layouts of signatures without the flag,
// with the sampler flag and with both are checked through root_signature_layout.
// Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation (0 errors required).
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../src/shader_library.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#ifndef LEONIDA_TEST_HEAP_INDEX_DIR
#error "LEONIDA_TEST_HEAP_INDEX_DIR must name the folder build.py wrote (spirv/index.json, fssm/cs_heap_index.fssm)"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

constexpr UINT lanes = 64;
constexpr UINT64 bytes = lanes * 4;
constexpr std::uint32_t sentinel = 0x5e5e5e5eu;

enum Buf { a1, a2, a3, b1, b2, b3, buffer_count };

// A root signature of one 32-bit constant with `flags`. The host serializer is given no flags (Windows 10's refuses
// the heap-indexing ones) and the flags word of the RTS0 1.0 header, its sixth dword, is written afterwards.
std::vector<std::uint8_t> root_signature(UINT flags) {
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {0, 0, 1};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc.Desc_1_0 = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    std::vector<std::uint8_t> blob = serialize_root_signature(desc);
    if (blob.size() < 32 || std::memcmp(blob.data(), "DXBC", 4) != 0) return {};
    std::uint32_t parts = 0;
    std::memcpy(&parts, blob.data() + 28, 4);
    for (std::uint32_t i = 0; i < parts && 32 + 4 * (i + 1) <= blob.size(); ++i) {
        std::uint32_t at = 0;
        std::memcpy(&at, blob.data() + 32 + 4 * i, 4);
        if (std::uint64_t(at) + 8 + 24 > blob.size() || std::memcmp(blob.data() + at, "RTS0", 4) != 0) continue;
        std::memcpy(blob.data() + at + 8 + 20, &flags, 4);
        return blob;
    }
    return {};
}

int run(vktest::Device& d, const std::filesystem::path& root_dir, unsigned& words_checked) {
    leonida::ShaderLibrary library;
    EXPECT(library.load((root_dir / "spirv").string()));
    const auto fssm = read_file(root_dir / "fssm" / "cs_heap_index.fssm");
    EXPECT(fssm.size() >= 0x150 && library.find("cs", leonida::sha256_hex(fssm)));
    const auto container = shader_container(fssm);

    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.buffer_device_address = true;  // the V# holds the buffer's address
    info.bindless = d.bindless;
    info.bindless_partially_bound = d.partially_bound;
    info.image_view_min_lod = d.image_view_min_lod;
    info.shader_dir = (root_dir / "spirv").string();
    Ref<ID3D12Device> device;
    EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p))));

    // The layouts: the heap bases follow the one root constant (s4), CBV/SRV/UAV first; no flag, no heap SGPR.
    constexpr UINT res = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    constexpr UINT smp = D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    const struct {
        UINT flags;
        std::uint32_t resource_sgpr, sampler_sgpr;
    } layouts[] = {{0, rt::rootsig::kNoSgpr, rt::rootsig::kNoSgpr},
                   {res, 5, rt::rootsig::kNoSgpr},
                   {smp, rt::rootsig::kNoSgpr, 5},
                   {res | smp, 5, 6}};
    Ref<ID3D12RootSignature> root;
    for (const auto& want : layouts) {
        const auto blob = root_signature(want.flags);
        EXPECT(!blob.empty());
        Ref<ID3D12RootSignature> signature;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&signature.p))));
        const rt::d3d12::RootArgumentLayout* layout = rt::d3d12::root_signature_layout(signature.p);
        EXPECT(layout && layout->compute_ok && layout->flags == want.flags);
        EXPECT(layout->parameters.size() == 1 && layout->parameters[0].compute_offset == 4);
        EXPECT(layout->compute_resource_heap_sgpr == want.resource_sgpr);
        EXPECT(layout->compute_sampler_heap_sgpr == want.sampler_sgpr);
        if (want.flags == res) std::swap(root.p, signature.p);
    }
    EXPECT(root.p);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.p;
    pso_desc.CS = {container.data(), container.size()};
    Ref<ID3D12PipelineState> pso;
    EXPECT(SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, upload_heap{D3D12_HEAP_TYPE_UPLOAD},
        readback_heap{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> upload, readback, buf[buffer_count];
    const auto create = [&](Ref<ID3D12Resource>& out, const D3D12_HEAP_PROPERTIES& heap, UINT64 size,
                            D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        const auto desc = buffer_desc(size, flags);
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(&out.p)));
    };
    EXPECT(create(upload, upload_heap, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ));
    EXPECT(create(readback, readback_heap, bytes * buffer_count, D3D12_RESOURCE_FLAG_NONE,
                  D3D12_RESOURCE_STATE_COPY_DEST));
    for (auto& b : buf)
        EXPECT(create(b, default_heap, bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_DEST));
    {
        std::uint8_t* mapped = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
        for (UINT64 i = 0; i < bytes / 4; ++i) std::memcpy(mapped + i * 4, &sentinel, 4);
        upload->Unmap(0, nullptr);
    }

    // Two shader-visible heaps of four slots; slot 0 stays empty in both.
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap_a, heap_b;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap_a.p))));
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap_b.p))));
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    EXPECT(step == 32);  // the shader steps 32 bytes a slot
    for (ID3D12DescriptorHeap* h : {heap_a.p, heap_b.p}) {
        const UINT64 gpu = h->GetGPUDescriptorHandleForHeapStart().ptr;
        EXPECT(gpu >= 0x10000 && gpu < (1ull << 32));  // a 32-bit pointer reaches it
    }
    EXPECT(heap_a->GetGPUDescriptorHandleForHeapStart().ptr != heap_b->GetGPUDescriptorHandleForHeapStart().ptr);
    const auto raw_view = [&](ID3D12DescriptorHeap* h, UINT index, Buf which) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer = {0, UINT(bytes / 4), 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
        D3D12_CPU_DESCRIPTOR_HANDLE at = h->GetCPUDescriptorHandleForHeapStart();
        at.ptr += SIZE_T(index) * step;
        device->CreateUnorderedAccessView(buf[which].p, nullptr, &view, at);
    };
    raw_view(heap_a.p, 1, a1);
    raw_view(heap_a.p, 2, a2);
    raw_view(heap_a.p, 3, a3);
    raw_view(heap_b.p, 1, b1);
    raw_view(heap_b.p, 2, b2);
    raw_view(heap_b.p, 3, b3);

    // The indirect arguments: one D3D12_DISPATCH_ARGUMENTS of (1, 1, 1), read from an upload buffer.
    const D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
    const D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument, 0};
    Ref<ID3D12CommandSignature> command_signature;
    EXPECT(SUCCEEDED(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&command_signature.p))));
    Ref<ID3D12Resource> arguments;
    EXPECT(create(arguments, upload_heap, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ));
    {
        D3D12_DISPATCH_ARGUMENTS* mapped = nullptr;
        EXPECT(SUCCEEDED(arguments->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
        *mapped = {1, 1, 1};
        arguments->Unmap(0, nullptr);
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

    constexpr auto uav_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_BARRIER barriers[buffer_count];
    for (auto& b : buf) list->CopyBufferRegion(b.p, 0, upload.p, 0, bytes);
    for (int i = 0; i < buffer_count; ++i) barriers[i] = transition(buf[i].p, D3D12_RESOURCE_STATE_COPY_DEST, uav_state);
    list->ResourceBarrier(buffer_count, barriers);
    list->SetComputeRootSignature(root.p);
    list->SetPipelineState(pso.p);
    const auto dispatch = [&](ID3D12DescriptorHeap* h, UINT k, bool indirect) {
        list->SetDescriptorHeaps(1, &h);
        list->SetComputeRoot32BitConstant(0, k, 0);
        if (indirect)
            list->ExecuteIndirect(command_signature.p, 1, arguments.p, 0, nullptr, 0);
        else
            list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER all{};
        all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // a1 and b1 are written twice
        list->ResourceBarrier(1, &all);
    };
    dispatch(heap_a.p, 2, false);
    dispatch(heap_b.p, 2, false);
    dispatch(heap_a.p, 3, false);
    dispatch(heap_b.p, 3, true);
    for (int i = 0; i < buffer_count; ++i) barriers[i] = transition(buf[i].p, uav_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(buffer_count, barriers);
    for (int i = 0; i < buffer_count; ++i) list->CopyBufferRegion(readback.p, bytes * i, buf[i].p, 0, bytes);
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(event != nullptr);
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, event)));
    if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "the fence never reached 1\n");
        return 1;
    }
    CloseHandle(event);

    std::vector<std::uint32_t> got(bytes / 4 * buffer_count);
    {
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        std::memcpy(got.data(), mapped, got.size() * 4);
        readback->Unmap(0, nullptr);
    }
    // a1 and b1: the static slot; a2, b2: k = 2 through heaps A and B; a3, b3: k = 3 through heap A (Dispatch) and
    // heap B (ExecuteIndirect).
    const auto want = [](Buf which, UINT g) -> std::uint32_t {
        switch (which) {
            case a1:
            case b1:
                return 0xa5000000u + g;
            case a2:
            case b2:
                return 0x2000u + g;
            case a3:
            case b3:
                return 0x3000u + g;
            default:
                return sentinel;
        }
    };
    static const char* const names[buffer_count] = {"A[1]", "A[2]", "A[3]", "B[1]", "B[2]", "B[3]"};
    unsigned bad = 0;
    for (int i = 0; i < buffer_count; ++i)
        for (UINT g = 0; g < lanes; ++g) {
            const std::uint32_t have = got[i * lanes + g], expected = want(Buf(i), g);
            ++words_checked;
            if (have != expected && bad++ < 8)
                std::fprintf(stderr, "%s word %u: 0x%08x, expected 0x%08x\n", names[i], g, have, expected);
        }
    EXPECT(bad == 0);
    return 0;
}

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;   // no Vulkan GPU: ctest skips
    if (device_status != vktest::DeviceStatus::ready) return 1;
    unsigned words = 0;
    if (const int status = run(d, LEONIDA_TEST_HEAP_INDEX_DIR, words)) return status;
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("d3d12_heap_index: %u words match through heap-indexed V#s (two heaps, run-time and static slots, "
                "Dispatch and ExecuteIndirect)\n",
                words);
    return 0;
}
