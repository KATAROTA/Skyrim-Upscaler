#pragma once
#include <d3d11.h>
#include <dxgi.h>

#include <Detours.h>
#include "SkyrimUpscaler.h"
#include "SettingGUI.h"

#include <Detours.h>
#include "DRS.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <RE/S/State.h>

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChain;
decltype(&IDXGISwapChain::Present) ptrPresent;
decltype(&ID3D11DeviceContext::RSSetViewports) ptrRSSetViewports;

void WINAPI hk_ID3D11DeviceContext_RSSetViewports(ID3D11DeviceContext* This, UINT count, const D3D11_VIEWPORT* v)
{
    auto* u=SkyrimUpscaler::GetSingleton();
    if(v && count && u->mUpscalerReady && u->mEnableUpscaler && u->mUpscaleType==DLSS &&
        !u->mNativeViewportSeen.load(std::memory_order_relaxed)) {
        for(UINT i=0;i<count;++i) {
            if(std::fabs(v[i].Width-float(u->mRenderSizeX))<=2.0f &&
               std::fabs(v[i].Height-float(u->mRenderSizeY))<=2.0f &&
               std::fabs(v[i].TopLeftX)<1.0f && std::fabs(v[i].TopLeftY)<1.0f) {
                u->mNativeViewportSeen.store(true,std::memory_order_relaxed);
                logger::info("Build20 reduced raster verified: {}x{}",v[i].Width,v[i].Height);
                break;
            }
        }
    }
    (This->*ptrRSSetViewports)(count,v);
}

HRESULT WINAPI hk_IDXGISwapChain_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
    SettingGUI::GetSingleton()->OnRender();
    auto hr=(This->*ptrPresent)(SyncInterval,Flags);
    DRS::GetSingleton()->Update();
    SkyrimUpscaler::GetSingleton()->mBuild19EarlyAppliedThisFrame=false;
    return hr;
}

HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChain(
	IDXGIAdapter*               pAdapter,
	D3D_DRIVER_TYPE             DriverType,
	HMODULE                     Software,
	UINT                        Flags,
	const D3D_FEATURE_LEVEL*    pFeatureLevels,
	UINT                        FeatureLevels,
	UINT                        SDKVersion,
	const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain**            ppSwapChain,
	ID3D11Device**              ppDevice,
	D3D_FEATURE_LEVEL*          pFeatureLevel,
	ID3D11DeviceContext**       ppImmediateContext)
{
	if (pSwapChainDesc == nullptr)
		return E_INVALIDARG;
	DXGI_SWAP_CHAIN_DESC sDesc;
	if (REL::Module::IsVR()) {
		memcpy(&sDesc, pSwapChainDesc, sizeof(DXGI_SWAP_CHAIN_DESC));
		sDesc.BufferDesc.Width = 1024;
		sDesc.BufferDesc.Height = 1024;
	}
	else
		sDesc = *pSwapChainDesc;

	logger::info("Calling original D3D11CreateDeviceAndSwapChain");
	HRESULT hr = (*ptrD3D11CreateDeviceAndSwapChain)(pAdapter,
		DriverType,
		Software,
		Flags,
		pFeatureLevels,
		FeatureLevels,
		SDKVersion,
		&sDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);

	if (FAILED(hr) || ppSwapChain == nullptr || ppDevice == nullptr ||
		ppImmediateContext == nullptr || *ppSwapChain == nullptr ||
		*ppDevice == nullptr || *ppImmediateContext == nullptr) {
		logger::error("D3D11CreateDeviceAndSwapChain failed: HRESULT 0x{:08X}", static_cast<unsigned int>(hr));
		return FAILED(hr) ? hr : E_FAIL;
	}

	auto device = *ppDevice;
	auto deviceContext = *ppImmediateContext;
	auto swapChain = *ppSwapChain;
	auto* upscaler = SkyrimUpscaler::GetSingleton();
	upscaler->SetupSwapChain(swapChain);
	if (upscaler->mDevice == nullptr || upscaler->mContext == nullptr) {
		logger::error("Could not initialize the Direct3D 11 device/context");
		return E_FAIL;
	}
	upscaler->PreInit();
	SettingGUI::GetSingleton()->InitIMGUI(swapChain, device, deviceContext);
	logger::info("Detouring virtual function tables");
	*(uintptr_t*)&ptrPresent = Detours::X64::DetourClassVTable(*(uintptr_t*)swapChain, &hk_IDXGISwapChain_Present, 8);
    // ID3D11DeviceContext vtable slots 44/45: observation only, no viewport mutations.
    *(uintptr_t*)&ptrRSSetViewports = Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_RSSetViewports, 44);

	return hr;
}

