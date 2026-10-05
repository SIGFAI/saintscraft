// SRTTR.exe imports DirectInput8Create from dinput8.dll, so Windows loads this proxy from the game
// folder first. It forwards that call to the real system DLL and starts the link.
#include "compositor.h"
#include "devchan.h"
#include "ground.h"
#include "input.h"
#include "link.h"
#include "log.h"
#include "lua.h"
#include "luadev.h"
#include "mainloop.h"
#include "sr3.h"

#include <MinHook.h>
#include <windows.h>

#include <mutex>

namespace
{
	using DI8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);

	DI8CreateFn RealCreate()
	{
		static DI8CreateFn fn = [] {
			wchar_t path[MAX_PATH];
			GetSystemDirectoryW(path, MAX_PATH);
			lstrcatW(path, L"\\dinput8.dll");
			HMODULE real = LoadLibraryW(path);
			return real ? reinterpret_cast<DI8CreateFn>(GetProcAddress(real, "DirectInput8Create")) : nullptr;
		}();
		return fn;
	}

	// DirectInput8Create can come before Start has set MinHook up: kept until then.
	std::mutex g_diMutex;
	void* g_di8;
	bool g_started;

	DWORD WINAPI Start(LPVOID)
	{
		using namespace mcsr3;
		Log("mcsr3 link starting");
		if (MH_Initialize() != MH_OK)
		{
			Log("MinHook init failed");
			return 1;
		}
		if (lua::Init())
		{
			luadev::Install();
			devchan::Start();
			mainloop::OnTick(lua::Tick);
		}
		if (sr3::Init())
		{
			link::Start();
			ground::Install();
			mainloop::OnPump(sr3::UnhideForUpdate);
			mainloop::OnTick(link::Tick);
		}
		mainloop::Install();
		compositor::Install();
		input::Install();
		{
			std::lock_guard lock(g_diMutex);
			g_started = true;
			input::OnDirectInput8(g_di8);
		}
		Log("mcsr3 link ready");
		return 0;
	}
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, void* outer)
{
	DI8CreateFn real = RealCreate();
	HRESULT hr = real ? real(inst, ver, riid, out, outer) : E_FAIL;
	if (SUCCEEDED(hr) && out)
	{
		std::lock_guard lock(g_diMutex);
		g_di8 = *out;
		if (g_started)
			mcsr3::input::OnDirectInput8(g_di8);
	}
	return hr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		// The image is fully mapped by now; scanning and hooking happen off the loader lock.
		CloseHandle(CreateThread(nullptr, 0, Start, nullptr, 0, nullptr));
	}
	return TRUE;
}
