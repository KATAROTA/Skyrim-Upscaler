#include <SkyrimUpscaler.h>
#include <PCH.h>
#include <DRS.h>
#include <ReShadePlugin.h>
#include <cmath>
#include <d3dcompiler.h>
#include <RE/R/Renderer.h>
#include <cstring>
#include <RE/S/State.h>
#include "NativeMetalFX.h"


#define GetSettingInt(a_section, a_setting, a_default) a_setting = (int)ini.GetLongValue(a_section, #a_setting, a_default);
#define SetSettingInt(a_section, a_setting) ini.SetLongValue(a_section, #a_setting, a_setting);

#define GetSettingFloat(a_section, a_setting, a_default) a_setting = (float)ini.GetDoubleValue(a_section, #a_setting, a_default);
#define SetSettingFloat(a_section, a_setting) ini.SetDoubleValue(a_section, #a_setting, a_setting);

#define GetSettingBool(a_section, a_setting, a_default) a_setting = ini.GetBoolValue(a_section, #a_setting, a_default);
#define SetSettingBool(a_section, a_setting) ini.SetBoolValue(a_section, #a_setting, a_setting);




void SkyrimUpscaler::LoadINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(L"Data\\SKSE\\Plugins\\SkyrimUpscaler.ini");
	GetSettingBool("Settings", mEnableUpscaler, false);
	GetSettingInt("Settings", mUpscaleType, 0);
	GetSettingInt("Settings", mQualityLevel, 2);
	GetSettingBool("Settings", mUseOptimalMipLodBias, true);
	GetSettingFloat("Settings", mMipLodBias, 0);
	GetSettingBool("Settings", mSharpening, true);
	GetSettingFloat("Settings", mSharpness, 0.20);
	// This branch targets the DXMT native backend, not the broken FSR2 shared-resource path.
    if (mUpscaleType != DLSS && mUpscaleType != DLAA) {
        logger::warn("Build20 native backend: legacy mode {} replaced by MetalFX", mUpscaleType);
        mUpscaleType = DLSS;
    }
    mQualityLevel = std::clamp(mQualityLevel, 0, 2);
    mSharpness = std::isfinite(mSharpness) ? std::clamp(mSharpness, 0.0f, 1.0f) : 0.20f;
    GetSettingBool("Settings", mEnableJitter, true);
    mDisableEvaluation = false;
	int mToggleHotkey = ImGuiKey_End;
	GetSettingInt("Hotkeys", mToggleHotkey, ImGuiKey_End);
	SettingGUI::GetSingleton()->mToggleHotkey = mToggleHotkey;

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
	ini.SaveFile(L"Data\\SKSE\\Plugins\\SkyrimUpscaler.ini");
}

void SkyrimUpscaler::MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	static bool inited = false;
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
	case SKSE::MessagingInterface::kNewGame:
	case SKSE::MessagingInterface::kPreLoadGame:
		if (!inited) {
			inited = true;
		}
        ResetNativeHistory();
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
	if (mContext == nullptr)
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


bool SkyrimUpscaler::ConfigureNativeMode(int mode, int quality)
{
    if (!mNativeAvailable || mNativeFailed || (mode != DLSS && mode != DLAA)) return false;
    quality = std::clamp(quality, 0, 2);
    const float scales[] = {0.5f, 0.58f, 2.0f/3.0f};
    const float scale = mode == DLAA ? 1.0f : scales[quality];
    const int w = (std::max)(1, static_cast<int>(std::lround(mDisplaySizeX * scale)));
    const int h = (std::max)(1, static_cast<int>(std::lround(mDisplaySizeY * scale)));
    if (!b20::native.Configure(static_cast<UINT>(w), static_cast<UINT>(h))) return false;
    mUpscaleType = mode; mQualityLevel = quality;
    mRenderSizeX = w; mRenderSizeY = h; mRenderScale = float(w)/mDisplaySizeX;
    mMotionScale[0] = float(w); mMotionScale[1] = float(h);
    ResetNativeHistory();
    DRS::GetSingleton()->targetScaleFactor = mRenderScale;
    DRS::GetSingleton()->ControlResolution();
    return true;
}

void SkyrimUpscaler::ResetNativeHistory()
{
    mNativeHistoryReset = true;
    mNativeViewportSeen.store(false, std::memory_order_relaxed);
    mNativeViewportWait=0;
    mJitterIndex = 0;
    b20::native.Reset();
}

void SkyrimUpscaler::UpdateNativeTAA()
{
    auto* taa = UnkOuterStruct::GetSingleton();
    if (!taa || !taa->unkInnerStruct) return;
    if (!mNativeTAAKnown) { mNativeOriginalTAA = taa->GetTAA(); mNativeTAAKnown = true; }
    taa->SetTAA(IsEnabled() ? false : mNativeOriginalTAA);
}

