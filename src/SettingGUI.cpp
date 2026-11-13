#include "SettingGUI.h"
#include <SkyrimUpscaler.h>

LRESULT WndProcHook::thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	auto& io = ImGui::GetIO();
	if (uMsg == WM_KILLFOCUS) {
		// ClearInputKeys also clears input characters.
		io.ClearInputKeys();
	}
	return func(hWnd, uMsg, wParam, lParam);
}
static inline const char* format_to_string(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R16G16_TYPELESS:
		return "R16G16 Typeless";
	case DXGI_FORMAT_R16G16_UINT:
		return "R16G16 UInt";
	case DXGI_FORMAT_R16G16_SINT:
		return "R16G16 SInt";
	case DXGI_FORMAT_R16G16_FLOAT:
		return "R16G16 Float";
	case DXGI_FORMAT_R16G16_UNORM:
		return "R16G16 UNorm";
	case DXGI_FORMAT_R16G16_SNORM:
		return "R16G16 SNorm";
	default:
		return "     ";
	}
}

void ProcessEvent(ImGuiKey key);

void SettingGUI::InitIMGUI(IDXGISwapChain* swapchain, ID3D11Device* device, ID3D11DeviceContext* context)
{
	mSwapChain = swapchain;
	mDevice = device;
	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	auto&    imgui_io = ImGui::GetIO();

	imgui_io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard;
	imgui_io.BackendFlags = ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_RendererHasVtxOffset;

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsLight();

	DXGI_SWAP_CHAIN_DESC desc;
	swapchain->GetDesc(&desc);

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(desc.OutputWindow);
	ImGui_ImplDX11_Init(device, context);

	WndProcHook::func = reinterpret_cast<WNDPROC>(
		SetWindowLongPtr(
			desc.OutputWindow,
			GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(WndProcHook::thunk)));
	if (!WndProcHook::func)
		logger::error("SetWindowLongPtrA failed!");
	else
		logger::info("SettingGUI::InitIMGUI Success!");
}