struct UpscalerHooks
{

	struct BSGraphics_Renderer_Init_InitD3D
	{
		static void thunk()
		{
			func();
			if (!REL::Module::IsVR())
				MenuOpenCloseEventHandler::Register();
			SkyrimUpscaler::GetSingleton()->InitUpscaler();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

    struct Main_DrawWorld_MainDraw
    {
        static void thunk(INT64 renderer, int unk)
        {
            func(renderer, unk);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // Native HDR reconstruction precedes Bethesda's ImageSpace and UI passes.
    struct Main_PostProcessing_Build19
    {
        static void thunk(RE::ImageSpaceManager* a_this, std::uint32_t a3,
            RE::RENDER_TARGET a_target, void* a4, bool a5)
        {
            auto* u=SkyrimUpscaler::GetSingleton();
            if(!u->mBuild19EarlyAppliedThisFrame && u->EvaluateBeforePostProcessing()) {
                u->mBuild19EarlyAppliedThisFrame=true;
                auto* state=RE::BSGraphics::State::GetSingleton();
                if(state) {
                    auto& r=state->GetRuntimeData();
                    r.dynamicResolutionWidthRatio=1.0f;
                    r.dynamicResolutionHeightRatio=1.0f;
                    r.dynamicResolutionPreviousWidthRatio=1.0f;
                    r.dynamicResolutionPreviousHeightRatio=1.0f;
                    r.dynamicResolutionLock=1;
                    // Keep full-size ratios through the rest of this frame, including UI.
                    // The next DRS hook restores the selected world render ratios.
                }
            }
            func(a_this,a3,a_target,a4,a5);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

	struct BSImagespaceShader_Hook_VR
	{
		static void thunk(RE::BSImagespaceShader* param_1, uint64_t param_2)
		{
			static bool initTAA = false;
			if (!initTAA) {
				initTAA = true;
				UnkOuterStruct::GetSingleton()->SetTAA(SkyrimUpscaler::GetSingleton()->mUpscaleType == TAA);
				if (SkyrimUpscaler::GetSingleton()->mTargetTex.mImage == nullptr) {
					ID3D11RenderTargetView* RTV;
					ID3D11DepthStencilView* DSV;
					SkyrimUpscaler::GetSingleton()->mContext->OMGetRenderTargets(1, &RTV, &DSV);
					ID3D11Resource* renderTarget;
					RTV->GetResource(&renderTarget);
					ID3D11Texture2D* targetTex;
					renderTarget->QueryInterface(IID_PPV_ARGS(&targetTex));
					SkyrimUpscaler::GetSingleton()->SetupTarget(targetTex);
				}
			}
			SkyrimUpscaler::GetSingleton()->EvaluateUpscaler();
			func(param_1, param_2);
			static ID3D11Resource*         TargetTex = nullptr;
			static ID3D11Resource*         DepthTex = nullptr;
			if (TargetTex == nullptr) {
				ID3D11RenderTargetView* RTV = nullptr;
				ID3D11DepthStencilView* DSV = nullptr;
				SkyrimUpscaler::GetSingleton()->mContext->OMGetRenderTargets(1, &RTV, &DSV);
				if (RTV != nullptr)
					RTV->GetResource(&TargetTex);
				if (DSV != nullptr)
					DSV->GetResource(&DepthTex);
			}
			if (SkyrimUpscaler::GetSingleton()->IsEnabled()) {
				SkyrimUpscaler::GetSingleton()->mContext->CopyResource(TargetTex, SkyrimUpscaler::GetSingleton()->mTargetTex.mImage);
				SkyrimUpscaler::GetSingleton()->mContext->CopyResource(SkyrimUpscaler::GetSingleton()->mDepthBuffer.mImage, DepthTex);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSGraphics_Renderer_Begin_UpdateJitter
	{
		static void thunk(BSGraphics::State* a_state)
		{
			auto* upscaler = SkyrimUpscaler::GetSingleton();
            upscaler->UpdateNativeTAA();
            func(a_state);
			const float width = static_cast<float>(upscaler->mRenderSizeX);
			const float height = static_cast<float>(upscaler->mRenderSizeY);
			if (upscaler->IsEnabled() && upscaler->mEnableJitter && !upscaler->mDisableEvaluation && upscaler->mUpscaleType != TAA) {
				if (!(width > 0.0f && height > 0.0f && width <= 16384.0f && height <= 16384.0f)) {
					static bool warnedSize = false;
					if (!warnedSize) {
						logger::error("Jitter cancelled: invalid render size {}x{}", upscaler->mRenderSizeX, upscaler->mRenderSizeY);
						warnedSize = true;
					}
					reinterpret_cast<RE::BSGraphics::State*>(a_state)->projectionPosScaleX = 0.0f;
					reinterpret_cast<RE::BSGraphics::State*>(a_state)->projectionPosScaleY = 0.0f;
					upscaler->SetJitterOffsets(0.0f, 0.0f);
					return;
				}
				float x = 0.0f;
				float y = 0.0f;
				upscaler->GetJitters(&x, &y);
				const float jitterX = -2.0f * x / width;
				const float jitterY = 2.0f * y / height;
				if (!std::isfinite(x) || !std::isfinite(y) ||
					!std::isfinite(jitterX) || !std::isfinite(jitterY)) {
					static bool warnedFinite = false;
					if (!warnedFinite) {
						logger::error("Jitter cancelled: nonfinite offsets x={} y={} render={}x{}", x, y, width, height);
						warnedFinite = true;
					}
					reinterpret_cast<RE::BSGraphics::State*>(a_state)->projectionPosScaleX = 0.0f;
					reinterpret_cast<RE::BSGraphics::State*>(a_state)->projectionPosScaleY = 0.0f;
					upscaler->SetJitterOffsets(0.0f, 0.0f);
					return;
				}
				static int diagnosticFrames = 0;
				if (diagnosticFrames++ < 8) {
					const auto offset = reinterpret_cast<std::uintptr_t>(&reinterpret_cast<RE::BSGraphics::State*>(a_state)->projectionPosScaleX) - reinterpret_cast<std::uintptr_t>(a_state);
					logger::info("Jitter frame {}: size={}x{}, raw=({}, {}), normalized=({}, {}), assumedStateOffset=0x{:X}",
						 diagnosticFrames, width, height, x, y, jitterX, jitterY, offset);
				}
				reinterpret_cast<RE::BSGraphics::State*>(a_state)->projectionPosScaleX = jitterX;
				reinterpret_cast<RE::BSGraphics::State*>(a_state)->projectionPosScaleY = jitterY;
				upscaler->SetJitterOffsets(-x, -y);
			} else {
				if(upscaler->IsEnabled()){reinterpret_cast<RE::BSGraphics::State*>(a_state)->projectionPosScaleX=0.0f;reinterpret_cast<RE::BSGraphics::State*>(a_state)->projectionPosScaleY=0.0f;}
				upscaler->SetJitterOffsets(0.0f, 0.0f);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		// Validate Direct3D entry points before modifying the Skyrim executable.
		auto dllD3D11 = GetModuleHandleA("d3d11.dll");
		if (dllD3D11 == nullptr) {
			logger::error("Cannot install upscaler hooks: d3d11.dll is not loaded");
			return;
		}
		auto createDevice = GetProcAddress(dllD3D11, "D3D11CreateDeviceAndSwapChain");
		if (createDevice == nullptr) {
			logger::error("Cannot install upscaler hooks: D3D11CreateDeviceAndSwapChain not found");
			return;
		}
		// Hook for getting the swapchain.
		stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));
		// Have to hook the creation of SwapChain to hook CreateTexture2D before depth and motion textures are created
		char* ptr = nullptr;
		auto  moduleBase = (uintptr_t)GetModuleHandle(ptr);
		*(FARPROC*)&ptrD3D11CreateDeviceAndSwapChain = createDevice;
		Detours::IATHook(moduleBase, "d3d11.dll", "D3D11CreateDeviceAndSwapChain", (uintptr_t)hk_D3D11CreateDeviceAndSwapChain);
		 
		// Setup our own jitters
		stl::write_thunk_call<BSGraphics_Renderer_Begin_UpdateJitter>(REL::RelocationID(75460, 77245).address() + REL::Relocate(0xE5, 0x133, 0x104));  // D6A0C0 (D6A1A5), DA5A00 (DA5AE2)
		// Always enable TAA jitters, even without TAA
		static REL::Relocation<uintptr_t> updateJitterHook{ REL::RelocationID(75709, 77518) };          // D7CFB0, DB96E0
		static REL::Relocation<uintptr_t> buildCameraStateDataHook{ REL::RelocationID(75711, 77520) };  // D7D130, DB9850
		uint8_t                           patch1[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		uint8_t                           patch2[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		uint8_t                           patch3[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		if (!REL::Module::IsVR()) {
			REL::safe_write<uint8_t>(updateJitterHook.address() + REL::Relocate(0xE, 0x11), patch1);
			REL::safe_write<uint8_t>(buildCameraStateDataHook.address() + REL::Relocate(0x1D5, 0x1D6), patch2);
			// Pre-UI Hook for upscaling, only for flatrim
			stl::write_thunk_call<Main_DrawWorld_MainDraw>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x16F, 0x17A, 0x132));  // EBF510 (EBF67F), F05BF0 (F05D6A)

            // Early post-processing hook is optional: skip if the site is patched or
            // does not resolve to an executable function in this exact runtime.
            const auto site = REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7);
            const auto* call = reinterpret_cast<const std::uint8_t*>(site);
            const std::uint8_t expected[5]{0xE8, 0x04, 0x2E, 0xFB, 0xFF};
            bool safeCall = std::memcmp(call, expected, sizeof(expected)) == 0;
            if (safeCall) {
                std::int32_t disp = 0;
                std::memcpy(&disp, call + 1, sizeof(disp));
                const auto target = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(site + 5) + disp);
                MEMORY_BASIC_INFORMATION memory{};
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(target), &memory, sizeof(memory)))
                    safeCall = false;
                else {
                    const DWORD allowed = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                    safeCall = (memory.State == MEM_COMMIT) && (memory.Protect & allowed);
                }
                if (safeCall) {
                    // Reject an already-hooked call to a foreign module/trampoline.
                    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
                    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) {
                        safeCall = false;
                    } else {
                        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
                        safeCall = nt->Signature == IMAGE_NT_SIGNATURE &&
                            target >= module && (target - module) < nt->OptionalHeader.SizeOfImage;
                    }
                }
                if (safeCall) {
                    stl::write_thunk_call<Main_PostProcessing_Build19>(site);
                    SkyrimUpscaler::GetSingleton()->mBuild19PostProcessHookInstalled = true;
                    logger::info("Build19 prepost hook INSTALLED at 0x{:X}, original target=0x{:X}", site, target);
                }
            }
            if (!safeCall)
                logger::warn("Build19 prepost hook SKIPPED: unverified or non-executable call site 0x{:X}; Build17 fallback active", site);

		} else {
			REL::safe_write<uint8_t>(updateJitterHook.address() + 0x17, patch2);
			REL::safe_write<uint8_t>(buildCameraStateDataHook.address() + 0x34, patch3);
			// Pre-UI Hook for upscaling specifically for VR
			stl::write_thunk_call<BSImagespaceShader_Hook_VR>(REL::Offset(0x132c827).address());
		}
		logger::info("Installed upscaler hooks");
	}
};

void InstallUpscalerHooks() {
	UpscalerHooks::Install();
}

