#include "DRS.h"

#include <SkyrimUpscaler.h>
#include <RE/S/State.h>

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
    auto* u=SkyrimUpscaler::GetSingleton();
    ControlResolution();
    const bool active=u->IsEnabled() && u->mDisplaySizeX>0 && u->mDisplaySizeY>0;
    const float x=active?float(u->mRenderSizeX)/u->mDisplaySizeX:1.0f;
    const float y=active?float(u->mRenderSizeY)/u->mDisplaySizeY:1.0f;
    auto* state=reinterpret_cast<RE::BSGraphics::State*>(a_state);
    auto& r=state->GetRuntimeData();
    // CommonLib's versioned accessor uses State+0x70 on 1.7.104.
    // The legacy custom layout used +0x60 and overwrote Halton entries.
    static bool layoutLogged=false;
    if(!layoutLogged){
        layoutLogged=true;
        const auto off=reinterpret_cast<std::uintptr_t>(&r)-reinterpret_cast<std::uintptr_t>(state);
        logger::info("Build20 State runtime offset=0x{:X}",off);
    }
    const auto off=reinterpret_cast<std::uintptr_t>(&r)-reinterpret_cast<std::uintptr_t>(state);
    if(off!=0x70){u->mNativeFailed=true;u->mUpscalerReady=false;u->SetEnabled(false);return;}
    static float previousX=1.0f,previousY=1.0f;
    static bool wasActive=false;
    static std::int32_t savedLock=0;
    if(active&&!wasActive)savedLock=r.dynamicResolutionLock;
    if(active)r.dynamicResolutionLock=(u->mUpscaleType==DLSS)?0:1;
    else if(wasActive)r.dynamicResolutionLock=savedLock;
    r.dynamicResolutionPreviousWidthRatio=previousX;
    r.dynamicResolutionPreviousHeightRatio=previousY;
    r.dynamicResolutionWidthRatio=x;
    r.dynamicResolutionHeightRatio=y;
    if(previousX!=x||previousY!=y)logger::info("Build20 engine DRS: {}x{} -> {}x{}",previousX,previousY,x,y);
    previousX=x;previousY=y;lastScaleFactor=x;currentScaleFactor=x;wasActive=active;
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
            SkyrimUpscaler::GetSingleton()->ResetNativeHistory();
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
