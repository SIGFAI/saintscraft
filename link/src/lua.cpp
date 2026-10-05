#include "lua.h"

#include "log.h"
#include "scan.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

// The game links Lua 5.1.4 statically, with lua_Number = double (lua_pushnumber stores with movsd).
// Signatures for the API come from SaintExec (Nathnefo/SaintExec, for SRIV Re-Elected: the same
// engine), and match SRTTR uniquely.
namespace mcsr3::lua
{
	namespace
	{
		constexpr int kGlobalsIndex = -10002;  // LUA_GLOBALSINDEX
		constexpr int kTNil = 0, kTBoolean = 1, kTNumber = 3;

		struct TValue
		{
			union { double n; int b; void* p; } value;
			int tt;
			int pad;
		};
		static_assert(sizeof(TValue) == 16);

		// lua_State fields this file touches (5.1 layout, x64): top @0x10, base @0x18, l_G @0x20.
		TValue*& TopRef(lua_State* L) { return *reinterpret_cast<TValue**>(reinterpret_cast<char*>(L) + 0x10); }
		TValue* Base(lua_State* L) { return *reinterpret_cast<TValue**>(reinterpret_cast<char*>(L) + 0x18); }
		std::uint8_t Status(lua_State* L) { return *(reinterpret_cast<std::uint8_t*>(L) + 0x0A); }
		void* GlobalOf(lua_State* L) { return *reinterpret_cast<void**>(reinterpret_cast<char*>(L) + 0x20); }

		using LoadBufferFn = int (*)(lua_State*, const char*, std::size_t, const char*);
		using PCallFn = int (*)(lua_State*, int, int, int);
		using SetFieldFn = void (*)(lua_State*, int, const char*);
		using ToLStringFn = const char* (*)(lua_State*, int, std::size_t*);
		using PushCClosureFn = void (*)(lua_State*, lua_CFunction, int);
		using CurThreadFn = std::int64_t (*)();
		using GetTopFn = int (*)(lua_State*);

		LoadBufferFn g_loadbuffer;
		PCallFn g_pcall, g_pcallOrig;
		SetFieldFn g_setfield;
		ToLStringFn g_tolstring;
		PushCClosureFn g_pushcclosure;
		CurThreadFn g_curThreadOrig;
		GetTopFn g_gettopOrig;
		std::atomic<DWORD> g_gpThread;
		lua_State** g_gameplaySlot;  // the game's own global holding the "game play" state

		struct Job
		{
			std::string name, code;
			std::function<void(bool, const std::string&)> done;
		};
		std::mutex g_mutex;
		std::deque<Job> g_jobs;
		std::vector<std::function<void(lua_State*)>> g_onState;
		lua_State* g_seenState;
		thread_local bool g_inJob;
		thread_local std::string g_out;  // mclog() output of the job running now

		std::int64_t g_lastThread;

		// Script actions ask for "the current script thread"; outside a script thread (our jobs) it
		// is 0 and they crash. SaintExec's fix: hand back the last real one.
		std::int64_t CurThreadHook()
		{
			std::int64_t t = g_curThreadOrig();
			if (t)
				return g_lastThread = t;
			return g_inJob ? g_lastThread : t;
		}

		int McLog(lua_State* L)
		{
			int n = Top(L);
			std::string line;
			for (int i = 1; i <= n; i++)
			{
				const char* s = g_tolstring(L, i, nullptr);
				line += s ? s : "<?>";
				if (i < n)
					line += '\t';
			}
			if (g_inJob)
				g_out += line + "\n";
			else
				Log("lua: %s", line.c_str());
			return 0;
		}

		bool RunChunk(lua_State* L, const char* name, const std::string& code, std::string& err)
		{
			TValue* top0 = TopRef(L);
			if (g_loadbuffer(L, code.data(), code.size(), name) != 0 || g_pcallOrig(L, 0, 0, 0) != 0)
			{
				const char* m = g_tolstring(L, -1, nullptr);
				err = m ? m : "(non-string error)";
				TopRef(L) = top0;
				return false;
			}
			TopRef(L) = top0;
			return true;
		}

		// On a game thread, inside the game's own Lua API call on L. Jobs run on L itself, and only if
		// it belongs to the gameplay universe and is running (status 0): a suspended coroutine can't
		// take calls, and the gameplay universe runs on whichever thread the game gives it.
		void Pump(lua_State* L)
		{
			lua_State* gp = Gameplay();
			if (!gp || g_inJob || GlobalOf(L) != GlobalOf(gp) || Status(L) != 0)
				return;
			if (gp != g_seenState)
			{
				g_seenState = gp;
				Log("lua: gameplay state %p", (void*)gp);
				g_inJob = true;
				std::string err;
				// The game locks its globals table after setup; this lets new globals be created.
				RunChunk(L, "=mcsr3_init", "pcall(_PrepareForDynamicGlobals, '')", err);
				Register(L, "mclog", McLog);
				std::vector<std::function<void(lua_State*)>> fns;
				{
					std::lock_guard lock(g_mutex);
					fns = g_onState;
				}
				for (auto& f : fns)
					f(L);
				g_inJob = false;
			}
			for (;;)
			{
				Job job;
				{
					std::lock_guard lock(g_mutex);
					if (g_jobs.empty())
						return;
					job = std::move(g_jobs.front());
					g_jobs.pop_front();
				}
				g_inJob = true;
				g_out.clear();
				std::string err;
				bool ok = RunChunk(L, ("=" + job.name).c_str(), job.code, err);
				g_inJob = false;
				if (!ok)
					g_out += "ERROR: " + err + "\n";
				if (job.done)
					job.done(ok, g_out);
			}
		}

