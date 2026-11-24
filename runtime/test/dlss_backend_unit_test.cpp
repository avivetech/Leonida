// CPU-only unit test for the DLSS backend's translation rules: it drives the pure functions
// (pick_quality / create_flags / effective_mv_scale / preset_key) and, through a recording mock of the
// nvngx::Parameter ABI, the create/eval parameter-bag assembly (fill_create_params / fill_eval_params) —
// no D3D12 device and no NGX, so it runs everywhere and guards the game-measured mapping and the guide's
// 310.6 key order that the .cpp must not drift from.
#include <cstdio>
#include <string>
#include <vector>

#include "../src/dlss_backend.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// A recording Parameter: every Set lands in a list of "name=value" words; Get answers are irrelevant here.
struct MockParameter final : nvngx::Parameter {
    std::vector<std::string> puts;

    std::string dump() const {
        std::string out;
        for (const std::string& w : puts) out += w + " ";
        return out;
    }

    void Set(const char* name, unsigned long long) override { puts.push_back(std::string(name) + "=ull"); }
    void Set(const char* name, float) override { puts.push_back(std::string(name) + "=f"); }
    void Set(const char* name, double) override { puts.push_back(std::string(name) + "=d"); }
    void Set(const char* name, unsigned int) override { puts.push_back(std::string(name) + "=ui"); }
    void Set(const char* name, int) override { puts.push_back(std::string(name) + "=i"); }
    void Set(const char* name, ID3D11Resource*) override { puts.push_back(std::string(name) + "=r11"); }
    void Set(const char* name, ID3D12Resource*) override { puts.push_back(std::string(name) + "=r12"); }
    void Set(const char* name, void*) override { puts.push_back(std::string(name) + "=p"); }
    nvngx::Result Get(const char*, unsigned long long*) const override { return nvngx::RESULT_FAIL; }
    nvngx::Result Get(const char*, float*) const override { return nvngx::RESULT_FAIL; }
    nvngx::Result Get(const char*, double*) const override { return nvngx::RESULT_FAIL; }
    nvngx::Result Get(const char*, unsigned int*) const override { return nvngx::RESULT_FAIL; }
    nvngx::Result Get(const char*, int*) const override { return nvngx::RESULT_FAIL; }
    nvngx::Result Get(const char*, ID3D11Resource**) const override { return nvngx::RESULT_FAIL; }
    nvngx::Result Get(const char*, ID3D12Resource**) const override { return nvngx::RESULT_FAIL; }
    nvngx::Result Get(const char*, void**) const override { return nvngx::RESULT_FAIL; }
    void Reset() override {}
};

