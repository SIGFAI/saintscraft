#pragma once

// Draws Minecraft's frame (from the mod's shared memory "Local\MCPassthroughFrame") into SR3's
// picture at Present: the world layer where it is in front of SR3's own geometry, then the overlay
// (hand, HUD, screens) on top.
namespace mcsr3::compositor
{
	bool Install();
	void SetEnabled(bool on);
	bool Enabled();
}
