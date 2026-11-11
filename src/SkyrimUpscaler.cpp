#include <SkyrimUpscaler.h>
#include <PCH.h>
#include <DRS.h>
#include <ReShadePlugin.h>
#include <cmath>
#include <d3dcompiler.h>
#include <RE/R/Renderer.h>
#include <cstring>


#define GetSettingInt(a_section, a_setting, a_default) a_setting = (int)ini.GetLongValue(a_section, #a_setting, a_default);
#define SetSettingInt(a_section, a_setting) ini.SetLongValue(a_section, #a_setting, a_setting);

#define GetSettingFloat(a_section, a_setting, a_default) a_setting = (float)ini.GetDoubleValue(a_section, #a_setting, a_default);
#define SetSettingFloat(a_section, a_setting) ini.SetDoubleValue(a_section, #a_setting, a_setting);

#define GetSettingBool(a_section, a_setting, a_default) a_setting = ini.GetBoolValue(a_section, #a_setting, a_default);
#define SetSettingBool(a_section, a_setting) ini.SetBoolValue(a_section, #a_setting, a_setting);


// Build 16: correct the COLOR input shape, not just the metadata passed to NVNGX.
// DXMT/MetalFX reads InputContentWidth/Height from the NGX evaluation parameters.
// Passing a full-size rendered image with smaller metadata crops/zooms the image.
// This GPU pass makes a genuinely smaller source texture. It does not yet cause
// Skyrim's own draw calls to render at lower resolution, so it is not a FPS fix.
namespace {
struct Build16GpuDownsample
{
    ID3D11Texture2D* smallTexture = nullptr;
    ID3D11ShaderResourceView* fullSRV = nullptr;
    ID3D11UnorderedAccessView* smallUAV = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    bool attempted = false;
    bool ready = false;