void contains(const MockParameter& m, const char* word, const char* what) {
    check(m.dump().find(word) != std::string::npos, what);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using leonida::dlss::pick_quality;
    using leonida::dlss::create_flags;
    using leonida::dlss::effective_mv_scale;
    using leonida::dlss::preset_key;
    using nvngx::PerfQualityValue;

    // pick_quality: the measured FFX slots of 1.0.2.1 — 1280x720 -> 1920x1080 is 1.5x.
    check(pick_quality(1.5f) == nvngx::PERF_VALUE_MAX_QUALITY, "1.5x -> MaxQuality (the game's FSR Quality slot)");
    check(pick_quality(2.0f) == nvngx::PERF_VALUE_ULTRA_QUALITY, "2.0x -> UltraQuality");
    check(pick_quality(1.7f) == nvngx::PERF_VALUE_ULTRA_QUALITY, "1.7x -> UltraQuality");
    // The shipped-enum collisions: 1.7237x is Balanced's own ratio and UltraQuality does not win it.
    check(pick_quality(1.7237f) == nvngx::PERF_VALUE_BALANCED, "1.7237x -> Balanced (shipped collision)");
    check(pick_quality(1.72f) == nvngx::PERF_VALUE_BALANCED, "1.72x -> Balanced");
    check(pick_quality(1.705f) == nvngx::PERF_VALUE_ULTRA_QUALITY, "1.705x -> UltraQuality (under the 1.71 cut)");
    check(pick_quality(3.0f) == nvngx::PERF_VALUE_MAX_PERF, "3.0x -> MaxPerf (shipped collision with UltraPerf)");
    check(pick_quality(2.99f) == nvngx::PERF_VALUE_ULTRA_QUALITY, "2.99x -> UltraQuality");
    check(pick_quality(4.0f) == nvngx::PERF_VALUE_MAX_PERF, "4.0x -> MaxPerf (UltraPerformance lives above 3.0)");
    check(pick_quality(1.06f) == nvngx::PERF_VALUE_MAX_QUALITY, "1.06x -> MaxQuality band");
    check(pick_quality(1.05f) == nvngx::PERF_VALUE_DLAA, "1.05x -> DLAA (boundary)");
    check(pick_quality(1.0f) == nvngx::PERF_VALUE_DLAA, "1.0x -> DLAA");

    // create_flags: the measured 1.0.2.1 FSR4 context is HDR + low-res MVs + inverted depth + auto exposure
    // (the game renders motion vectors at render resolution, so MV_LOW_RES rides the default).
    leonida::dlss::Description game{};
    game.upscale_ratio = 1.5f;
    game.hdr = true;
    game.depth_inverted = true;
    game.auto_exposure = true;
    const int expect_game = nvngx::DLSS_FLAG_IS_HDR | nvngx::DLSS_FLAG_MV_LOW_RES |
                            nvngx::DLSS_FLAG_DEPTH_INVERTED | nvngx::DLSS_FLAG_AUTO_EXPOSURE;
    check(create_flags(game) == expect_game, "game-measured context -> HDR|MVLowRes|DepthInverted|AutoExposure");
    leonida::dlss::Description all{};
    all.hdr = all.mv_low_res = all.mv_jittered = all.depth_inverted = all.auto_exposure = true;
    const int expect_all = nvngx::DLSS_FLAG_IS_HDR | nvngx::DLSS_FLAG_MV_LOW_RES | nvngx::DLSS_FLAG_MV_JITTERED |
                           nvngx::DLSS_FLAG_DEPTH_INVERTED | nvngx::DLSS_FLAG_AUTO_EXPOSURE;
    check(create_flags(all) == expect_all, "all set -> all bits");
    leonida::dlss::Description none{};
    check(none.mv_low_res == true, "mv_low_res defaults to true (the game renders MVs at render resolution)");
    check(create_flags(none) == nvngx::DLSS_FLAG_MV_LOW_RES, "defaults -> MV_LOW_RES only");

    // effective_mv_scale: the shipped helper's 0-means-1.0 rule.
    check(effective_mv_scale(0.0f) == 1.0f, "mv scale 0 -> 1.0");
    check(effective_mv_scale(0.5f) == 0.5f, "mv scale passes through");
    check(effective_mv_scale(-0.0f) == 1.0f, "negative zero -> 1.0 as well");

    // preset_key: one key per quality slot.
    check(preset_key(nvngx::PERF_VALUE_MAX_QUALITY) == nvngx::PRESET_KEY_QUALITY, "MaxQuality -> Quality preset key");
    check(preset_key(nvngx::PERF_VALUE_BALANCED) == nvngx::PRESET_KEY_BALANCED, "Balanced -> Balanced key");
    check(preset_key(nvngx::PERF_VALUE_MAX_PERF) == nvngx::PRESET_KEY_PERFORMANCE, "MaxPerf -> Performance key");
    check(preset_key(nvngx::PERF_VALUE_ULTRA_PERFORMANCE) == nvngx::PRESET_KEY_ULTRA_PERFORMANCE,
          "UltraPerformance -> UltraPerformance key");
    check(preset_key(nvngx::PERF_VALUE_ULTRA_QUALITY) == nvngx::PRESET_KEY_ULTRA_QUALITY,
          "UltraQuality -> UltraQuality key");
    check(preset_key(nvngx::PERF_VALUE_DLAA) == nvngx::PRESET_KEY_DLAA, "DLAA -> DLAA key");

    // PresetValue enum: the shipped J/K/L/M numbering must not drift (K=11 transformer, L=12, M=13).
    check(nvngx::PRESET_J == 10 && nvngx::PRESET_K == 11 && nvngx::PRESET_L == 12 && nvngx::PRESET_M == 13,
          "preset enum keeps shipped numbering");

    // Result codes: the guide's AllocateParameters fallback story (5.3) lives on FAIL_OutOfDate = |12.
    check(nvngx::RESULT_SUCCESS == 1, "RESULT_SUCCESS = 1 (shipped enum)");
    check(static_cast<unsigned>(nvngx::RESULT_FAIL) == 0xBAD00000u, "FAIL base = 0xBAD00000");
    check(static_cast<unsigned>(nvngx::RESULT_FAIL_OUT_OF_DATE) == 0xBAD0000Cu, "FAIL_OutOfDate = base|12");
    check(static_cast<unsigned>(nvngx::RESULT_FAIL_NOT_INITIALIZED) == 0xBAD00007u, "FAIL_NotInitialized = base|7");

    // create bag: the shipped helper's exact key and type order (NGX_D3D12_CREATE_DLSS_EXT), and the preset
    // hint rides the bag before CreateFeature whenever a preset is named.
    {
        MockParameter m;
        leonida::dlss::Description d{};
        d.render_width = 1280;
        d.render_height = 720;
        d.upscale_width = 1920;
        d.upscale_height = 1080;
        d.upscale_ratio = 1.5f;  // the game's Quality slot
        d.hdr = true;
        d.preset = nvngx::PRESET_K;
        leonida::dlss::fill_create_params(&m, d, 1, leonida::dlss::pick_quality(d.upscale_ratio),
                                          leonida::dlss::create_flags(d));
        const char* expect =
            "CreationNodeMask=ui VisibilityNodeMask=ui Width=ui Height=ui OutWidth=ui OutHeight=ui "
            "PerfQualityValue=i DLSS.Feature.Create.Flags=i DLSS.Enable.Output.Subrects=i "
            "DLSS.Hint.Render.Preset.Quality=i ";
        check(m.dump() == expect, "create bag: helper key/type order + preset hint on the create bag");
    }
    {
        MockParameter m;
        leonida::dlss::Description d{};
        d.render_width = 1280;
        d.render_height = 720;
        d.upscale_width = 1920;
        d.upscale_height = 1080;
        d.upscale_ratio = 1.5f;
        leonida::dlss::fill_create_params(&m, d, 1, leonida::dlss::pick_quality(d.upscale_ratio),
                                          leonida::dlss::create_flags(d));
        check(m.dump().find("Preset") == std::string::npos,
              "create bag: PRESET_DEFAULT sets no preset key (OTA default stays)");
    }

    // eval bag: the shipped helper's exact key and type order (NGX_D3D12_EVALUATE_DLSS_EXT), optional
    // resources skipped when null; the preset refresh lands with the reset/mv-scale group.
    {
        MockParameter m;
        ID3D12Resource *color = reinterpret_cast<ID3D12Resource*>(0x11), *out = reinterpret_cast<ID3D12Resource*>(0x22);
        leonida::dlss::FfxDescription fx{};
        fx.color = color;
        fx.output = out;
        fx.jitter_x = 0.25f;
        fx.jitter_y = -0.125f;
        fx.mv_scale_x = 0.0f;  // the shipped 0-means-1.0
        fx.mv_scale_y = 0.5f;
        fx.reset = true;
        fx.exposure = reinterpret_cast<ID3D12Resource*>(0x33);
        leonida::dlss::fill_eval_params(&m, fx, nvngx::PRESET_J, nvngx::PERF_VALUE_MAX_QUALITY);
        const char* expect =
            "Color=r12 Output=r12 Jitter.Offset.X=f Jitter.Offset.Y=f Sharpness=f Reset=i "
            "MV.Scale.X=f MV.Scale.Y=f DLSS.Hint.Render.Preset.Quality=i ExposureTexture=r12 ";
        check(m.dump() == expect, "eval bag: helper key/type order, null depth/mv skipped, preset refreshed");
    }

    // sweep: across the plausible FFX ratio range, the bucket agrees with an independent, table-driven
    // reference at every step — the .005-band edges (1.05/1.6/1.71/2.0/3.0), the 1.7237 collision band,
    // and the 2.0-3.0 UltraQuality fold are each pinned at thousands of points, not just the edges.
    {
        auto reference = [](float r) {
            // The table, written straight from the guide's Optimal Settings ratios (MaxQuality 1.5,
            // UltraQuality 1.7, Balanced 1.7237, MaxPerf 3.0) and the shipped-enum conservatism.
            if (r >= 3.0f) return nvngx::PERF_VALUE_MAX_PERF;
            if (r >= 2.0f) return nvngx::PERF_VALUE_ULTRA_QUALITY;
            if (r > 1.71f) return nvngx::PERF_VALUE_BALANCED;   // 1.7237x, the Balanced band up to 2.0
            if (r > 1.6f) return nvngx::PERF_VALUE_ULTRA_QUALITY;
            if (r > 1.05f) return nvngx::PERF_VALUE_MAX_QUALITY;
            return nvngx::PERF_VALUE_DLAA;
        };
        bool ok = true;
        float first_bad = 0.0f;
        for (int step = 0; step <= 3500; ++step) {
            const float r = 1.0f + step * 0.001f;
            if (leonida::dlss::pick_quality(r) != reference(r)) {
                ok = false;
                first_bad = r;
                break;
            }
        }
        check(ok, "sweep 1.0..4.5 @0.001: pick_quality matches the ratio table at every step");
        if (!ok) std::printf("      first mismatch at %.3f\n", first_bad);
    }

    std::printf("dlss_backend_unit: %s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
