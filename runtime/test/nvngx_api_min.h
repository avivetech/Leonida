// The subset of the NVIDIA NGX interface that dlss_backend_unit_test.cpp and src/dlss_backend.cpp call, written out
// from the ABI as shipped:
//   - the export names, from a PE export-table dump of the driver's NGX Core (nvngx.dll, 310.x line, D3D12 section
//     read here: NVSDK_NGX_D3D12_{Init_ProjectID, Init_Ext, Init, AllocateParameters, GetCapabilityParameters,
//     GetParameters, CreateFeature, EvaluateFeature, ReleaseFeature, DestroyParameters, GetScratchBufferSize,
//     Shutdown, Shutdown1});
//   - the 17-virtual-slot parameter object, as the SDK ships it (single-inheritance MSVC vtable; the order below
//     exercised end to end by a probe that runs Init/Capability/Create/Evaluate on real hardware);
//   - the semantics of the create/evaluate parameters, documented in the public DLSS Programming Guide (310.6,
//     sections 5.2 to 5.4, key strings and flag bits included).
// Nothing from the NVIDIA DLSS SDK is copied here: those files carry the NVIDIA RTX SDK license, which forbids
// making them subject to an open-source license; nvngx_dlss.dll itself is a user-side component (the driver's
// NGX Core loads it, and the game-side copy lives next to the executable per the guide's distribution section).
#pragma once

#include <cstdint>

typedef struct ID3D11Resource ID3D11Resource;
typedef struct ID3D12Resource ID3D12Resource;

