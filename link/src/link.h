#pragma once
#include <string>

// The connection to the Minecraft mod (mc/, dev.mcsr3.client.HostLink): WebSocket JSON on 127.0.0.1:25599.
// Every frame SR3's camera and player go to Minecraft, which renders from that camera.
namespace mcsr3::link
{
	struct McPos
	{
		double x, y, z;
	};

	void Start();
	void Tick();  // main thread, once a frame (mainloop)
	bool Connected();
	void Send(const std::string& json);
	// While a Minecraft screen is open (Minecraft mode): its cursor, in window pixels. Any thread.
	bool Cursor(float& x, float& y);

	// SR3 metres (left-handed, Y up) <-> Minecraft blocks (right-handed, Y up): z flips, and y is shifted so
	// the floor under the player is a whole block (ground.cpp levels it).
	void SetYOffset(double off);
	McPos ToMc(float x, float y, float z);
	void FromMc(const McPos& m, float& x, float& y, float& z);
}