    bool Ensure(ID3D11Device* dev, ID3D11Texture2D* full, UINT width, UINT height)
    {
        if (ready) return true;
        if (attempted || !dev || !full || !width || !height) return false;
        attempted = true;
        D3D11_TEXTURE2D_DESC sourceDesc{};
        full->GetDesc(&sourceDesc);
        if (sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            !(sourceDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
            logger::error("Build16 downsample: full color texture does not support RGBA8 SRV, format={} bind=0x{:X}",
                static_cast<unsigned int>(sourceDesc.Format), sourceDesc.BindFlags);
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &smallTexture);
        if (FAILED(hr) || !smallTexture) {
            logger::error("Build16 downsample: CreateTexture2D failed 0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        hr = dev->CreateShaderResourceView(full, nullptr, &fullSRV);
        if (FAILED(hr) || !fullSRV) {
            logger::error("Build16 downsample: input SRV failed 0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        hr = dev->CreateUnorderedAccessView(smallTexture, nullptr, &smallUAV);
        if (FAILED(hr) || !smallUAV) {
            logger::error("Build16 downsample: output UAV failed 0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.MaxAnisotropy = 1;
        samp.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        samp.MinLOD = 0.0f;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        hr = dev->CreateSamplerState(&samp, &sampler);
        if (FAILED(hr) || !sampler) {
            logger::error("Build16 downsample: sampler failed 0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }

        // Use the Windows/Wine shader compiler at runtime. No bundled binaries.
        HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
        if (!dll) {
            logger::error("Build16 downsample: d3dcompiler_47.dll is unavailable; using safe full-input fallback");
            return false;
        }
        using CompileFn = decltype(&D3DCompile);
        auto compile = reinterpret_cast<CompileFn>(GetProcAddress(dll, "D3DCompile"));
        if (!compile) {
            logger::error("Build16 downsample: D3DCompile export not found");
            return false;
        }
        static constexpr char shader[] = R"HLSL(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);
SamplerState LinearClamp : register(s0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    OutputColor.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5f) / float2(w, h);
    OutputColor[id.xy] = InputColor.SampleLevel(LinearClamp, uv, 0.0f);
}
)HLSL";
        ID3DBlob* bytecode = nullptr;
        ID3DBlob* errors = nullptr;
        hr = compile(shader, std::strlen(shader), "Build16ColorDownsample", nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
        if (FAILED(hr) || !bytecode) {
            logger::error("Build16 downsample: HLSL compilation failed 0x{:08X}: {}",
                static_cast<unsigned int>(hr),
                errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "no compiler diagnostics");
            if (errors) errors->Release();
            if (bytecode) bytecode->Release();
            return false;
        }
        if (errors) errors->Release();
        hr = dev->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &cs);
        bytecode->Release();
        if (FAILED(hr) || !cs) {
            logger::error("Build16 downsample: CreateComputeShader failed 0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        ready = true;
        logger::info("Build16 GPU downsample ready: {}x{} -> {}x{}, format=RGBA8",
            sourceDesc.Width, sourceDesc.Height, width, height);
        return true;
    }

    ID3D11Texture2D* Execute(ID3D11Device* dev, ID3D11DeviceContext* ctx,
        ID3D11Texture2D* full, UINT width, UINT height)
    {
        if (!ctx || !Ensure(dev, full, width, height)) return nullptr;
        ID3D11ComputeShader* previousShader = nullptr;
        ID3D11ShaderResourceView* previousSRV = nullptr;
        ID3D11UnorderedAccessView* previousUAV = nullptr;
        ID3D11SamplerState* previousSampler = nullptr;
        ctx->CSGetShader(&previousShader, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &previousSRV);
        ctx->CSGetUnorderedAccessViews(0, 1, &previousUAV);
        ctx->CSGetSamplers(0, 1, &previousSampler);

        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &fullSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &smallUAV, nullptr);
        ctx->CSSetSamplers(0, 1, &sampler);
        ctx->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        ctx->CSSetShaderResources(0, 1, &nullSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
        ctx->CSSetShader(previousShader, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &previousSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &previousUAV, nullptr);
        ctx->CSSetSamplers(0, 1, &previousSampler);

        if (previousShader) previousShader->Release();
        if (previousSRV) previousSRV->Release();
        if (previousUAV) previousUAV->Release();
        if (previousSampler) previousSampler->Release();
        return smallTexture;
    }
};
static Build16GpuDownsample gBuild16Downsample;

// Build 18: optional EARLY INPUT experiment. kMAIN is R11G11B10_FLOAT (HDR),
// not the final SDR backbuffer. Only the active DRS rectangle is sampled.
// This is not a final HDR/tone-mapping solution: user must compare colours.
struct Build18EarlyMain
{
    ID3D11Texture2D* output = nullptr;
    ID3D11Texture2D* sourceTexture = nullptr;
    ID3D11ShaderResourceView* sourceSRV = nullptr;
    ID3D11UnorderedAccessView* outputUAV = nullptr;
    ID3D11ComputeShader* shader = nullptr;
    bool attempted = false;
    bool ready = false;

    bool Ensure(ID3D11Device* dev, ID3D11Texture2D* src, UINT width, UINT height)
    {
        if (ready) return src == sourceTexture;
        if (attempted || !dev || !src || !width || !height) return false;
        attempted = true;
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
            !(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            sd.Width < width || sd.Height < height ||
            sd.SampleDesc.Count != 1) {
            logger::warn("Build18 kMAIN rejected: {}x{} format={} bind=0x{:X}, requested={}x{}",
                sd.Width, sd.Height, static_cast<unsigned int>(sd.Format), sd.BindFlags, width, height);
            return false;
        }
        D3D11_TEXTURE2D_DESC od{};
        od.Width = width;
        od.Height = height;
        od.ArraySize = 1;
        od.MipLevels = 1;
        od.SampleDesc.Count = 1;
        od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        od.Usage = D3D11_USAGE_DEFAULT;
        od.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = dev->CreateTexture2D(&od, nullptr, &output);
        if (FAILED(hr) || !output) {
            logger::error("Build18 kMAIN: output texture allocation failed HRESULT=0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        hr = dev->CreateShaderResourceView(src, nullptr, &sourceSRV);
        if (FAILED(hr) || !sourceSRV) {
            logger::error("Build18 kMAIN: source SRV creation failed HRESULT=0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        hr = dev->CreateUnorderedAccessView(output, nullptr, &outputUAV);
        if (FAILED(hr) || !outputUAV) {
            logger::error("Build18 kMAIN: output UAV creation failed HRESULT=0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
        if (!dll) {
            logger::error("Build18 kMAIN: d3dcompiler_47.dll missing");
            return false;
        }
        using CompileFn = decltype(&D3DCompile);
        auto compile = reinterpret_cast<CompileFn>(GetProcAddress(dll, "D3DCompile"));
        if (!compile) {
            logger::error("Build18 kMAIN: D3DCompile missing");
            return false;
        }
        static constexpr char srcCode[] = R"HLSL(
Texture2D<float4> SceneHDR : register(t0);
RWTexture2D<float4> SmallSDR : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    SmallSDR.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    // IMPORTANT: Load, not SampleLevel: the native engine already rendered the
    // reduced scene into this region. Resampling the full HDR image would re-zoom.
    float3 hdr = max(SceneHDR.Load(int3(tid.xy, 0)).rgb, 0.0f);
    // TEMPORARY tone mapping; cannot reproduce Skyrim's image-space effects.
    float3 mapped = hdr / (1.0f + hdr);
    float3 displayColor = pow(saturate(mapped), 1.0f / 2.2f);
    SmallSDR[tid.xy] = float4(displayColor, 1.0f);
}
)HLSL";
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        hr = compile(srcCode, std::strlen(srcCode), "Build18EarlyMain", nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr) || !code) {
            logger::error("Build18 kMAIN: HLSL compile failed HRESULT=0x{:08X}: {}",
                static_cast<unsigned int>(hr),
                errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "no compiler message");
            if (errors) errors->Release();
            if (code) code->Release();
            return false;
        }
        if (errors) errors->Release();
        hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
        code->Release();
        if (FAILED(hr) || !shader) {
            logger::error("Build18 kMAIN: compute shader creation failed HRESULT=0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        sourceTexture = src;
        ready = true;
        logger::info("Build18 kMAIN prepared: HDR {}x{} -> SDR {}x{} (pre-postprocessing EXPERIMENT)",
            sd.Width, sd.Height, width, height);
        return true;
    }

    ID3D11Texture2D* Execute(ID3D11Device* dev, ID3D11DeviceContext* ctx,
        ID3D11Texture2D* src, UINT width, UINT height)
    {
        if (!ctx || !Ensure(dev, src, width, height)) return nullptr;
        ID3D11ComputeShader* oldShader = nullptr;
        ID3D11ShaderResourceView* oldSRV = nullptr;
        ID3D11UnorderedAccessView* oldUAV = nullptr;
        ID3D11RenderTargetView* oldRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* oldDSV = nullptr;
        ctx->CSGetShader(&oldShader, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &oldSRV);
        ctx->CSGetUnorderedAccessViews(0, 1, &oldUAV);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, &oldDSV);

        // Avoid an SRV / RTV feedback hazard if the HDR scene is still bound.
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &sourceSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &outputUAV, nullptr);
        ctx->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        ctx->CSSetShaderResources(0, 1, &nullSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
        ctx->CSSetShader(oldShader, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &oldSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &oldUAV, nullptr);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, oldDSV);
        if (oldShader) oldShader->Release();
        if (oldSRV) oldSRV->Release();
        if (oldUAV) oldUAV->Release();
        for (auto* rtv : oldRTV) if (rtv) rtv->Release();
        if (oldDSV) oldDSV->Release();
        return output;
    }
};
static Build18EarlyMain gBuild18EarlyMain;

// Build19: full-size output of the early NGX evaluation. Never write into the swapchain.
// The colour is converted back to HDR kMAIN before Bethesda's image-space stage.
struct Build19PrePostResolve
{
    ID3D11Texture2D* output = nullptr;
    ID3D11ShaderResourceView* outputSRV = nullptr;
    ID3D11ComputeShader* decoder = nullptr;
    bool attempted = false;
    bool ready = false;

    bool Ensure(ID3D11Device* dev, ID3D11Texture2D* backBuffer)
    {
        if (ready) return true;
        if (attempted || !dev || !backBuffer) return false;
        attempted = true;

        D3D11_TEXTURE2D_DESC d{};
        backBuffer->GetDesc(&d);
        if (!d.Width || !d.Height || d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            logger::warn("Build19 prepost: output texture requires RGBA8 backbuffer; format={}",
                static_cast<unsigned int>(d.Format));
            return false;
        }
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        d.MiscFlags = 0;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        HRESULT hr = dev->CreateTexture2D(&d, nullptr, &output);
        if (FAILED(hr) || !output) {
            logger::error("Build19 prepost: intermediate output allocation failed HRESULT=0x{:08X}",
                static_cast<unsigned int>(hr));
            return false;
        }
        hr = dev->CreateShaderResourceView(output, nullptr, &outputSRV);
        if (FAILED(hr) || !outputSRV) {
            logger::error("Build19 prepost: output SRV failed HRESULT=0x{:08X}",
                static_cast<unsigned int>(hr));
            return false;
        }
        HMODULE compilerDll = LoadLibraryA("d3dcompiler_47.dll");
        if (!compilerDll) {
            logger::error("Build19 prepost: d3dcompiler_47.dll unavailable");
            return false;
        }
        using CompileFn = decltype(&D3DCompile);
        auto compile = reinterpret_cast<CompileFn>(GetProcAddress(compilerDll, "D3DCompile"));
        if (!compile) {
            logger::error("Build19 prepost: D3DCompile unavailable");
            return false;
        }
        static constexpr char source[] = R"HLSL(
Texture2D<float4> UpscaledSDR : register(t0);
RWTexture2D<float4> DestinationHDR : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    DestinationHDR.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    // Inverse of Build18EarlyMain's Reinhard + 2.2 gamma encoding.
    // This is a reversible approximation, not Skyrim's actual tone mapping.
    float3 encoded = saturate(UpscaledSDR.Load(int3(tid.xy, 0)).rgb);
    float3 reinhard = pow(encoded, 2.2f);
    float3 hdr = reinhard / max(1.0f - reinhard, 0.00390625f);
    DestinationHDR[tid.xy] = float4(min(hdr, 64.0f), 1.0f);
}
)HLSL";
        ID3DBlob* bytecode = nullptr;
        ID3DBlob* diagnostics = nullptr;
        hr = compile(source, std::strlen(source), "Build19PrePostResolve", nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &diagnostics);
        if (FAILED(hr) || !bytecode) {
            logger::error("Build19 prepost decoder HLSL compile failed: HRESULT=0x{:08X}: {}",
                static_cast<unsigned int>(hr), diagnostics ?
                std::string(static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()) :
                std::string("no compiler details"));
            if (diagnostics) diagnostics->Release();
            if (bytecode) bytecode->Release();
            return false;
        }
        if (diagnostics) diagnostics->Release();
        hr = dev->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &decoder);
        bytecode->Release();
        if (FAILED(hr) || !decoder) {
            logger::error("Build19 prepost: decoder shader creation failed HRESULT=0x{:08X}",
                static_cast<unsigned int>(hr));
            return false;
        }
        ready = true;
        logger::info("Build19 prepost GPU path READY: NGX RGBA8 output -> HDR kMAIN, Bethesda ImageSpace remains enabled");
        return true;
    }

    bool DecodeToMain(ID3D11DeviceContext* ctx, ID3D11Texture2D* mainTexture,
        ID3D11UnorderedAccessView* mainUAV)
    {
        if (!ctx || !ready || !mainTexture || !mainUAV) return false;
        D3D11_TEXTURE2D_DESC mainDesc{};
        mainTexture->GetDesc(&mainDesc);
        if (mainDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT) return false;

        ID3D11ComputeShader* previousShader = nullptr;
        ID3D11ShaderResourceView* previousSRV = nullptr;
        ID3D11UnorderedAccessView* previousUAV = nullptr;
        ID3D11RenderTargetView* previousRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* previousDSV = nullptr;
        ctx->CSGetShader(&previousShader, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &previousSRV);
        ctx->CSGetUnorderedAccessViews(0, 1, &previousUAV);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previousRTV, &previousDSV);

        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->CSSetShader(decoder, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &outputSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &mainUAV, nullptr);
        ctx->Dispatch((mainDesc.Width + 7) / 8, (mainDesc.Height + 7) / 8, 1);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        ctx->CSSetShaderResources(0, 1, &nullSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
        ctx->CSSetShader(previousShader, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &previousSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &previousUAV, nullptr);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previousRTV, previousDSV);

        if (previousShader) previousShader->Release();
        if (previousSRV) previousSRV->Release();
        if (previousUAV) previousUAV->Release();
        for (auto* rtv : previousRTV) if (rtv) rtv->Release();
        if (previousDSV) previousDSV->Release();
        return true;
    }
};
static Build19PrePostResolve gBuild19PrePostResolve;

}

void SkyrimUpscaler::LoadINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(L"Data\\SKSE\\Plugins\\SkyrimUpscaler.ini");
	GetSettingBool("Settings", mEnableUpscaler, false);
	GetSettingInt("Settings", mUpscaleType, 0);
	GetSettingInt("Settings", mQualityLevel, 0);
	GetSettingBool("Settings", mUseOptimalMipLodBias, true);
	GetSettingFloat("Settings", mMipLodBias, 0);
	GetSettingBool("Settings", mSharpening, false);
	GetSettingFloat("Settings", mSharpness, 0);
	mUpscaleType = std::clamp(mUpscaleType, 0, 3);
	mQualityLevel = std::clamp(mQualityLevel, 0, 3);
	int mToggleHotkey = ImGuiKey_End;
	GetSettingInt("Hotkeys", mToggleHotkey, ImGuiKey_End);
	SettingGUI::GetSingleton()->mToggleHotkey = mToggleHotkey;

	auto bFXAAEnabled = RE::GetINISetting("bFXAAEnabled:Display");
	if (!REL::Module::IsVR()) {
		if (bFXAAEnabled) {
			if (bFXAAEnabled->GetBool())
				logger::info("Forcing FXAA off.");
			bFXAAEnabled->data.b = false;
		}
	}
}
void SkyrimUpscaler::SaveINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(L"Data\\SKSE\\Plugins\\SkyrimUpscaler.ini");
	SetSettingBool("Settings", mEnableUpscaler);
	SetSettingInt("Settings", mUpscaleType);
	SetSettingInt("Settings", mQualityLevel);
	SetSettingBool("Settings", mUseOptimalMipLodBias);
	SetSettingFloat("Settings", mMipLodBias);
	SetSettingBool("Settings", mSharpening);
	SetSettingFloat("Settings", mSharpness);
	int mToggleHotkey = SettingGUI::GetSingleton()->mToggleHotkey;
	SetSettingInt("Hotkeys", mToggleHotkey);
}

void SkyrimUpscaler::MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	static bool inited = false;
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
	case SKSE::MessagingInterface::kNewGame:
	case SKSE::MessagingInterface::kPreLoadGame:
		if (!inited) {
			LoadINI();
			//InitUpscaler();
			inited = true;
		}
		break;

	case SKSE::MessagingInterface::kSaveGame:
	case SKSE::MessagingInterface::kDeleteGame:
		SaveINI();
		break;
	}
}

void SkyrimUpscaler::SetupSwapChain(IDXGISwapChain* swapchain)
{
	mSwapChain = swapchain;
	if (FAILED(mSwapChain->GetDevice(IID_PPV_ARGS(&mDevice))) || mDevice == nullptr) {
		logger::error("IDXGISwapChain::GetDevice failed");
		return;
	}
	mDevice->GetImmediateContext(&mContext);
	if (mContext == nullptr || !SetupDirectX(mDevice, 0))
		logger::error("Direct3D upscaler initialization failed");
}

float SkyrimUpscaler::GetVerticalFOVRad()
{
	static float& fac = (*(float*)(RELOCATION_ID(513786, 388785).address()));
	const auto    base = fac;
	const auto    x = base / 1.30322540f;
	const auto    vFOV = 2 * atan(x / (float(mRenderSizeX) / mRenderSizeY));
	return vFOV;
}


// Called by the verified Main_PostProcessing hook, BEFORE Skyrim image-space.
// If any prerequisite is missing, the old late-frame Build17 path runs instead.
bool SkyrimUpscaler::EvaluateBeforePostProcessing()
{
    if (!mBuild19PostProcessHookInstalled || !mUseBuild19PrePost || !mTryEngineDRSRatios ||
        !IsEnabled() || mDisableEvaluation || mUpscaleType != DLSS ||
        mRenderSizeX <= 0 || mRenderSizeY <= 0 || !mDevice || !mContext ||
        !mTargetTex.mImage || !mMotionVectors.mImage || !mDepthBuffer.mImage ||
        !mMotionVectorsEmpty.mImage)
        return false;

    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) return false;
    auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    if (!main.texture || !main.UAV) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logger::warn("Build19 prepost: kMAIN texture or UAV missing, using Build17 fallback");
        }
        return false;
    }
    auto* mainColor = reinterpret_cast<ID3D11Texture2D*>(main.texture);
    D3D11_TEXTURE2D_DESC desc{};
    mainColor->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
        desc.Width != static_cast<UINT>(mDisplaySizeX) ||
        desc.Height != static_cast<UINT>(mDisplaySizeY)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logger::warn("Build19 prepost: kMAIN unexpected: {}x{} fmt={}",
                desc.Width, desc.Height, static_cast<unsigned int>(desc.Format));
        }
        return false;
    }
    if (!gBuild19PrePostResolve.Ensure(mDevice, mTargetTex.mImage)) return false;

    // Use exactly the pixels Skyrim rendered under native engine DRS.
    auto* reducedColor = gBuild18EarlyMain.Execute(mDevice, mContext, mainColor,
        static_cast<UINT>(mRenderSizeX), static_cast<UINT>(mRenderSizeY));
    if (!reducedColor) return false;

    static float& nearPlane = (*(float*)(RELOCATION_ID(517032, 403540).address() + 0x40));
    static float& farPlane = (*(float*)(RELOCATION_ID(517032, 403540).address() + 0x44));
    ID3D11Texture2D* mask = mEnableTransparentMask ? mTransparentMask.mImage : nullptr;
    const int jitter = mEnableJitter ? 1 : 0;
    SimpleEvaluate(0, reducedColor, mMotionVectors.mImage, mDepthBuffer.mImage,
        mask, gBuild19PrePostResolve.output,
        mRenderSizeX, mRenderSizeY, mSharpness,
        mJitterOffsets[0] * jitter, mJitterOffsets[1] * jitter,
        mMotionScale[0], mMotionScale[1], false,
        nearPlane / 100.0f, farPlane / 100.0f, GetVerticalFOVRad());

    if (!gBuild19PrePostResolve.DecodeToMain(mContext, mainColor, reinterpret_cast<ID3D11UnorderedAccessView*>(main.UAV)))
        return false;
    // Keep motion vectors intact through Skyrim's native image-space stage.
    static unsigned int successCount = 0;
    if (successCount++ < 8) {
        logger::info("Build19 prepost APPLIED: kMAIN {}x{} HDR -> {}x{} RGBA8 -> MetalFX {}x{} -> HDR kMAIN; native image-space follows",
            desc.Width, desc.Height, mRenderSizeX, mRenderSizeY,
            mDisplaySizeX, mDisplaySizeY);
    }
    return true;
}

