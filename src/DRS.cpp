#include "DRS.h"

#include <SkyrimUpscaler.h>

void DRS::GetGameSettings()
{
	bEnableAutoDynamicResolution = RE::GetINISetting("bEnableAutoDynamicResolution:Display");
	if (!REL::Module::IsVR()) {
		if (bEnableAutoDynamicResolution) {
			if (!bEnableAutoDynamicResolution->GetBool())
				logger::info("Forcing DynamicResolution on.");
			bEnableAutoDynamicResolution->data.b = true;
		} else
			logger::warn("Unable to enable Dynamic Resolution, please enable manually.");
	}
}

void DRS::SetDRSVR(float renderScale)
{
	if (renderScale == 0)
		renderScale = currentScaleFactor;
	auto currentWidthRatio = reinterpret_cast<float*>(REL::Offset(0x3186d14).address());
	auto currentHeightRatio = reinterpret_cast<float*>(REL::Offset(0x3186d18).address());
	auto previousWidthRatio = reinterpret_cast<float*>(REL::Offset(0x3186d1c).address());
	auto previousHeightRatio = reinterpret_cast<float*>(REL::Offset(0x3186d20).address());
	*previousWidthRatio = *currentWidthRatio;
	*previousHeightRatio = *currentHeightRatio;
	*currentWidthRatio = renderScale;
	*currentHeightRatio = renderScale;
}

void DRS::Update()
{
	if (reset) {
		ResetScale();
		return;
	}
	else {
		ControlResolution();
	}
}

void DRS::ControlResolution()
{
	currentScaleFactor = SkyrimUpscaler::GetSingleton()->IsEnabled()?targetScaleFactor:1.0f;
}

void DRS::ResetScale()
{
	currentScaleFactor = 1.0f;
}

void DRS::SetDRS(BSGraphics::State* a_state)
{
    if (REL::Module::IsVR()) {
        SetDRSVR(currentScaleFactor);
    }

    auto& runtimeData = a_state->GetRuntimeData();
    const float oldWidth = runtimeData.fDynamicResolutionCurrentWidthScale;
    const float oldHeight = runtimeData.fDynamicResolutionCurrentHeightScale;
    const float oldRatioX = runtimeData.fDynamicResolutionWidthRatio;
    const float oldRatioY = runtimeData.fDynamicResolutionHeightRatio;

    // Build 16 wrote only CurrentWidth/HeightScale. In Community Shaders,
    // Skyrim's upscaling path additionally writes the runtime Width/HeightRatio
    // and their previous-frame counterparts. Probe these fields ONLY when opted in.
    auto* upscaler = SkyrimUpscaler::GetSingleton();
    const bool experimental = upscaler->mTryEngineDRSRatios &&
        upscaler->mUpscalerReady && upscaler->mUpscaleType == DLSS;
    static bool wasExperimental = false;
    const float scale = (currentScaleFactor >= 0.5f && currentScaleFactor <= 1.0f)
        ? currentScaleFactor : 1.0f;
    const float effectiveRatio = experimental ? scale : 1.0f;

    runtimeData.fDynamicResolutionCurrentWidthScale = scale;
    runtimeData.fDynamicResolutionCurrentHeightScale = scale;

    if (experimental || wasExperimental) {
        // Keep previous ratios synchronized with the values used in the prior frame.
        // When the experiment is switched off, restore 1.0 without restarting Skyrim.
        runtimeData.fDynamicResolutionPreviousWidthScale = oldRatioX;
        runtimeData.fDynamicResolutionPreviousHeightScale = oldRatioY;
        runtimeData.fDynamicResolutionWidthRatio = effectiveRatio;
        runtimeData.fDynamicResolutionHeightRatio = effectiveRatio;
    }

    static int diagnosticFrames = 0;
    static float lastDiagnosticScale = -1.0f;
    static bool lastExperimental = false;
    if (lastDiagnosticScale != scale || lastExperimental != experimental) {
        lastDiagnosticScale = scale;
        lastExperimental = experimental;
        diagnosticFrames = 0;
    }
    if (diagnosticFrames++ < 8) {
        logger::info("Build17 DRS: experimental={} requested={} current {}x{} -> {}x{} ratio {}x{} -> {}x{} previous={}x{} stateOffset=0x{:X}",
            experimental, scale, oldWidth, oldHeight,
            runtimeData.fDynamicResolutionCurrentWidthScale,
            runtimeData.fDynamicResolutionCurrentHeightScale,
            oldRatioX, oldRatioY,
            runtimeData.fDynamicResolutionWidthRatio,
            runtimeData.fDynamicResolutionHeightRatio,
            runtimeData.fDynamicResolutionPreviousWidthScale,
            runtimeData.fDynamicResolutionPreviousHeightScale,
            static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(&runtimeData) - reinterpret_cast<std::uintptr_t>(a_state)));
    }
    wasExperimental = experimental;
    lastScaleFactor = scale;
}

void DRS::MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		GetGameSettings();
		break;
	}
}

// Fader Menu
// Mist Menu
// Loading Menu
// LoadWaitSpinner

RE::BSEventNotifyControl MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event->menuName == RE::MainMenu::MENU_NAME ||
		a_event->menuName == RE::LoadingMenu::MENU_NAME ||
		a_event->menuName == RE::RaceSexMenu::MENU_NAME) {
		if (a_event->opening) {
			DRS::GetSingleton()->reset = true;
			DRS::GetSingleton()->ResetScale();
		} else {
			DRS::GetSingleton()->reset = false;
			DRS::GetSingleton()->ControlResolution();
		}
	}
	else if (a_event->menuName == RE::FaderMenu::MENU_NAME) {
		if (!a_event->opening) {
			DRS::GetSingleton()->reset = false;
			DRS::GetSingleton()->ControlResolution();
		}
	}

	return RE::BSEventNotifyControl::kContinue;
}

bool MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto                             ui = RE::UI::GetSingleton();

	if (!ui) {
		logger::error("UI event source not found");
		return false;
	}

	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

	logger::info("Registered {}", typeid(singleton).name());

	return true;
}
