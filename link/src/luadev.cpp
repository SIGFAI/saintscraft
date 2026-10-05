// Reverse-engineering helpers for the dev channel (tools/srlua.py):
//   mc_cfunc(f)        offset in SRTTR.exe of a script action's C function
//   mc_read(addr, t)   read memory: t = "f" float, "d" double, "i" int32, "q" pointer (as number)
//   mc_write(addr, t, v)  write memory (same types)
//   mc_obj(name)       address of a script object (e.g. "#PLAYER1#") via the game's own lookup
//   mc_hwwatch(addr)  hardware write watchpoint (0 clears); writers go to the log
//   mc_call(off, a1..a4)  call SRTTR+off with up to 4 integer args, returns rax (exploration only)
//   mc_hurt(hp | "fire" | "ragdoll")  damage (set on fire, knock over) the nearest SR3 person within 30 m; returns their
//                         distance (no argument: their address)
//                         (no argument: their address)
// Addresses travel as Lua numbers (doubles): exact for user-space pointers (< 2^53).
#include "luadev.h"

#include "log.h"
#include "hwwatch.h"
#include "lua.h"
#include "scan.h"
#include "sr3.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace mcsr3::luadev
{
	namespace
	{
		struct TValue
		{
			void* p;
			int tt;
			int pad;
		};

		TValue* Arg(lua_State* L, int idx)
		{
			return *reinterpret_cast<TValue**>(reinterpret_cast<char*>(L) + 0x18) + (idx - 1);
		}

		int CFunc(lua_State* L)
		{
			TValue* v = Arg(L, 1);
			if (lua::Top(L) < 1 || v->tt != 6)  // LUA_TFUNCTION
				return lua::PushNil(L), 1;
			auto* cl = static_cast<std::uint8_t*>(v->p);
			if (!cl[0x0A])  // isC
				return lua::PushNil(L), 1;
			auto f = *reinterpret_cast<std::uintptr_t*>(cl + 0x20);
			lua::PushNumber(L, double(f - scan::ImageBase()));
			return 1;
		}

		int Read(lua_State* L)
		{
			auto addr = std::uintptr_t(lua::ToNumber(L, 1));
			const char* t = lua::Top(L) >= 2 ? lua::ToString(L, 2) : "q";
			std::uint8_t buf[8]{};
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(addr), buf, 8, &got) || got < 4)
				return lua::PushNil(L), 1;
			double r = 0;
			switch (t ? t[0] : 'q')
			{
			case 'f': { float f; std::memcpy(&f, buf, 4); r = f; break; }
			case 'd': std::memcpy(&r, buf, 8); break;
			case 'i': { std::int32_t i; std::memcpy(&i, buf, 4); r = i; break; }
			default: { std::uint64_t q; std::memcpy(&q, buf, 8); r = double(q); break; }
			}
			lua::PushNumber(L, r);
			return 1;
		}

		int Write(lua_State* L)
		{
			auto addr = std::uintptr_t(lua::ToNumber(L, 1));
			const char* t = lua::ToString(L, 2);
			double v = lua::ToNumber(L, 3);
			std::uint8_t buf[8];
			SIZE_T n = 8;
			switch (t ? t[0] : 'q')
			{
			case 'f': { float f = float(v); std::memcpy(buf, &f, 4); n = 4; break; }
			case 'd': std::memcpy(buf, &v, 8); break;
			case 'i': { std::int32_t i = std::int32_t(v); std::memcpy(buf, &i, 4); n = 4; break; }
			default: { std::uint64_t q = std::uint64_t(v); std::memcpy(buf, &q, 8); break; }
			}
			SIZE_T done = 0;
			lua::PushBool(L, WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(addr), buf, n, &done) && done == n);
			return 1;
		}

		// SRTTR+0x638c00: script object lookup by name (from get_object_pos: SRTTR+0x645a60 ->
		// 0x63f130 -> 0x638c00(name, 0)).
		int Obj(lua_State* L)
		{
			using LookupFn = void* (*)(const char*, int);
			const char* name = lua::ToString(L, 1);
			void* o = name ? reinterpret_cast<LookupFn>(scan::ImageBase() + 0x638c00)(name, 0) : nullptr;
			if (!o)
				return lua::PushNil(L), 1;
			lua::PushNumber(L, double(reinterpret_cast<std::uintptr_t>(o)));
			return 1;
		}

		// mc_tp(x, y, z [, resetCam, checkObstacles]): the player through the teleport script
		// action's own internals (SRTTR+0x65dfc0): human lookup 0x629ae0, then 0x8829b0-style prep and
		// human_teleport 0x522eb0(human, pos, orient, 1, 1, 0, 0x2ab610(human, 4), resetCam, check).
		int Tp(lua_State* L)
		{
			using LookupFn = std::uint8_t* (*)(const char*, int);
			using PrepFn = void (*)(void*, int);
			using FlagsFn = std::uint16_t (*)(void*, int);
			using TeleportFn = void (*)(void*, const float*, const float*, bool, bool, bool, std::uint16_t, bool, bool);
			const auto base = scan::ImageBase();
			std::uint8_t* h = reinterpret_cast<LookupFn>(base + 0x629ae0)("#PLAYER1#", 0);
			if (!h)
				return lua::PushBool(L, false), 1;
			float pos[3] = { float(lua::ToNumber(L, 1)), float(lua::ToNumber(L, 2)), float(lua::ToNumber(L, 3)) };
			float orient[9];
			std::memcpy(orient, h + 0x6c, sizeof(orient));
			bool resetCam = lua::Top(L) >= 4 && lua::ToNumber(L, 4) != 0;
			bool check = lua::Top(L) >= 5 && lua::ToNumber(L, 5) != 0;
			reinterpret_cast<PrepFn>(base + 0x829b0)(h, 0);
			std::uint16_t flags = reinterpret_cast<FlagsFn>(base + 0x2ab610)(h, 4);
			reinterpret_cast<TeleportFn>(base + 0x522eb0)(h, pos, orient, true, true, false, flags, resetCam, check);
			lua::PushBool(L, true);
			return 1;
		}

		int Hurt(lua_State* L)
		{
			sr3::Player p;
			sr3::Human h[128];
			int n = sr3::GetPlayer(p) ? sr3::NearbyHumans(p.feet, 30.0f, h, 128) : 0;
			int best = -1;
			float bestD = 1e9f;
			for (int i = 0; i < n; i++)
			{
				float dx = h[i].feet.x - p.feet.x, dy = h[i].feet.y - p.feet.y, dz = h[i].feet.z - p.feet.z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (d < bestD)
					bestD = d, best = i;
			}
			if (best < 0)
				return lua::PushNil(L), 1;
			if (lua::Top(L) == 0)  // mc_hurt(): just who that is (the human's address)
				return lua::PushNumber(L, double(h[best].handle)), 1;
			const char* what = lua::ToString(L, 1);
			bool ok = what && std::strcmp(what, "fire") == 0 ? sr3::IgniteHuman(h[best].handle)
				: what && std::strcmp(what, "ragdoll") == 0 ? sr3::RagdollHuman(h[best].handle, 1500)
				: sr3::DamageHuman(h[best].handle, float(lua::ToNumber(L, 1)));
			lua::PushNumber(L, ok ? bestD : -bestD);
			return 1;
		}

		int HwWatch(lua_State* L)
		{
			hwwatch::Set(std::uintptr_t(lua::ToNumber(L, 1)));
			lua::PushNumber(L, hwwatch::Hits());
			return 1;
		}

		int Call(lua_State* L)
		{
			using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
			auto off = std::uintptr_t(lua::ToNumber(L, 1));
			std::uint64_t a[4]{};
			for (int i = 0; i < 4 && i + 2 <= lua::Top(L); i++)
				a[i] = std::uint64_t(lua::ToNumber(L, i + 2));
			lua::PushNumber(L, double(reinterpret_cast<Fn>(scan::ImageBase() + off)(a[0], a[1], a[2], a[3])));
			return 1;
		}
	}

	void Install()
	{
		lua::OnState([](lua_State* L) {
			lua::Register(L, "mc_cfunc", CFunc);
			lua::Register(L, "mc_read", Read);
			lua::Register(L, "mc_write", Write);
			lua::Register(L, "mc_obj", Obj);
			lua::Register(L, "mc_call", Call);
			lua::Register(L, "mc_tp", Tp);
			lua::Register(L, "mc_hurt", Hurt);
			lua::Register(L, "mc_hwwatch", HwWatch);
		});
	}
}
