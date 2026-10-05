// Hardware write watchpoint for reverse engineering (dev channel: mc_hwwatch(addr) / mc_hwwatch(0)).
// DR0 on every thread; each distinct writing instruction is logged once with its SRTTR call stack.
#include "hwwatch.h"

#include "log.h"
#include "scan.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace mcsr3::hwwatch
{
	namespace
	{
		std::mutex g_mutex;
		std::set<std::uintptr_t> g_seen;
		std::atomic<int> g_hits;
		PVOID g_veh;

		bool InImage(std::uintptr_t a)
		{
			auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(scan::ImageBase());
			auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(scan::ImageBase() + dos->e_lfanew);
			return a >= scan::ImageBase() && a < scan::ImageBase() + nt->OptionalHeader.SizeOfImage;
		}

		LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
		{
			if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1))
				return EXCEPTION_CONTINUE_SEARCH;
			ep->ContextRecord->Dr6 = 0;
			auto rip = reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
			g_hits++;
			std::lock_guard lock(g_mutex);
			if (g_seen.size() < 32 && g_seen.insert(rip).second)
			{
				// Exact call stack from the x64 unwind tables.
				char stack[512] = {};
				int len = 0;
				CONTEXT c = *ep->ContextRecord;
				for (int i = 0; i < 12 && c.Rip; i++)
				{
					DWORD64 imageBase = 0;
					PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
					if (!fn)
						break;
					PVOID handlerData;
					DWORD64 frame;
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &frame, nullptr);
					if (InImage(c.Rip))
						len += std::snprintf(stack + len, sizeof(stack) - len, " %llx", (unsigned long long)(c.Rip - scan::ImageBase()));
				}
				Log("hwwatch: write by SRTTR+%llx (thread %lu) stack:%s", (unsigned long long)(rip - scan::ImageBase()), GetCurrentThreadId(), stack);
			}
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		void SetAll(std::uintptr_t addr)
		{
			DWORD self = GetCurrentThreadId();
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			THREADENTRY32 te{ sizeof(te) };
			for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
			{
				if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self)
					continue;
				HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
				if (!t)
					continue;
				SuspendThread(t);
				CONTEXT c{};
				c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(t, &c))
				{
					c.Dr0 = addr;
					// L0 enable, R/W0 = 01 (write), LEN0 = 11 (4 bytes)
					c.Dr7 = addr ? (c.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18) : c.Dr7 & ~0xF0003ull;
					SetThreadContext(t, &c);
				}
				ResumeThread(t);
				CloseHandle(t);
			}
			CloseHandle(snap);
		}
	}

	void Set(std::uintptr_t addr)
	{
		if (!g_veh)
			g_veh = AddVectoredExceptionHandler(1, Handler);
		{
			std::lock_guard lock(g_mutex);
			g_seen.clear();
		}
		g_hits = 0;
		// From a helper thread, so the calling (game) thread gets its registers set too.
		std::thread(SetAll, addr).join();
		Log("hwwatch: %s %llx", addr ? "watching" : "cleared", (unsigned long long)addr);
	}

	int Hits() { return g_hits; }
}
