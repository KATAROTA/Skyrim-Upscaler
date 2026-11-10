#include <SkyrimUpscaler.h>
#include <PCH.h>
#include <DRS.h>
#include <ReShadePlugin.h>
#include <cmath>
#include <d3dcompiler.h>
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
				mContext->CopyResource(mTempColor.mImage, source);
				int j = (mEnableJitter) ? 1 : 0;
				if (!mDisableEvaluation) {
					bool fullInput = mForceMetalFXFullInput && (mUpscaleType == DLSS || mUpscaleType == DLAA);
					int inputWidth = fullInput ? mDisplaySizeX : mRenderSizeX;
					int inputHeight = fullInput ? mDisplaySizeY : mRenderSizeY;
					ID3D11Texture2D* evaluationColor = mTempColor.mImage;
					if (mUpscaleType == DLSS && !fullInput && mUseMetalFXGpuDownsample) {
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
						logger::info("Build16 MetalFX source: texture={}x{} declared={}x{} gpuResample={} fullInput={}",
							actualInputDesc.Width, actualInputDesc.Height, inputWidth, inputHeight,
							evaluationColor != mTempColor.mImage, fullInput);
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