void SkyrimUpscaler::EvaluateUpscaler(ID3D11Texture2D* source)
{
	static float&      g_fNear = (*(float*)(RELOCATION_ID(517032, 403540).address() + 0x40));  // 2F26FC0, 2FC1A90
	static float&      g_fFar = (*(float*)(RELOCATION_ID(517032, 403540).address() + 0x44));   // 2F26FC4, 2FC1A94
	static bool        lastEnable = false;

	if (mSwapChain != nullptr) {
		if (mContext != nullptr && mTempColor.mImage != nullptr &&
			mMotionVectorsEmpty.mImage != nullptr && mTargetTex.mImage != nullptr &&
			mDepthBuffer.mImage != nullptr && mMotionVectors.mImage != nullptr) {
			ID3D11Texture2D* transparentMask = (mEnableTransparentMask ? mTransparentMask.mImage : nullptr);
			bool             enable = IsEnabled();
			bool             delayOneFrame = false && (lastEnable && !enable);
			bool             TAAEnabled = (mUpscaleType == TAA);
			if (((IsEnabled() && !DRS::GetSingleton()->reset) || delayOneFrame) && !TAAEnabled) {
				lastEnable = enable;
				// For DLSS to work in borderless mode we must copy the backbuffer to a temporary texture
				if (source == nullptr)
					source = mTargetTex.mImage;
				// Build18: defer the full-frame copy until the fallback SDR path is actually used.
				int j = (mEnableJitter) ? 1 : 0;
				if (!mDisableEvaluation) {
					bool fullInput = mForceMetalFXFullInput && (mUpscaleType == DLSS || mUpscaleType == DLAA);
					int inputWidth = fullInput ? mDisplaySizeX : mRenderSizeX;
					int inputHeight = fullInput ? mDisplaySizeY : mRenderSizeY;
					ID3D11Texture2D* evaluationColor = mTempColor.mImage;
                    bool usedEarlyMain = false;
                    if (mUpscaleType == DLSS && !fullInput &&
                        mTryEngineDRSRatios && mUseBuild18EarlyMain) {
                        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                        if (renderer) {
                            // Use the renderer-owned kMAIN, never an unverified fixed address.
                            auto* rawMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture;
                            auto* mainColor = reinterpret_cast<ID3D11Texture2D*>(rawMain);
                            if (mainColor) {
                                if (auto* smallColor = gBuild18EarlyMain.Execute(mDevice, mContext, mainColor,
                                    static_cast<UINT>(mRenderSizeX), static_cast<UINT>(mRenderSizeY))) {
                                    evaluationColor = smallColor;
                                    usedEarlyMain = true;
                                }
                            }
                        }
                        static bool warned = false;
                        if (!usedEarlyMain && !warned) {
                            logger::warn("Build18 kMAIN unavailable or incompatible; using Build16 SDR fallback");
                            warned = true;
                        }
                    }
                    if (!usedEarlyMain)
                        mContext->CopyResource(mTempColor.mImage, source);
					if (!usedEarlyMain && mUpscaleType == DLSS && !fullInput && mUseMetalFXGpuDownsample) {
						if (auto* resized = gBuild16Downsample.Execute(mDevice, mContext, mTempColor.mImage,
							static_cast<UINT>(mRenderSizeX), static_cast<UINT>(mRenderSizeY))) {
							evaluationColor = resized;
						} else {
							// No crop/zoom if shader creation fails; preserve the last working visual path.
							fullInput = true;
							inputWidth = mDisplaySizeX;
							inputHeight = mDisplaySizeY;
						}
					}
					static int lastFullInput = -1;
					static unsigned int diagCount = 0;
					if (lastFullInput != static_cast<int>(fullInput)) {
						lastFullInput = static_cast<int>(fullInput);
						diagCount = 0;
					}
					if (diagCount++ < 5) {
						D3D11_TEXTURE2D_DESC actualInputDesc{};
						evaluationColor->GetDesc(&actualInputDesc);
						logger::info("Build18 MetalFX source: texture={}x{} declared={}x{} gpuResample={} fullInput={} earlyMain={}",
							actualInputDesc.Width, actualInputDesc.Height, inputWidth, inputHeight,
							evaluationColor != mTempColor.mImage && !usedEarlyMain, fullInput, usedEarlyMain);
						D3D11_TEXTURE2D_DESC srcDesc{}, dstDesc{}, mvDesc{}, depthDesc{};
						mTempColor.mImage->GetDesc(&srcDesc);
						mTargetTex.mImage->GetDesc(&dstDesc);
							mMotionVectors.mImage->GetDesc(&mvDesc);
						mDepthBuffer.mImage->GetDesc(&depthDesc);
						UINT viewportCount = 1;
						D3D11_VIEWPORT viewport{};
						mContext->RSGetViewports(&viewportCount, &viewport);
						const auto* drs = DRS::GetSingleton();
						logger::info("MetalFX input test: mode={} fullInput={} inputRegion={}x{} source={}x{} output={}x{} motion={}x{} depth={}x{} DRS current={} target={} reset={} currentViewport={}x{}",
							mUpscaleType, fullInput, inputWidth, inputHeight, srcDesc.Width, srcDesc.Height,
							dstDesc.Width, dstDesc.Height, mvDesc.Width, mvDesc.Height, depthDesc.Width, depthDesc.Height,
							drs->currentScaleFactor, drs->targetScaleFactor, drs->reset,
							viewportCount == 1 ? viewport.Width : 0.0f, viewportCount == 1 ? viewport.Height : 0.0f);
                        // These renderer buffers are advisory only: do not hand an HDR world buffer
                        // to MetalFX as an RGBA8 input without checking format, contents and timing.
                        D3D11_TEXTURE2D_DESC opaqueDesc{};
                        const bool hasOpaque = (mOpaqueColor.mImage != nullptr);
                        if (hasOpaque)
                            mOpaqueColor.mImage->GetDesc(&opaqueDesc);
                        logger::info("Build14 buffers: backbuffer={}x{} fmt={} source={}x{} fmt={} opaqueFound={} opaque={}x{} fmt={} bind=0x{:X} depthFmt={} motionFmt={}",
                            dstDesc.Width, dstDesc.Height, static_cast<unsigned int>(dstDesc.Format),
                            srcDesc.Width, srcDesc.Height, static_cast<unsigned int>(srcDesc.Format),
                            hasOpaque, opaqueDesc.Width, opaqueDesc.Height, static_cast<unsigned int>(opaqueDesc.Format), opaqueDesc.BindFlags,
                            static_cast<unsigned int>(depthDesc.Format), static_cast<unsigned int>(mvDesc.Format));
					}
					SimpleEvaluate(0, evaluationColor, mMotionVectors.mImage, mDepthBuffer.mImage, transparentMask, mTargetTex.mImage, inputWidth, inputHeight, mSharpness,
						mJitterOffsets[0] * j, mJitterOffsets[1] * j, mMotionScale[0], mMotionScale[1], false, g_fNear / 100, g_fFar / 100, GetVerticalFOVRad());
					mContext->CopyResource(mMotionVectors.mImage, mMotionVectorsEmpty.mImage);
				}
			}
		}
	}
}

