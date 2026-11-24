// DLSS backend: the NVIDIA NGX path that the FFX-facing DLSS provider exposes. This is what a DLSS-enabled
// runtime builds on when the user's rig has the NGX Core (nvngx.dll from the driver, nvngx_dlss.dll next to the
// exe): the pure translation surface (pick_quality, create_flags, preset_key, the create/eval parameter-bag
// assembly) is exposed on its own so the unit tests can drive it without NGX, and the process-level bits
// (Core load, init, capability query, feature create/evaluate/release) wrap the driver's NGX D3D12 exports.
// The NGX declarations live in runtime/test/nvngx_api_min.h next to ffx_api_min.h; nothing of NVIDIA's SDK is
// vendored (their license forbids it) — only written-out declarations from the ABI and the public 310.6 guide.
#pragma once

#include <d3d12.h>

#include <cstdint>

#include "../test/nvngx_api_min.h"
#include "../test/ffx_api_min.h"  // declaration subset; ffx_to_native bridges its DispatchDescUpscale

namespace leonida::dlss {

// What the FFX create asks for, in DLSS terms. The flags are the FFX upscale context flags (the game passes
// HDR + low-res MVs + inverted depth + auto exposure on 1.0.2.1); `preset` is a DLSS.Hint.Render.Preset value
// (nvngx::PresetValue); the default 0 leaves the driver's OTA choice in place.
struct Description {
    unsigned int render_width = 0;
    unsigned int render_height = 0;
    unsigned int upscale_width = 0;
    unsigned int upscale_height = 0;
    float upscale_ratio = 1.0f;  // from the game's own ffxQuery(GETUPSCALERATIOFROMQUALITYMODE)
    bool hdr = false;
    bool mv_low_res = true;
    bool mv_jittered = false;
    bool depth_inverted = false;
    bool auto_exposure = false;
    int preset = nvngx::PRESET_DEFAULT;
};

// One dispatch's inputs. The plain ID3D12Resource* form is the native one the parameter bag wants; the FFX
// form (the game's ffx::DispatchDescUpscale carries ffx::Resource wrappers) is handled by the bridge below.
struct FfxDescription {
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion_vectors = nullptr;
    ID3D12Resource* exposure = nullptr;
    ID3D12Resource* transparency_mask = nullptr;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float mv_scale_x = 0.0f;  // 0 means 1.0 at eval, per the shipped helper
    float mv_scale_y = 0.0f;
    float sharpness = 0.0f;   // deprecated upstream; stays 0
    bool reset = false;       // scene transitions
};

struct Feature {
    nvngx::Handle* ngx = nullptr;
    nvngx::Parameter* params = nullptr;
    nvngx::Result rc_create = 0;
    nvngx::PerfQualityValue quality = nvngx::PERF_VALUE_MAX_QUALITY;
    int flags = 0;
    int preset = nvngx::PRESET_DEFAULT;  // per-slot hint set at eval; 0 stays the OTA default
};

// The translation pieces, exposed on their own because the tests drive them without NGX (they are the
// game-measured mapping rules, not just implementation details):
//   pick_quality — the FFX mode's ratio names the DLSS slot; the shipped-enum collisions resolve
//                  conservatively (3.0x keeps MaxPerf, 1.7237x keeps Balanced; 2.0-3.0 folds into
//                  UltraQuality, and the 1.71 cut is where the shipped values stop differing).
//   create_flags — the bitmap the measured 1.0.2.1 FSR4 context translates to.
//   effective_mv_scale — the shipped helper's 0-means-1.0 rule.
nvngx::PerfQualityValue pick_quality(float upscale_ratio);
int create_flags(const Description& desc);
float effective_mv_scale(float mv_scale);

// The per-slot preset key for a quality mode (DLSS.Hint.Render.Preset.*), and whether the feature's preset
// hint should be set at all: the driver's OTA default stays untouched for PRESET_DEFAULT.
const char* preset_key(nvngx::PerfQualityValue quality);

// The parameter-bag assembly, also driven by the tests (a mock nvngx::Parameter records the calls): the
// guide's create sequence (5.3, via the shipped NGX_D3D12_CREATE_DLSS_EXT helper) and the eval sequence
// (5.4, via NGX_D3D12_EVALUATE_DLSS_EXT). The preset hint goes into the bag before CreateFeature (guide
// 3.12: presets are selected per scaling-ratio slot on the create parameters) and is refreshed per eval.
void fill_create_params(nvngx::Parameter* params, const Description& desc, unsigned int creation_node_mask,
                        nvngx::PerfQualityValue quality, int flags);
void fill_eval_params(nvngx::Parameter* params, const FfxDescription& fx, int preset,
                      nvngx::PerfQualityValue quality);

// The FFX-face bridge: pulls the game's ffx::DispatchDescUpscale into the native FfxDescription so the shim
// can feed one dispatch straight from the FFX front's upscale desc. Null ffx resources stay null on the NGX
// side; sharpness rides ffx's own enableSharpening flag (0 when unset, per the deprecated-key story).
FfxDescription ffx_to_native(ID3D12GraphicsCommandList* list, const ffx::DispatchDescUpscale& fx);

// Process-level: loads the NGX Core by name (LEONIDA_NGX_DLL overrides), inits on device, checks capability.
bool init(ID3D12Device* device, const wchar_t* data_path);
bool initiated();
void shutdown(ID3D12Device* device);

// One DLSS feature per (description); destroying the previous one is the caller's business — NGX caches it
// internally, and the guide's release-then-create on state change keeps that cache warm.
Feature create(ID3D12GraphicsCommandList* list, const Description& desc, ID3D12Device* device,
               unsigned int creation_node_mask);

bool eval_from_ffx(Feature& f, ID3D12GraphicsCommandList* list, const FfxDescription& fx);

void destroy(Feature& f);

}  // namespace leonida::dlss
