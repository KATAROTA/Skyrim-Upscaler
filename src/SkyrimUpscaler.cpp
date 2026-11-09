#include <SkyrimUpscaler.h>
#include <PCH.h>
#include <DRS.h>
#include <ReShadePlugin.h>
#include <cmath>


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
					SimpleEvaluate(0, mTempColor.mImage, mMotionVectors.mImage, mDepthBuffer.mImage, transparentMask, mTargetTex.mImage, mRenderSizeX, mRenderSizeY, mSharpness, 
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
		logger::info("FSR2 SimpleInit starting: mode={}, quality={}, display={}x{}, format={}",
			upscaleType, mQualityLevel, mDisplaySizeX, mDisplaySizeY, static_cast<unsigned int>(desc.Format));
		mOutColor.mImage = (ID3D11Texture2D*)SimpleInit(0, upscaleType, mQualityLevel, mDisplaySizeX, mDisplaySizeY, false, false, false, false, mSharpening, true, desc.Format);
		logger::info("FSR2 SimpleInit returned output texture address: 0x{:X}", reinterpret_cast<std::uintptr_t>(mOutColor.mImage));
		if (mOutColor.mImage == nullptr) {
			logger::error("FSR2 output texture not available; disabling upscaler before jitter/rendering");
			SetEnabled(false);
			return;
		}
		if (!mTempColor.mImage &&
			(FAILED(mDevice->CreateTexture2D(&desc, nullptr, &mTempColor.mImage)) ||
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
		logger::info("FSR2 render size {}x{} versus output size {}x{}", mRenderSizeX, mRenderSizeY, mDisplaySizeX, mDisplaySizeY);
		if (mRenderSizeX <= 0 || mRenderSizeY <= 0 || mRenderSizeX > 16384 || mRenderSizeY > 16384) {
			logger::error("FSR2 invalid render dimensions, stopping before Jitter / DRS / MotionScale");
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
		logger::info("FSR2 initialization READY; jitter can now use validated render dimensions");
		UnkOuterStruct::GetSingleton()->SetTAA(false);
	} else {
		DRS::GetSingleton()->targetScaleFactor = 1.0f;
		DRS::GetSingleton()->ControlResolution();
		if (mUseOptimalMipLodBias)
			mMipLodBias = 0;
		UnkOuterStruct::GetSingleton()->SetTAA(mEnableUpscaler);
	}
}