bool SkyrimUpscaler::IsEnabled()
{
	return mEnableUpscaler && mUpscalerReady && !DRS::GetSingleton()->reset;
}


void SkyrimUpscaler::GetJitters(float* out_x, float* out_y)
{
	*out_x = 0.0f;
	*out_y = 0.0f;
	if (!mUpscalerReady || mRenderSizeX <= 0 || mRenderSizeY <= 0)
		return;
	const auto phase = GetJitterPhaseCount(0);
	if (phase <= 0) {
		static bool warned = false;
		if (!warned) {
			logger::error("GetJitterPhaseCount returned {}, withholding jitter", phase);
			warned = true;
		}
		return;
	}
	mJitterIndex++;
	GetJitterOffset(out_x, out_y, static_cast<int>(mJitterIndex), phase);
	if (!std::isfinite(*out_x) || !std::isfinite(*out_y)) {
		logger::error("GetJitterOffset returned nonfinite offsets");
		*out_x = *out_y = 0.0f;
	}
}

void SkyrimUpscaler::SetJitterOffsets(float x, float y)
{
	mJitterOffsets[0] = x;
	mJitterOffsets[1] = y;
}

void SkyrimUpscaler::SetMotionScale(float x, float y)
{
	mMotionScale[0] = x;
	mMotionScale[1] = y;
	SetMotionScaleX(0, x);
	SetMotionScaleY(0, y);
}

