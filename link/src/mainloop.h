#pragma once
#include <functional>

// SR3's main thread, at the top of its message pump: no Lua or game update is running. Gameplay Lua
// lives on this thread too, so this is where we read game state and drive our features.
namespace mcsr3::mainloop
{
	bool Install();
	void OnTick(std::function<void()> fn);  // at most once every 4 ms, main thread
	void OnPump(std::function<void()> fn);  // every PeekMessage call (keep it cheap), main thread
	unsigned long ThreadId();               // 0 until the first tick
}
