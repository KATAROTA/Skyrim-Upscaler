
/***************SKSE PLUGIN***************/

#include "Plugin.h"
#include <SKSE/API.h>
#include <DRS.h>
#include <SkyrimUpscaler.h>

#include <array>
#include <cstdint>
#include <cstring>


static void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		if (!REL::Module::IsVR()) {
			RE::BSInputDeviceManager::GetSingleton()->AddEventSink(InputListener::GetSingleton());
			logger::info("Input listener registered");
		}
		break;
	}
	DRS::GetSingleton()->MessageHandler(a_msg);
	SkyrimUpscaler::GetSingleton()->MessageHandler(a_msg);
}


namespace
{
    template <std::size_t N>
    bool VerifyHookBytes(
        const char* name,
        std::uintptr_t address,
        const std::array<std::uint8_t, N>& expected)
    {
        if (address == 0 ||
            std::memcmp(
                reinterpret_cast<const void*>(address),
                expected.data(),
                expected.size()) != 0) {
            logger::error("Hook verification FAILED: {} at 0x{:X}", name, address);
            return false;
        }

        logger::info("Hook verification OK: {}", name);
        return true;
    }

    bool VerifyRuntimeHooks()
    {
        bool valid = true;

        auto require = [&](const char* name, std::uintptr_t address, auto expected) {
            if (!VerifyHookBytes(name, address, expected)) {
                valid = false;
            }
        };

        require("Renderer Init",
            REL::RelocationID(75595, 77226).address() + 0x2BC,
            std::array<std::uint8_t, 5>{0xE8, 0x9F, 0x2D, 0x02, 0x00});

        require("UpdateJitter",
            REL::RelocationID(75460, 77245).address() + 0x133,
            std::array<std::uint8_t, 5>{0xE8, 0xF8, 0x44, 0x01, 0x00});

        require("Jitter NOP",
            REL::RelocationID(75709, 77518).address() + 0x11,
            std::array<std::uint8_t, 6>{0x80, 0x7A, 0x18, 0x00, 0x74, 0x72});

        require("Camera NOP",
            REL::RelocationID(75711, 77520).address() + 0x1D6,
            std::array<std::uint8_t, 10>{
                0x80, 0x79, 0x18, 0x00, 0x0F,
                0x84, 0xD4, 0x00, 0x00, 0x00
            });

        require("MainDraw",
            REL::RelocationID(79947, 82084).address() + 0x17A,
            std::array<std::uint8_t, 5>{0xE8, 0x51, 0xF2, 0xE9, 0xFF});

        require("DRS",
            REL::RelocationID(35556, 36555).address() + 0x2D,
            std::array<std::uint8_t, 5>{0xE8, 0x4E, 0x7B, 0x9C, 0x00});

        return valid;
    }
}

void Init()
{
	SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);
	SKSE::AllocTrampoline(256);

	DRS::InstallHooks();
	InstallUpscalerHooks();
}

void InitializeLog()
{
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		util::report_and_fail("Failed to find standard logging directory"sv);
	}

	*path /= std::format("{}.log"sv, Plugin::NAME);
	auto       sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

#ifndef NDEBUG
	const auto level = spdlog::level::trace;
#else
	const auto level = spdlog::level::info;
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
	log->set_level(level);
	log->flush_on(level);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%l] %v"s);
}

std::string GetLibraryPath(std::string filename)
{
	char    path[MAX_PATH];
	HMODULE hm = NULL;

	if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
							  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&Init, &hm) == 0) {
		int ret = GetLastError();
		fprintf(stderr, "GetModuleHandle failed, error = %d\n", ret);
		// Return or however you want to handle an error.
	}
	if (GetModuleFileNameA(hm, path, sizeof(path)) == 0) {
		int ret = GetLastError();
		fprintf(stderr, "GetModuleFileName failed, error = %d\n", ret);
		// Return or however you want to handle an error.
	}

	std::string::size_type pos = std::string(path).find_last_of("\\/");
	return (std::string(path).substr(0, pos) + "/../../UpscalerBasePlugin/" + filename);
}

EXTERN_C [[maybe_unused]] __declspec(dllexport) bool __cdecl SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
#ifndef NDEBUG
	while (!IsDebuggerPresent()) {};
#endif

	InitializeLog();
	LoadLibrary(GetLibraryPath("nvngx_dlss.dll").c_str());
	LoadLibrary(GetLibraryPath("ffx_fsr2_api_x64.dll").c_str());
	LoadLibrary(GetLibraryPath("ffx_fsr2_api_dx12_x64.dll").c_str());
	LoadLibrary(GetLibraryPath("dxil.dll").c_str());
	LoadLibrary(GetLibraryPath("dxcompiler.dll").c_str());
	LoadLibrary(GetLibraryPath("XeFX.dll").c_str());
	LoadLibrary(GetLibraryPath("XeFX_Loader.dll").c_str());
	LoadLibrary(GetLibraryPath("libxess.dll").c_str());
	LoadLibrary(GetLibraryPath("PDPerfPlugin.dll").c_str());
	logger::info("Loaded plugin");

	SKSE::Init(a_skse);

	if (REL::Module::get().version() != REL::Version{ 1, 7, 104, 0 }) {
		logger::error("Unsupported Skyrim runtime; expected 1.7.104.0");
		return false;
	}

	if (!VerifyRuntimeHooks()) {
		logger::error("Hook verification failed; refusing to load");
		return false;
	}

	Init();

	return true;
}

EXTERN_C [[maybe_unused]] __declspec(dllexport) constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData v;
	v.PluginName(Plugin::NAME);
	v.PluginVersion(Plugin::VERSION);
	v.UsesAddressLibrary();
	v.UsesNoStructs();
	return v;
}();

EXTERN_C [[maybe_unused]] __declspec(dllexport) bool __cdecl SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* pluginInfo)
{
	pluginInfo->name = SKSEPlugin_Version.pluginName;
	pluginInfo->infoVersion = SKSE::PluginInfo::kVersion;
	pluginInfo->version = SKSEPlugin_Version.pluginVersion;
	return true;
}