void SkyrimUpscaler::SetEnabled(bool enabled)
{
	mEnableUpscaler = enabled;
	if (mEnableUpscaler && mUpscaleType != TAA) {
		DRS::GetSingleton()->targetScaleFactor = mRenderScale;
		DRS::GetSingleton()->ControlResolution();
		if (mUseOptimalMipLodBias)
			mMipLodBias = (mUpscaleType==DLAA)?0:GetOptimalMipmapBias(0);
		UnkOuterStruct::GetSingleton()->SetTAA(false);
	} else {
		DRS::GetSingleton()->targetScaleFactor = 1.0f;
		DRS::GetSingleton()->ControlResolution();
		if (mUseOptimalMipLodBias)
			mMipLodBias = 0;
		UnkOuterStruct::GetSingleton()->SetTAA(mEnableUpscaler && mUpscaleType == TAA);
	}
}

void SkyrimUpscaler::SetupTarget(ID3D11Texture2D* target_buffer)
{
	mTargetTex.mImage = target_buffer;
}

void SkyrimUpscaler::SetupDepth(ID3D11Texture2D* depth_buffer)
{
	mDepthBuffer.mImage = depth_buffer;
}

void SkyrimUpscaler::SetupOpaqueColor(ID3D11Texture2D* opaque_buffer)
{
	mOpaqueColor.mImage = opaque_buffer;
}