bool SkyrimUpscaler::EvaluateBeforePostProcessing()
{
    if (!mNativeAvailable || mNativeFailed || !mBuild19PostProcessHookInstalled) return false;
    if (DRS::GetSingleton()->reset) { ResetNativeHistory(); return false; }
    b20::Source s;
    const bool valid = b20::GetSource(s, static_cast<UINT>(mDisplaySizeX), static_cast<UINT>(mDisplaySizeY));
    if (!mUpscalerReady) {
        if (!valid) {
            static unsigned attempts = 0;
            if (++attempts == 1) logger::warn("Build20 waiting for validated kMAIN, motion and depth textures");
            if (attempts >= 120) { mNativeFailed = true; mEnableUpscaler = false; logger::error("Build20 engine texture validation failed; native rendering retained"); }
            return false;
        }
        if (!ConfigureNativeMode(mUpscaleType, mQualityLevel)) {
            mNativeFailed = true; mEnableUpscaler = false; logger::error("Build20 texture preflight failed; native rendering retained"); return false;
        }
        mUpscalerReady = true;
        logger::info("Build20 engine sources READY; color={}, depth={}, motion={}", static_cast<unsigned>(s.colorDesc.Format), static_cast<unsigned>(s.depthDesc.Format), static_cast<unsigned>(s.motionDesc.Format));
        SetEnabled(mEnableUpscaler);
        // The current world frame was rendered at native resolution. Start DRS on the next frame.
        return false;
    }
    if (!IsEnabled()) return false;
    // The 1.7.104 State layout differs from earlier AE builds. Validate the
    // actual ratios before taking a rectangular crop of the world buffers.
    auto* state = RE::BSGraphics::State::GetSingleton();
    if (state) {
        const auto& r=state->GetRuntimeData();
        const float x=float(mRenderSizeX)/mDisplaySizeX, y=float(mRenderSizeY)/mDisplaySizeY;
        if (std::fabs(r.dynamicResolutionWidthRatio-x)>0.003f ||
            std::fabs(r.dynamicResolutionHeightRatio-y)>0.003f) {
            if (++mNativeViewportWait < 12) return false;
            logger::error("Build20 DRS ratio mismatch: {}x{} expected {}x{}",r.dynamicResolutionWidthRatio,r.dynamicResolutionHeightRatio,x,y);
            mNativeFailed=true;mUpscalerReady=false;SetEnabled(false);return false;
        }
    } else { mNativeFailed=true;mUpscalerReady=false;SetEnabled(false);return false; }
    if (mUpscaleType==DLSS && !mNativeViewportSeen.load(std::memory_order_relaxed)) {
        if (++mNativeViewportWait < 12) return false;
        logger::error("Build20 DRS fix: no reduced world viewport observed");
        SetEnabled(false);
        DRS::GetSingleton()->SetDRS(reinterpret_cast<BSGraphics::State*>(state));
        return false;
    }
    if (valid && b20::native.Evaluate(s, mJitterOffsets[0], mJitterOffsets[1], mSharpening ? mSharpness : 0.0f)) {
        mNativeHistoryReset = false;
        mNativeViewportWait=0;
        return true;
    }
    mNativeFailed = true;
    mUpscalerReady = false;
    SetEnabled(false);
    logger::error("Build20 native evaluation aborted; restoring vanilla renderer, no late resampling");
    return false;
}

// Legacy pre-UI SDR resampling is intentionally disabled. Native MetalFX runs
// before ImageSpace and never upscales the UI or downsamples the final frame.
void SkyrimUpscaler::EvaluateUpscaler(ID3D11Texture2D*) {}

bool SkyrimUpscaler::IsEnabled()
{
	return mEnableUpscaler && mUpscalerReady && !DRS::GetSingleton()->reset;
}


void SkyrimUpscaler::GetJitters(float* out_x, float* out_y)
{
    *out_x = *out_y = 0.0f;
    if (!IsEnabled() || mRenderSizeX <= 0 || mRenderSizeY <= 0) return;
    const auto halton = [](unsigned index, unsigned base) {
        float f=1.0f, v=0.0f;
        while(index) { f/=float(base); v+=f*float(index%base); index/=base; }
        return v;
    };
    const float scale = float(mDisplaySizeX)/mRenderSizeX;
    const unsigned phase = std::clamp(static_cast<unsigned>(std::lround(8.0f*scale*scale)),8u,32u);
    const unsigned index = static_cast<unsigned>(mJitterIndex) % phase + 1u;
    mJitterIndex = float(index % phase);
    *out_x = halton(index,2)-0.5f;
    *out_y = halton(index,3)-0.5f;
}

void SkyrimUpscaler::SetJitterOffsets(float x, float y)
{
	mJitterOffsets[0] = x;
	mJitterOffsets[1] = y;
}

void SkyrimUpscaler::SetMotionScale(float x, float y)
{
    mMotionScale[0]=x; mMotionScale[1]=y;
}

void SkyrimUpscaler::SetEnabled(bool enabled)
{
    mEnableUpscaler = enabled && !mNativeFailed;
    ResetNativeHistory();
    DRS::GetSingleton()->targetScaleFactor = (mEnableUpscaler && mUpscalerReady) ? mRenderScale : 1.0f;
    DRS::GetSingleton()->ControlResolution();
    // The old aggressive sampler override is not used by this branch.
    mMipLodBias = 0.0f;
    UpdateNativeTAA();
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
    mNativeAvailable = false;
    mNativeFailed = false;
    if (mSwapChain == nullptr || mDevice == nullptr || mContext == nullptr || !mBuild19PostProcessHookInstalled) {
        mNativeFailed=true; SetEnabled(false); logger::error("Build20 prerequisites missing; vanilla renderer retained"); return;
    }
    b20::ComPtr<ID3D11Texture2D> back;
    if (FAILED(mSwapChain->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf()))) || !back) {
        mNativeFailed=true; SetEnabled(false); return;
    }
    D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);
    mDisplaySizeX=desc.Width;mDisplaySizeY=desc.Height;
    mRenderSizeX=mDisplaySizeX;mRenderSizeY=mDisplaySizeY;
    mRenderScale=1.0f;
    mNativeAvailable=b20::native.Init(mDevice,mContext,desc.Width,desc.Height);
    if(!mNativeAvailable){mNativeFailed=true;SetEnabled(false);logger::error("Build20 DXMT MetalFX initialization failed; vanilla renderer retained");return;}
    logger::info("Build20 DXMT MetalFX interface READY; waiting for first world frame {}x{}",mDisplaySizeX,mDisplaySizeY);
}