namespace nvngx {

using Result = int;
using Version = int;
using EngineType = int;
using Feature = int;  // NVSDK_NGX_Feature_SuperSampling = 1

constexpr Result RESULT_SUCCESS = 0x1;
constexpr Result RESULT_FAIL = static_cast<int>(0xBAD00000u);
constexpr Result RESULT_FAIL_FEATURE_NOT_SUPPORTED = RESULT_FAIL | 1;
constexpr Result RESULT_FAIL_PLATFORM_ERROR = RESULT_FAIL | 2;
constexpr Result RESULT_FAIL_FEATURE_ALREADY_EXISTS = RESULT_FAIL | 3;
constexpr Result RESULT_FAIL_FEATURE_NOT_FOUND = RESULT_FAIL | 4;
constexpr Result RESULT_FAIL_INVALID_PARAMETER = RESULT_FAIL | 5;
constexpr Result RESULT_FAIL_NOT_INITIALIZED = RESULT_FAIL | 7;
constexpr Result RESULT_FAIL_OUT_OF_DATE = RESULT_FAIL | 12;  // an older driver: the deprecated GetParameters fallback

constexpr Version SDK_VERSION = 0x15;  // NVSDK_NGX_Version_API of the 310.6 release (NVNGX version 1.5.0)
constexpr EngineType ENGINE_CUSTOM = 0;
constexpr Feature FEATURE_SUPERSAMPLING = 1;

// DLSS create flags, as the guide documents them (section 5.3) and the runtime checks them.
constexpr int DLSS_FLAG_IS_HDR = 1 << 0;
constexpr int DLSS_FLAG_MV_LOW_RES = 1 << 1;
constexpr int DLSS_FLAG_MV_JITTERED = 1 << 2;
constexpr int DLSS_FLAG_DEPTH_INVERTED = 1 << 3;
constexpr int DLSS_FLAG_AUTO_EXPOSURE = 1 << 6;

// PerfQualityValue, the shipped enum order (guide section 5.2.8.1): MaxPerf=MaxPerformance, MaxQuality=Quality.
enum PerfQualityValue {
    PERF_VALUE_MAX_PERF = 0,         // 3.0x
    PERF_VALUE_BALANCED = 1,         // 1.7237x
    PERF_VALUE_MAX_QUALITY = 2,      // 1.5x
    PERF_VALUE_ULTRA_PERFORMANCE = 3,  // 3.0x, extended slot
    PERF_VALUE_ULTRA_QUALITY = 4,    // 1.7x, extended slot
    PERF_VALUE_DLAA = 5,             // 1.0x
};

// DLSS.Hint.Render.Preset values (guide 5.2.8.2, the 310.6 enum). Default keeps whatever the OTA brings; the
// live presets are J/K/L/M (K is the transformer one recommended for DLAA/Balanced/Quality, L is Ultra Perf's,
// M is Perf's); the rest are deprecated or reserved.
enum PresetValue {
    PRESET_DEFAULT = 0,
    PRESET_E = 5,
    PRESET_F = 6,
    PRESET_J = 10,
    PRESET_K = 11,
    PRESET_L = 12,
    PRESET_M = 13,
};

// The opaque object Allocate/Get(Capability)Parameters hands out: 17 virtual slots, single-inheritance vtable,
// declared in this exact order (verified at runtime; changing the order changes the ABI).
struct Parameter {
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;
    virtual Result Get(const char* name, unsigned long long* out) const = 0;
    virtual Result Get(const char* name, float* out) const = 0;
    virtual Result Get(const char* name, double* out) const = 0;
    virtual Result Get(const char* name, unsigned int* out) const = 0;
    virtual Result Get(const char* name, int* out) const = 0;
    virtual Result Get(const char* name, ID3D11Resource** out) const = 0;
    virtual Result Get(const char* name, ID3D12Resource** out) const = 0;
    virtual Result Get(const char* name, void** out) const = 0;
    virtual void Reset() = 0;
};

// Where the NGX Core looks for feature dlls beyond the application folder (guide section 5.1), plus the
// logging block the Core reads whenever the caller declares an SDK version that knows it (0x14+; 0x15 is
// passed). The layout matches the shipped struct exactly: 16-byte path list (pointer + length + pad),
// the opaque internal slot, and the inline 16-byte logging record. A shorter record ends the process at
// init with 0xc0000005 (the Core dereferences/reads past the shorter object — observed), which is why the
// logging record is inlined bytes, not a pointer the zeroing would hide behind.
struct PathListInfo {
    const wchar_t* const* path;
    unsigned int length;
};
typedef void(__cdecl* AppLogCallback)(const char* message, int logging_level, Feature source);
struct LoggingInfo {
    AppLogCallback callback;      // app-side sink; null keeps the Core's defaults
    int minimum_level;            // 0 off / 1 on / 2 verbose
    bool disable_other_sinks;     // true requires a non-null callback
};
struct FeatureCommonInfo {
    PathListInfo pathList;
    void* internal;          // engine-managed
    LoggingInfo logging;
};

// The Core reads these records by the shipped layout; a mismatch is a silent ABI break, so pin the sizes.
static_assert(sizeof(PathListInfo) == 16, "PathListInfo must match the shipped 16-byte record");
static_assert(sizeof(LoggingInfo) == 16, "LoggingInfo must match the shipped 16-byte record");
static_assert(sizeof(FeatureCommonInfo) == 40, "FeatureCommonInfo must total the shipped 40-byte record");

typedef struct Handle Handle;  // opaque, from CreateFeature, consumed by Evaluate/Release

// Function signatures exactly as the NGX Core exports them (the short Init name; the SDK's longer
// NVSDK_NGX_D3D12_Init_with_ProjectID is a wrapper around it).
using PfnInitWithProjectId = Result (*)(const char* project_id, EngineType engine, const char* engine_version,
                                        const wchar_t* data_path, ID3D12Device* device,
                                        const FeatureCommonInfo* info, Version version);
using PfnAllocateParameters = Result (*)(Parameter**);
using PfnGetCapabilityParameters = Result (*)(Parameter**);
using PfnCreateFeature = Result (*)(ID3D12GraphicsCommandList*, Feature, Parameter*, Handle**);
using PfnEvaluateFeature = Result (*)(ID3D12GraphicsCommandList*, const Handle*, Parameter*, void* callback);
using PfnReleaseFeature = Result (*)(Handle*);
using PfnDestroyParameters = Result (*)(Parameter*);
using PfnShutdown1 = Result (*)(void* device);

// The parameter keys this integration reads or writes (string values documented in the guide 5.2-5.5 and
// exercised by the runtime).
constexpr const char* CREATION_NODE_MASK = "CreationNodeMask";
constexpr const char* VISIBILITY_NODE_MASK = "VisibilityNodeMask";
constexpr const char* WIDTH = "Width";
constexpr const char* HEIGHT = "Height";
constexpr const char* OUT_WIDTH = "OutWidth";
constexpr const char* OUT_HEIGHT = "OutHeight";
constexpr const char* PERF_QUALITY_VALUE = "PerfQualityValue";
constexpr const char* DLSS_CREATE_FLAGS = "DLSS.Feature.Create.Flags";
constexpr const char* DLSS_ENABLE_OUTPUT_SUBRECTS = "DLSS.Enable.Output.Subrects";
constexpr const char* COLOR = "Color";
constexpr const char* OUTPUT = "Output";
constexpr const char* DEPTH = "Depth";
constexpr const char* MOTION_VECTORS = "MotionVectors";
constexpr const char* JITTER_OFFSET_X = "Jitter.Offset.X";
constexpr const char* JITTER_OFFSET_Y = "Jitter.Offset.Y";
constexpr const char* MV_SCALE_X = "MV.Scale.X";
constexpr const char* MV_SCALE_Y = "MV.Scale.Y";
constexpr const char* RESET = "Reset";
constexpr const char* SHARPNESS = "Sharpness";
constexpr const char* EXPOSURE_TEXTURE = "ExposureTexture";
constexpr const char* TRANSPARENCY_MASK = "TransparencyMask";
constexpr const char* SUPER_AVAILABLE = "SuperSampling.Available";
constexpr const char* SUPER_NEEDS_UPDATED_DRIVER = "SuperSampling.NeedsUpdatedDriver";
constexpr const char* SUPER_MIN_DRIVER_VERSION_MAJOR = "SuperSampling.MinDriverVersionMajor";
constexpr const char* SUPER_MIN_DRIVER_VERSION_MINOR = "SuperSampling.MinDriverVersionMinor";
constexpr const char* SUPER_FEATURE_INIT_RESULT = "SuperSampling.FeatureInitResult";
// Per-slot render preset hints (310.6): the int value is a member of PresetValue; setting the slot that
// matches the feature's quality mode overrides which network preset runs.
constexpr const char* PRESET_KEY_DLAA = "DLSS.Hint.Render.Preset.DLAA";
constexpr const char* PRESET_KEY_ULTRA_QUALITY = "DLSS.Hint.Render.Preset.UltraQuality";
constexpr const char* PRESET_KEY_QUALITY = "DLSS.Hint.Render.Preset.Quality";
constexpr const char* PRESET_KEY_BALANCED = "DLSS.Hint.Render.Preset.Balanced";
constexpr const char* PRESET_KEY_PERFORMANCE = "DLSS.Hint.Render.Preset.Performance";
constexpr const char* PRESET_KEY_ULTRA_PERFORMANCE = "DLSS.Hint.Render.Preset.UltraPerformance";

}  // namespace nvngx