void SkyrimUpscaler::SetupTransparentMask(ID3D11Texture2D* transparent_buffer)
{
	mTransparentMask.mImage = transparent_buffer;
}

void SkyrimUpscaler::SetupMotionVector(ID3D11Texture2D* motion_buffer)
{
	mMotionVectors.mImage = motion_buffer;
	if (!mMotionVectorsEmpty.mImage) {
		D3D11_TEXTURE2D_DESC desc;
		motion_buffer->GetDesc(&desc);
		if (FAILED(mDevice->CreateTexture2D(&desc, nullptr, &mMotionVectorsEmpty.mImage)) ||
			mMotionVectorsEmpty.mImage == nullptr) {
			logger::error("Unable to allocate motion-vector clearing texture");
			return;
		}
		ID3D11RenderTargetView* rtv = nullptr;
		if (SUCCEEDED(mDevice->CreateRenderTargetView(mMotionVectorsEmpty.mImage, nullptr, &rtv)) && rtv) {
			constexpr float black[4] = { 0.f, 0.f, 0.f, 0.f };
			mContext->ClearRenderTargetView(rtv, black);
			rtv->Release();
		} else {
			logger::error("Unable to clear motion-vector texture");
			mMotionVectorsEmpty.mImage->Release();
			mMotionVectorsEmpty.mImage = nullptr;
			return;
		}
		mDisplaySizeX = desc.Width;
		mDisplaySizeY = desc.Height;
	}
}