		// Every script action (C function called from Lua) asks for lua_gettop first: whenever a
		// gameplay script runs, we get a turn on its thread.
		int GetTopHook(lua_State* L)
		{
			lua_State* gp = Gameplay();
			if (gp && GlobalOf(L) == GlobalOf(gp))
				g_gpThread = GetCurrentThreadId();
			if (!g_inJob)
				Pump(L);
			return int(TopRef(L) - Base(L));
		}

		int PCallHook(lua_State* L, int nargs, int nresults, int errfunc)
		{
			if (!g_inJob)
				Pump(L);
			return g_pcallOrig(L, nargs, nresults, errfunc);
		}
	}

	void Tick()
	{
		// Main thread, top of the frame: no Lua is running. Only if gameplay Lua lives on this thread.
		lua_State* gp = Gameplay();
		if (gp && GetCurrentThreadId() == g_gpThread && !g_inJob)
			Pump(gp);
	}

	lua_State* Gameplay() { return g_gameplaySlot ? *g_gameplaySlot : nullptr; }

	int Top(lua_State* L) { return int(TopRef(L) - Base(L)); }
	const char* ToString(lua_State* L, int idx) { return g_tolstring(L, idx, nullptr); }

	double ToNumber(lua_State* L, int idx)
	{
		TValue* v = idx > 0 ? Base(L) + (idx - 1) : TopRef(L) + idx;
		if (idx > 0 ? v >= TopRef(L) : false)
			return 0;
		if (v->tt == kTNumber)
			return v->value.n;
		const char* s = g_tolstring(L, idx, nullptr);
		return s ? std::strtod(s, nullptr) : 0;
	}

	void PushNumber(lua_State* L, double n)
	{
		TValue* t = TopRef(L);
		t->value.n = n;
		t->tt = kTNumber;
		TopRef(L) = t + 1;
	}

	void PushBool(lua_State* L, bool b)
	{
		TValue* t = TopRef(L);
		t->value.b = b;
		t->tt = kTBoolean;
		TopRef(L) = t + 1;
	}

	void PushNil(lua_State* L)
	{
		TValue* t = TopRef(L);
		t->tt = kTNil;
		TopRef(L) = t + 1;
	}

	void Register(lua_State* L, const char* name, lua_CFunction fn)
	{
		g_pushcclosure(L, fn, 0);
		g_setfield(L, kGlobalsIndex, name);
	}

	void Run(std::string name, std::string code, std::function<void(bool, const std::string&)> done)
	{
		std::lock_guard lock(g_mutex);
		g_jobs.push_back({ std::move(name), std::move(code), std::move(done) });
	}

	void OnState(std::function<void(lua_State*)> fn)
	{
		std::lock_guard lock(g_mutex);
		g_onState.push_back(std::move(fn));
	}

	bool Init()
	{
		g_loadbuffer = (LoadBufferFn)scan::Unique("luaL_loadbuffer", "48 83 EC ?? 48 89 54 24 20 48 8D");
		g_pcall = (PCallFn)scan::Unique("lua_pcall", "48 89 5C 24 08 57 48 83 EC ?? 41 8B F8 44");
		g_setfield = (SetFieldFn)scan::Unique("lua_setfield", "75 ?? 49 8B D2 48 8B CB E8 ?? ?? ?? ?? 4C 8B 4B 10 4C 8D 44 24 20 49", -40);
		g_tolstring = (ToLStringFn)scan::Unique("lua_tolstring", "48 89 74 24 10 57 48 83 EC ?? 49 8B D8 8B F2 48 8B F9 E8 ?? ?? ?? ?? 4C", -5);
		g_pushcclosure = (PushCClosureFn)scan::Unique("lua_pushcclosure", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ?? 4C 8B 49 20 48 8B F2");
		auto gettop = scan::Unique("lua_gettop", "48 8B 41 10 48 2B 41 18 48 C1 F8 04 C3");
		auto curThread = scan::Unique("lua_get_current_thread", "8B 0D ?? ?? ?? ?? 8D 41 FF 83 F8 ?? 76");

		// lea rcx, "game play" / call create_state / mov [rip+slot], rax
		std::uintptr_t lea = scan::LeaTo(scan::String("game play"));
		const auto* b = reinterpret_cast<const std::uint8_t*>(lea);
		if (lea && b[7] == 0xE8 && b[12] == 0x48 && b[13] == 0x89 && b[14] == 0x05)
			g_gameplaySlot = reinterpret_cast<lua_State**>(scan::Rip(lea + 15, lea + 19));
		Log("lua: gameplay state slot = SRTTR+0x%llx", g_gameplaySlot ? (unsigned long long)(std::uintptr_t(g_gameplaySlot) - scan::ImageBase()) : 0ull);

		if (!g_loadbuffer || !g_pcall || !g_setfield || !g_tolstring || !g_pushcclosure || !gettop || !curThread || !g_gameplaySlot)
		{
			Log("lua: this SRTTR build doesn't match; Lua features off");
			return false;
		}
		if (MH_CreateHook((void*)g_pcall, (void*)PCallHook, (void**)&g_pcallOrig) != MH_OK ||
			MH_CreateHook((void*)gettop, (void*)GetTopHook, (void**)&g_gettopOrig) != MH_OK ||
			MH_CreateHook((void*)curThread, (void*)CurThreadHook, (void**)&g_curThreadOrig) != MH_OK ||
			MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
		{
			Log("lua: hooking failed");
			return false;
		}
		return true;
	}
}
