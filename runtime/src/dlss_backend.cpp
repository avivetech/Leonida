// DLSS backend: the NVIDIA NGX path behind the DLSS provider face the runtime exposes. The FFX-facing
// description (the game's ffx::DispatchDescUpscale or the plain FfxDescription) is translated to NGX — the two
// are ML upscalers with isomorphic inputs: color, depth, motion vectors, jitter, exposure; the DLSS side is
// documented in the DLSS Programming Guide 310.6 sections 5.2-5.4 — and run through the driver's NGX Core,
// which is loaded by name and never shipped with Leonida: nvngx.dll from the NVIDIA driver distribution,
// nvngx_dlss.dll next to the exe.
//
// Backend contract (src-level):
//   init(device)         once, before any create; also queries capability (SuperSampling.Available)
//   create(...)          once per (in, out) size pair on quality/flag change; releases the old handle
//   eval_from_ffx(...)   per dispatch: fills the parameter bag from the FFX description and records
//   shutdown(device)     releases the feature, parameters and the NGX instance
// The caller owns state preservation: NGX rewrites the command list state (guide 5.4 IMPORTANT), so the shim
// wraps the game's list and restores the game's state after the records.
#include "dlss_backend.h"

#include <windows.h>

#include <d3d12.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../test/nvngx_api_min.h"