void SkyrimUpscaler::PreInit()
{
	LoadINI();
	if (!REL::Module::IsVR()) {
		ID3D11Texture2D* back_buffer;
		if (FAILED(mSwapChain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) || back_buffer == nullptr) {
			logger::error("Could not obtain backbuffer during PreInit");
			return;
		}
		D3D11_TEXTURE2D_DESC desc;
		back_buffer->GetDesc(&desc);
		back_buffer->Release();
		mDisplaySizeX = desc.Width;
		mDisplaySizeY = desc.Height;
	}
}

void SkyrimUpscaler::InitUpscaler()
{
	mUpscalerReady = false;
	mRenderSizeX = 0;
	mRenderSizeY = 0;
	D3D11_TEXTURE2D_DESC desc;
	ID3D11Texture2D*     back_buffer;
	if (mSwapChain == nullptr || FAILED(mSwapChain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
		back_buffer == nullptr) {
		logger::error("Could not obtain backbuffer during InitUpscaler");
		return;
	}
	back_buffer->GetDesc(&desc);
	back_buffer->Release();
	if (REL::Module::IsVR()) {
		desc.Width = mDisplaySizeX;
		desc.Height = mDisplaySizeY;
	}
	mDisplaySizeX = desc.Width;
	mDisplaySizeY = desc.Height;
	if (mUpscaleType != TAA) {
		int upscaleType = (mUpscaleType == DLAA) ? DLSS : mUpscaleType;
		// These two run on DX12, need to make sure the job of last frame is done before reinitializing
		if (upscaleType == FSR2 || upscaleType == XESS)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		logger::info("Upscaler SimpleInit starting: mode={}, quality={}, display={}x{}, format={}",
			upscaleType, mQualityLevel, mDisplaySizeX, mDisplaySizeY, static_cast<unsigned int>(desc.Format));
		mOutColor.mImage = (ID3D11Texture2D*)SimpleInit(0, upscaleType, mQualityLevel, mDisplaySizeX, mDisplaySizeY, false, false, false, false, mSharpening, true, desc.Format);
		logger::info("Upscaler SimpleInit returned output texture address: 0x{:X}", reinterpret_cast<std::uintptr_t>(mOutColor.mImage));
		if (mOutColor.mImage == nullptr) {
			logger::error("Upscaler output texture not available; disabling upscaler before jitter/rendering");
			SetEnabled(false);
			return;
		}
		D3D11_TEXTURE2D_DESC fullColorDesc = desc;
		fullColorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		fullColorDesc.MiscFlags = 0;
		fullColorDesc.CPUAccessFlags = 0;
		fullColorDesc.Usage = D3D11_USAGE_DEFAULT;
		if (!mTempColor.mImage &&
			(FAILED(mDevice->CreateTexture2D(&fullColorDesc, nullptr, &mTempColor.mImage)) ||
			 mTempColor.mImage == nullptr)) {
			logger::error("Unable to create upscaler source texture");
			SetEnabled(false);
			return;
		}
		if (mUpscaleType == DLAA) {
			mRenderSizeX = mDisplaySizeX;
			mRenderSizeY = mDisplaySizeY;
			mRenderScale = 1.0f;
		} else {
			mRenderSizeX = GetRenderWidth(0);
			mRenderSizeY = GetRenderHeight(0);
			mRenderScale = float(mRenderSizeX) / mDisplaySizeX;
		}
		logger::info("Upscaler render size {}x{} versus output size {}x{}", mRenderSizeX, mRenderSizeY, mDisplaySizeX, mDisplaySizeY);
		if (mRenderSizeX <= 0 || mRenderSizeY <= 0 || mRenderSizeX > 16384 || mRenderSizeY > 16384) {
			logger::error("Upscaler invalid render dimensions, stopping before Jitter / DRS / MotionScale");
			SetEnabled(false);
			return;
		}
		mMotionScale[0] = REL::Module::IsVR() ? mRenderSizeX/2 : mRenderSizeX;
		mMotionScale[1] = mRenderSizeY;
		SetMotionScaleX(0, mMotionScale[0]);
		SetMotionScaleY(0, mMotionScale[1]);
		if (mEnableUpscaler && mUpscaleType != DLAA) {
			DRS::GetSingleton()->targetScaleFactor = mRenderScale;
			DRS::GetSingleton()->ControlResolution();	
			if (mUseOptimalMipLodBias)
				mMipLodBias = GetOptimalMipmapBias(0);
		} else {
			DRS::GetSingleton()->targetScaleFactor = 1.0f;
			DRS::GetSingleton()->ControlResolution();
			if (mUseOptimalMipLodBias)
				mMipLodBias = 0;
		}
		mUpscalerReady = true;
		logger::info("Upscaler initialization READY; jitter can now use validated render dimensions");
		UnkOuterStruct::GetSingleton()->SetTAA(false);
	} else {
		DRS::GetSingleton()->targetScaleFactor = 1.0f;
		DRS::GetSingleton()->ControlResolution();
		if (mUseOptimalMipLodBias)
			mMipLodBias = 0;
		UnkOuterStruct::GetSingleton()->SetTAA(mEnableUpscaler);
	}
}
