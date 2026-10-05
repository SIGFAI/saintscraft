#pragma once
#include <functional>
#include <string>

struct lua_State;
using lua_CFunction = int (*)(lua_State*);

// SR3's embedded Lua 5.1: the gameplay state ("game play") and a way to run code in it on the game's
// own thread.
namespace mcsr3::lua
{
	bool Init();  // find the Lua API and hook it; false (logged) if this build doesn't match
	void Tick();  // main-loop tick (mainloop::OnTick): runs queued jobs

	// Runs `code` in the gameplay state on the game thread at its next Lua call. `done` gets
	// (ok, output: everything mclog() printed, plus the error message on failure).
	void Run(std::string name, std::string code, std::function<void(bool, const std::string&)> done = {});

	// Called on the game thread whenever a fresh gameplay state appears (a save was loaded), so
	// features can register their functions and start their script threads.
	void OnState(std::function<void(lua_State*)> fn);

	// Lua API (resolved by Init) for C functions registered into the game.
	void Register(lua_State* L, const char* name, lua_CFunction fn);
	int Top(lua_State* L);
	const char* ToString(lua_State* L, int idx);
	double ToNumber(lua_State* L, int idx);
	void PushNumber(lua_State* L, double n);
	void PushBool(lua_State* L, bool b);
	void PushNil(lua_State* L);

	lua_State* Gameplay();  // nullptr while not in game
}
