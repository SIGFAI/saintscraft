#pragma once
#include <set>
#include <tuple>

// SR3's ground as Minecraft barrier columns, the way the GTA V mashup does it: vertical raycasts on a grid
// around the player (inside the game's update, where raycasts are safe), so mobs, items and blocks rest on
// SR3's floors and roads in every direction, not just where the camera looks. Minecraft's world is shifted
// up/down (link::YOffset) so the floor the player stands on is exactly a block's top.
namespace mcsr3::ground
{
	void Install();
	void Reset();  // Minecraft reconnected: send everything again (after a "clear")
	// A block the player built (Minecraft's "blocks" messages): re-levelling waits while any is near.
	void OnBuilt(int x, int y, int z, bool placed);
	// The blocks the player has built (Minecraft block coordinates). Main thread.
	const std::set<std::tuple<int, int, int>>& Built();
	// Levelled and the ground around the player sent: Minecraft can walk the player on it.
	bool Ready();
	// While Minecraft drives the player, the level stays put (a shift would move the player with it).
	void HoldLevel(bool hold);
}