namespace leonida::dlss {

namespace {

struct Core {
    nvngx::PfnInitWithProjectId init_project_id = nullptr;
    nvngx::PfnAllocateParameters allocate = nullptr;
    nvngx::PfnGetCapabilityParameters capability = nullptr;
    nvngx::PfnCreateFeature create_feature = nullptr;
    nvngx::PfnEvaluateFeature evaluate = nullptr;
    nvngx::PfnReleaseFeature release_feature = nullptr;
    nvngx::PfnDestroyParameters destroy = nullptr;
    nvngx::PfnShutdown1 shutdown1 = nullptr;
    bool enabled = false;
};

Core& core() {
    static Core c;
    return c;
}

bool g_initiated = false;

void* find_export(HMODULE module, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}

// Loads the NGX Core and binds exports. Env LEONIDA_NGX_DLL names a full nvngx.dll path; without it, the
// loader search finds nvngx.dll the way NGX itself does (application folder, then the driver distribution).
bool load_core() {
    if (core().enabled) return true;
    HMODULE module = nullptr;
    if (const char* path = std::getenv("LEONIDA_NGX_DLL"); path && *path) {
        module = LoadLibraryA(path);
    } else {
        module = GetModuleHandleA("nvngx.dll");  // already in the process (the game loads it), else below
        if (!module) module = LoadLibraryA("nvngx.dll");
    }
    if (!module) {
        std::printf("dlss: no NGX Core (set LEONIDA_NGX_DLL=<path to nvngx.dll>, or install the NVIDIA driver)\n");
        return false;
    }
    Core& c = core();
    c.init_project_id = reinterpret_cast<nvngx::PfnInitWithProjectId>(find_export(module, "NVSDK_NGX_D3D12_Init_ProjectID"));
    c.allocate = reinterpret_cast<nvngx::PfnAllocateParameters>(find_export(module, "NVSDK_NGX_D3D12_AllocateParameters"));
    c.capability = reinterpret_cast<nvngx::PfnGetCapabilityParameters>(find_export(module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    c.create_feature = reinterpret_cast<nvngx::PfnCreateFeature>(find_export(module, "NVSDK_NGX_D3D12_CreateFeature"));
    c.evaluate = reinterpret_cast<nvngx::PfnEvaluateFeature>(find_export(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    c.release_feature = reinterpret_cast<nvngx::PfnReleaseFeature>(find_export(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
    c.destroy = reinterpret_cast<nvngx::PfnDestroyParameters>(find_export(module, "NVSDK_NGX_D3D12_DestroyParameters"));
    c.shutdown1 = reinterpret_cast<nvngx::PfnShutdown1>(find_export(module, "NVSDK_NGX_D3D12_Shutdown1"));
    if (!(c.init_project_id && c.allocate && c.capability && c.create_feature && c.evaluate && c.release_feature &&
          c.destroy && c.shutdown1)) {
        std::printf("dlss: NGX Core exports incomplete\n");
        return false;
    }
    c.enabled = true;
    return true;
}

}  // namespace

bool init(ID3D12Device* device, const wchar_t* data_path) {
    if (!load_core()) return false;
    if (g_initiated) return true;
    // The NGX Core dereferences the info record unconditionally: a null here ends the process (observed),
    // a zeroed one is what the SDK's own init path passes.
    nvngx::FeatureCommonInfo info{};
    const nvngx::Result rc = core().init_project_id(
        "7c2f18e5-3bda-41e6-9f0d-2a4b8c6e5d17", nvngx::ENGINE_CUSTOM, "leonida 0.1", data_path, device, &info,
        nvngx::SDK_VERSION);
    if (rc != nvngx::RESULT_SUCCESS) {
        std::printf("dlss: init_project_id -> 0x%x\n", static_cast<unsigned>(rc));
        return false;
    }
    // capability: SuperSampling.Available + driver gate, as guide 5.2.6 prescribes
    nvngx::Parameter* caps = nullptr;
    if (core().capability(&caps) == nvngx::RESULT_SUCCESS && caps) {
        int available = 0, needs_newer = 0;
        caps->Get(nvngx::SUPER_AVAILABLE, &available);
        caps->Get(nvngx::SUPER_NEEDS_UPDATED_DRIVER, &needs_newer);
        core().destroy(caps);
        g_initiated = available != 0 && needs_newer == 0;
        return g_initiated;
    }
    return false;
}

bool initiated() {
    return g_initiated;
}

void shutdown(ID3D12Device* device) {
    if (core().enabled) core().shutdown1(device);
}

nvngx::PerfQualityValue pick_quality(float upscale_ratio) {
    // The FFX mode's ratio names the DLSS slot (the game's own ffxQuery feeds desc.upscale_ratio). The slot
    // ratios anchored by the guide's Optimal Settings table are MaxQuality=1.5, UltraQuality=1.7,
    // Balanced=1.7237, MaxPerf=3.0: distances fold 2.0-3.0 into UltraQuality and the 1.7237 collision keeps
    // Balanced in the 1.71..2.0 band.
    if (upscale_ratio >= 3.0f) return nvngx::PERF_VALUE_MAX_PERF;
    if (upscale_ratio >= 2.0f) return nvngx::PERF_VALUE_ULTRA_QUALITY;
    if (upscale_ratio > 1.71f) return nvngx::PERF_VALUE_BALANCED;    // 1.7237x
    if (upscale_ratio > 1.6f) return nvngx::PERF_VALUE_ULTRA_QUALITY;  // 1.7x
    if (upscale_ratio > 1.05f) return nvngx::PERF_VALUE_MAX_QUALITY;   // 1.5x
    return nvngx::PERF_VALUE_DLAA;
}

int create_flags(const Description& desc) {
    return (desc.hdr ? nvngx::DLSS_FLAG_IS_HDR : 0) | (desc.mv_low_res ? nvngx::DLSS_FLAG_MV_LOW_RES : 0) |
           (desc.mv_jittered ? nvngx::DLSS_FLAG_MV_JITTERED : 0) |
           (desc.depth_inverted ? nvngx::DLSS_FLAG_DEPTH_INVERTED : 0) |
           (desc.auto_exposure ? nvngx::DLSS_FLAG_AUTO_EXPOSURE : 0);
}

float effective_mv_scale(float mv_scale) {
    return mv_scale == 0.0f ? 1.0f : mv_scale;  // the shipped helper's 0-means-1.0 rule
}

const char* preset_key(nvngx::PerfQualityValue quality) {
    switch (quality) {
        case nvngx::PERF_VALUE_DLAA: return nvngx::PRESET_KEY_DLAA;
        case nvngx::PERF_VALUE_ULTRA_QUALITY: return nvngx::PRESET_KEY_ULTRA_QUALITY;
        case nvngx::PERF_VALUE_MAX_QUALITY: return nvngx::PRESET_KEY_QUALITY;
        case nvngx::PERF_VALUE_BALANCED: return nvngx::PRESET_KEY_BALANCED;
        case nvngx::PERF_VALUE_MAX_PERF: return nvngx::PRESET_KEY_PERFORMANCE;
        case nvngx::PERF_VALUE_ULTRA_PERFORMANCE: return nvngx::PRESET_KEY_ULTRA_PERFORMANCE;
    }
    return nvngx::PRESET_KEY_QUALITY;
}

// The create parameter bag, in the shipped helper's exact key and type order (NGX_D3D12_CREATE_DLSS_EXT):
// node masks as unsigned, sizes as unsigned, quality and flags as int. The preset hint rides the same bag
// so it is on it before CreateFeature — the guide's 3.12 example Sets it on the create parameters, and the
// slot pick is read at creation when the network preset is baked into the feature. PRESET_DEFAULT (0)
// leaves the key unset, which keeps the driver's OTA default in place.
void fill_create_params(nvngx::Parameter* params, const Description& desc, unsigned int creation_node_mask,
                        nvngx::PerfQualityValue quality, int flags) {
    params->Set(nvngx::CREATION_NODE_MASK, creation_node_mask);
    params->Set(nvngx::VISIBILITY_NODE_MASK, creation_node_mask);
    params->Set(nvngx::WIDTH, desc.render_width);
    params->Set(nvngx::HEIGHT, desc.render_height);
    params->Set(nvngx::OUT_WIDTH, desc.upscale_width);
    params->Set(nvngx::OUT_HEIGHT, desc.upscale_height);
    params->Set(nvngx::PERF_QUALITY_VALUE, static_cast<int>(quality));
    params->Set(nvngx::DLSS_CREATE_FLAGS, flags);
    params->Set(nvngx::DLSS_ENABLE_OUTPUT_SUBRECTS, 0);
    if (desc.preset != nvngx::PRESET_DEFAULT)
        params->Set(preset_key(quality), desc.preset);
}

// The eval parameter bag, in the shipped helper's exact key and type order (NGX_D3D12_EVALUATE_DLSS_EXT):
// resources, jitter floats, the deprecated sharpness (set 0), reset as int, the MV scale with the helper's
// 0-means-1.0 rule, then the optional resources. The preset hint is refreshed identically, so a bag reused
// across frames keeps describing the selected slot whatever the NGX side reads at which call.
void fill_eval_params(nvngx::Parameter* params, const FfxDescription& fx, int preset,
                      nvngx::PerfQualityValue quality) {
    params->Set(nvngx::COLOR, fx.color);
    params->Set(nvngx::OUTPUT, fx.output);
    if (fx.depth) params->Set(nvngx::DEPTH, fx.depth);
    if (fx.motion_vectors) params->Set(nvngx::MOTION_VECTORS, fx.motion_vectors);
    params->Set(nvngx::JITTER_OFFSET_X, fx.jitter_x);
    params->Set(nvngx::JITTER_OFFSET_Y, fx.jitter_y);
    params->Set(nvngx::SHARPNESS, fx.sharpness);
    params->Set(nvngx::RESET, fx.reset ? 1 : 0);
    params->Set(nvngx::MV_SCALE_X, effective_mv_scale(fx.mv_scale_x));
    params->Set(nvngx::MV_SCALE_Y, effective_mv_scale(fx.mv_scale_y));
    if (preset != nvngx::PRESET_DEFAULT)
        params->Set(preset_key(quality), preset);
    if (fx.exposure) params->Set(nvngx::EXPOSURE_TEXTURE, fx.exposure);
    if (fx.transparency_mask) params->Set(nvngx::TRANSPARENCY_MASK, fx.transparency_mask);
}

FfxDescription ffx_to_native(ID3D12GraphicsCommandList* list, const ffx::DispatchDescUpscale& fx) {
    (void)list;
    FfxDescription out{};
    out.color = static_cast<ID3D12Resource*>(fx.color.resource);
    out.output = static_cast<ID3D12Resource*>(fx.output.resource);
    out.depth = static_cast<ID3D12Resource*>(fx.depth.resource);
    out.motion_vectors = static_cast<ID3D12Resource*>(fx.motionVectors.resource);
    out.exposure = static_cast<ID3D12Resource*>(fx.exposure.resource);
    out.transparency_mask = static_cast<ID3D12Resource*>(fx.transparencyAndComposition.resource);
    // FFX's jitterOffset is the negative of what NGX wants (the shipped helper negates the game's half-pixel
    // jitter; the game passes the NGX-convention values already, so they pass through unchanged).
    out.jitter_x = fx.jitterOffset.x;
    out.jitter_y = fx.jitterOffset.y;
    // FFX's motionVectorScale is applied identically by NGX; only the 0-means-1.0 shipped rule rides on top.
    out.mv_scale_x = fx.motionVectorScale.x;
    out.mv_scale_y = fx.motionVectorScale.y;
    out.sharpness = fx.enableSharpening ? fx.sharpness : 0.0f;
    out.reset = fx.reset;
    return out;
}

Feature create(ID3D12GraphicsCommandList* list, const Description& desc, ID3D12Device* device,
               unsigned int creation_node_mask) {
    (void)device;
    if (!core().enabled) return Feature{};
    nvngx::Parameter* params = nullptr;
    if (core().allocate(&params) != nvngx::RESULT_SUCCESS || !params) return Feature{};
    const nvngx::PerfQualityValue quality = pick_quality(desc.upscale_ratio);
    const int flags = create_flags(desc);
    fill_create_params(params, desc, creation_node_mask, quality, flags);
    Feature f;
    f.params = params;
    nvngx::Handle* handle = nullptr;
    f.rc_create = core().create_feature(list, nvngx::FEATURE_SUPERSAMPLING, params, &handle);
    f.ngx = handle;
    f.quality = quality;
    f.flags = flags;
    f.preset = desc.preset;
    return f;
}

bool eval_from_ffx(Feature& f, ID3D12GraphicsCommandList* list, const FfxDescription& fx) {
    if (!f.ngx || !f.params) return false;
    fill_eval_params(f.params, fx, f.preset, f.quality);
    return core().evaluate(list, f.ngx, f.params, nullptr) == nvngx::RESULT_SUCCESS;
}

void destroy(Feature& f) {
    if (f.ngx && core().enabled) core().release_feature(f.ngx);
    f.ngx = nullptr;
    if (f.params && core().enabled) core().destroy(f.params);
    f.params = nullptr;
}

}  // namespace leonida::dlss
