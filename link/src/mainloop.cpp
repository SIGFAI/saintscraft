#include "mainloop.h"

#include "input.h"
#include "log.h"

#include <MinHook.h>
#include <windows.h>

#include <vector>

namespace mcsr3::mainloop
{
	namespace
	{
		decltype(&PeekMessageA) g_peekOrig;
		std::vector<std::function<void()>> g_ticks, g_pumps;  // only added to before Install
		DWORD g_thread;
		LARGE_INTEGER g_freq, g_last;
		bool g_inTick;

		BOOL WINAPI PeekMessageHook(LPMSG msg, HWND wnd, UINT min, UINT max, UINT remove)
		{
			for (auto& f : g_pumps)
				f();
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			// The pump calls PeekMessage until the queue is empty: once per 4 ms is once per frame.
			if (!g_inTick && now.QuadPart - g_last.QuadPart >= g_freq.QuadPart / 250)
			{
				g_last = now;
				g_thread = GetCurrentThreadId();
				g_inTick = true;
				for (auto& f : g_ticks)
					f();
				g_inTick = false;
			}
			BOOL got = g_peekOrig(msg, wnd, min, max, remove);
			if (got && msg && (remove & PM_REMOVE))
				input::FilterMessage(msg);
			return got;
		}
	}

	void OnTick(std::function<void()> fn) { g_ticks.push_back(std::move(fn)); }
	void OnPump(std::function<void()> fn) { g_pumps.push_back(std::move(fn)); }
	unsigned long ThreadId() { return g_thread; }

	bool Install()
	{
		QueryPerformanceFrequency(&g_freq);
		if (MH_CreateHook((void*)&PeekMessageA, (void*)PeekMessageHook, (void**)&g_peekOrig) != MH_OK || MH_EnableHook((void*)&PeekMessageA) != MH_OK)
		{
			Log("mainloop: hooking PeekMessageA failed");
			return false;
		}
		return true;
	}
}