void SettingGUI::OnRender()
{
	if (REL::Module::IsVR()) {
		ProcessEvent(static_cast<ImGuiKey>(mToggleHotkey));
	}
	
	auto&  io = ImGui::GetIO();
	io.MouseDrawCursor = mShowGUI;

	// Start the Dear ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	if (ImGui::IsKeyReleased(static_cast<ImGuiKey>(mToggleHotkey)))
		toggle();

	if (!REL::Module::IsVR()) {
		static bool lastShowGUI = false;
		if (mShowGUI != lastShowGUI) {
			auto controlMap = RE::ControlMap::GetSingleton();
			if (controlMap)
				controlMap->ignoreKeyboardMouse = mShowGUI;
			lastShowGUI = mShowGUI;
		}
	}

    if (mShowGUI) {
        auto* u=SkyrimUpscaler::GetSingleton();
        ImGui::Begin("Skyrim Upscaler - MetalFX", &mShowGUI, ImGuiWindowFlags_NoCollapse);
        if(u->mNativeFailed) ImGui::TextWrapped("MetalFX unavailable. Vanilla rendering is active. See SkyrimUpscaler.log.");
        else if(!u->mUpscalerReady) ImGui::TextWrapped("Waiting for a loaded world and validated engine textures.");
        else ImGui::TextUnformatted(u->IsEnabled()?"MetalFX Temporal active":"Native rendering (MetalFX off)");
        ImGui::BeginDisabled(!u->mUpscalerReady || u->mNativeFailed);
        bool enabled=u->mEnableUpscaler;
        if(ImGui::Checkbox("Enable MetalFX", &enabled)){u->SetEnabled(enabled);u->SaveINI();}
        int mode=u->mUpscaleType==DLAA?1:0;
        if(ImGui::Combo("Upscale type", &mode, "DLSS (MetalFX)\0DLAA (MetalFX Native AA)\0")) {
            if(u->ConfigureNativeMode(mode?DLAA:DLSS,u->mQualityLevel))u->SaveINI();
        }
        ImGui::BeginDisabled(u->mUpscaleType==DLAA);
        int quality=u->mQualityLevel;
        if(ImGui::Combo("Quality level",&quality,"Performance\0Balanced\0Quality\0")) {
            if(u->ConfigureNativeMode(u->mUpscaleType,quality))u->SaveINI();
        }
        ImGui::EndDisabled();
        if(ImGui::Checkbox("Sharpening",&u->mSharpening))u->SaveINI();
        ImGui::BeginDisabled(!u->mSharpening);
        ImGui::SliderFloat("Sharpness",&u->mSharpness,0.0f,1.0f,"%.2f");
        if(ImGui::IsItemDeactivatedAfterEdit())u->SaveINI();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::Text("Render: %d x %d",u->mRenderSizeX,u->mRenderSizeY);
        ImGui::Text("Output: %d x %d",u->mDisplaySizeX,u->mDisplaySizeY);
        ImGui::TextUnformatted("Build 22 | DXMT native temporal scaler");
        ImGui::End();
    }

	// Rendering
	ImGui::Render();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void SettingGUI::OnCleanup()
{
	// Cleanup
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

// Codes below are from CatHub
// https://github.com/Pentalimbed/cathub
class CharEvent : public RE::InputEvent
{
public:
	uint32_t keyCode;  // 18 (ascii code)
};

RE::BSEventNotifyControl InputListener::ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_eventSource)
{
	if (!a_event || !a_eventSource)
		return RE::BSEventNotifyControl::kContinue;

	auto& io = ImGui::GetIO();

	for (auto event = *a_event; event; event = event->next) {
		if (event->eventType == RE::INPUT_EVENT_TYPE::kChar) {
			io.AddInputCharacter(static_cast<CharEvent*>(event)->keyCode);
		} else if (event->eventType == RE::INPUT_EVENT_TYPE::kButton) {
			const auto button = static_cast<RE::ButtonEvent*>(event);
			if (!button || (button->IsPressed() && !button->IsDown()))
				continue;

			auto     scan_code = button->GetIDCode();
			uint32_t key = MapVirtualKeyEx(scan_code, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
			switch (scan_code) {
                case DIK_LEFTARROW: key = VK_LEFT; break;
                case DIK_RIGHTARROW: key = VK_RIGHT; break;
                case DIK_UPARROW: key = VK_UP; break;
                case DIK_DOWNARROW: key = VK_DOWN; break;
                case DIK_DELETE: key = VK_DELETE; break;
                case DIK_END: key = VK_END; break;
                case DIK_HOME: key = VK_HOME; break;   // pos1
                case DIK_PRIOR: key = VK_PRIOR; break; // page up
                case DIK_NEXT: key = VK_NEXT; break;   // page down
                case DIK_INSERT: key = VK_INSERT; break;
                case DIK_NUMPAD0: key = VK_NUMPAD0; break;
                case DIK_NUMPAD1: key = VK_NUMPAD1; break;
                case DIK_NUMPAD2: key = VK_NUMPAD2; break;
                case DIK_NUMPAD3: key = VK_NUMPAD3; break;
                case DIK_NUMPAD4: key = VK_NUMPAD4; break;
                case DIK_NUMPAD5: key = VK_NUMPAD5; break;
                case DIK_NUMPAD6: key = VK_NUMPAD6; break;
                case DIK_NUMPAD7: key = VK_NUMPAD7; break;
                case DIK_NUMPAD8: key = VK_NUMPAD8; break;
                case DIK_NUMPAD9: key = VK_NUMPAD9; break;
                case DIK_DECIMAL: key = VK_DECIMAL; break;
                case DIK_NUMPADENTER: key = IM_VK_KEYPAD_ENTER; break;
                case DIK_RMENU: key = VK_RMENU; break;       // right alt
                case DIK_RCONTROL: key = VK_RCONTROL; break; // right control
                case DIK_LWIN: key = VK_LWIN; break;         // left win
                case DIK_RWIN: key = VK_RWIN; break;         // right win
                case DIK_APPS: key = VK_APPS; break;
                default: break;
			}

			switch (button->device.get()) {
			case RE::INPUT_DEVICE::kMouse:
				if (scan_code > 7)  // middle scroll
					io.AddMouseWheelEvent(0, button->Value() * (scan_code == 8 ? 1 : -1));
				else {
					if (scan_code > 5)
						scan_code = 5;
					io.AddMouseButtonEvent(scan_code, button->IsPressed());
				}
				break;
			case RE::INPUT_DEVICE::kKeyboard:
				io.AddKeyEvent(VirtualKeyToImGuiKey(key), button->IsPressed());
				break;
			case RE::INPUT_DEVICE::kGamepad:
				// not implemented yet
				// key = GetGamepadIndex((RE::BSWin32GamepadDevice::Key)key);
				break;
			default:
				continue;
			}
		}
	}

	return RE::BSEventNotifyControl::kContinue;
}

void ProcessEvent(ImGuiKey key)
{
	auto& io = ImGui::GetIO();
	static bool leftButton = false;
	if (GetAsyncKeyState(VK_LBUTTON) < 0 && leftButton == false) {
		leftButton = true;
		io.AddMouseButtonEvent(0, leftButton);
	}
	if (GetAsyncKeyState(VK_LBUTTON) == 0 && leftButton == true) {
		leftButton = false;
		io.AddMouseButtonEvent(0, leftButton);
	}

	auto vkey = ImGuiKeyToVirtualKey(key);

	static bool pressed = false;
	if (GetAsyncKeyState(vkey) < 0 && pressed == false) {
		pressed = true;
	}
	if (GetAsyncKeyState(vkey) == 0 && pressed == true) {
		pressed = false;
		SettingGUI::GetSingleton()->toggle();
	}
}
