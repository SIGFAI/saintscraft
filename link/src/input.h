#pragma once
#include <functional>

// SR3's keyboard and mouse input, intercepted where the game reads it: raw input (mouse buttons, wheel and
// movement), window messages (keys and typed characters), DirectInput (hooked too, though SR3 wasn't seen
// reading it). The handler decides per event whether the game sees it.
namespace mcsr3::input
{
	struct Event
	{
		enum Kind { Key, Button, Wheel, Move, Char } kind;
		unsigned code;   // Key: virtual-key code; Button: 0 left, 1 right, 2 middle, 3 x1, 4 x2; Char: code point
		int value;       // Key: 1 down, 2 repeat, 0 up; Button: 1 down, 0 up; Wheel: notches (+ = away); Move: dx
		int value2 = 0;  // Move: dy
		unsigned sdl = 0;  // Key: SDL scancode (0 = none)
	};

	bool Install();
	// Called on the game thread for each event; return true to keep it from the game.
	void SetHandler(std::function<bool(const Event&)> handler);
	void OnDirectInput8(void* di8);  // the proxy's DirectInput8Create result
	// SR3's keyboard is window messages: each one PeekMessage removes from the queue. True: swallow it.
	bool FilterMessage(void* msg);
}
