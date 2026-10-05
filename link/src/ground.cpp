#include "ground.h"

#include "link.h"
#include "log.h"
#include "sr3.h"

#include <windows.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

namespace mcsr3::ground
{
	namespace
	{
		constexpr int kRadius = 24;          // blocks around the player
		constexpr int kProbesPerFrame = 64;  // the whole disc in ~30 frames
		constexpr float kAbove = 2.0f;       // rays start this far above the feet (under a room's ceiling)
		constexpr float kNearAbove = 0.5f;   // (the player's own and neighbouring columns: just over a step)
		constexpr float kBelow = 40.0f;      // and reach this far below them (landing after flying)
		constexpr int kBuiltClearance = 32;  // no re-levelling with built blocks this close (they'd shift)
		constexpr int kOffFrames = 300;      // ~5 s off the grid before re-levelling
		constexpr float kRelevelMove = 24.0f, kRelevelRise = 3.0f;  // ...and only this far from the last levelling

		std::unordered_map<long long, int> g_sent;  // column -> surface sent (eighths of a block)
		std::set<std::tuple<int, int, int>> g_built;
		int g_cx, g_cz, g_cursor;
		bool g_centred, g_levelled;
		std::atomic<bool> g_holdLevel;
		int g_offLevel;  // frames the player's floor has been off the block grid
		sr3::Vec3 g_levelAt;  // where the last levelling happened (a room with two floor heights mustn't flip-flop)

		long long Key(int x, int z) { return (long long(x) << 32) ^ (unsigned(z)); }

		constexpr int kDepth = 2;  // full barriers under each column's top block

		// Walls: horizontal rays at chest height (above floor steps, which the ground columns already have) fan out
		// from the player; what they hit becomes a 3-block barrier column, and a ray that later passes through a
		// column takes its wall away again (doors open, cars drive off).
		constexpr int kWallDirections = 128, kWallRaysPerFrame = 16;
		constexpr float kWallHeight = 1.2f, kWallReach = 24.0f;
		std::unordered_map<long long, int> g_walls;  // column -> wall's bottom block
		int g_wallCursor;

		// One frame's changes: columns to place ({"t":"ground"}: x, z, bottom block, top in eighths) and block
		// ranges to lift ({"t":"unground"}: x, z, y0, y1), sent lifts first.
		struct Batch
		{
			std::string place, lift;

			static void Append(std::string& to, int a, int b, int c, int d)
			{
				char e[64];
				std::snprintf(e, sizeof(e), "%s%d,%d,%d,%d", to.empty() ? "" : ",", a, b, c, d);
				to += e;
			}
			void Place(int x, int z, int bottom, int topEighths) { Append(place, x, z, bottom, topEighths); }
			void Lift(int x, int z, int y0, int y1) { Append(lift, x, z, y0, y1); }

			void Send() const
			{
				if (!lift.empty())
					link::Send("{\"t\":\"unground\",\"c\":[" + lift + "]}");
				if (!place.empty())
					link::Send("{\"t\":\"ground\",\"c\":[" + place + "]}");
			}
		};

		// Ceilings near the player: a ray up each column; what it hits over head height becomes one barrier block
		// (so flying indoors stops at the ceiling instead of going up into the floor above).
		constexpr int kCeilRadius = 6;
		constexpr float kCeilFrom = 0.5f, kCeilReach = 8.0f, kHeadroom = 1.9f;
		std::unordered_map<long long, int> g_ceilings;  // column -> ceiling block

		void ProbeCeiling(int bx, int bz, float sx, float sz, const sr3::Vec3& feet, Batch& batch)
		{
			float f;
			sr3::Vec3 from{ sx, feet.y + kCeilFrom, sz }, to{ sx, feet.y + kCeilReach, sz };
			int y = INT_MIN;
			if (sr3::Raycast(from, to, f))
			{
				float hy = from.y + (to.y - from.y) * f;
				if (hy - feet.y > kHeadroom)
					y = int(std::floor(link::ToMc(sx, hy, sz).y + 0.01));
			}
			auto it = g_ceilings.find(Key(bx, bz));
			int old = it == g_ceilings.end() ? INT_MIN : it->second;
			if (old == y)
				return;
			if (old != INT_MIN)
				batch.Lift(bx, bz, old, old);
			if (y == INT_MIN)
				g_ceilings.erase(Key(bx, bz));
			else
			{
				g_ceilings[Key(bx, bz)] = y;
				batch.Place(bx, bz, y, (y + 1) * 8);
			}
		}

		void RemoveWall(int x, int z, Batch& batch)
		{
			auto it = g_walls.find(Key(x, z));
			if (it == g_walls.end())
				return;
			batch.Lift(x, z, it->second, it->second + 2);
			g_walls.erase(it);
		}

		void ProbeWalls(const sr3::Vec3& feet, Batch& batch)
		{
			const float y = feet.y + kWallHeight;
			const link::McPos at = link::ToMc(feet.x, feet.y, feet.z);
			const int bottom = int(std::floor(at.y)) + 1;  // over the floor's own block
			const int px = int(std::floor(at.x)), pz = int(std::floor(at.z));
			for (int n = 0; n < kWallRaysPerFrame; n++, g_wallCursor = (g_wallCursor + 1) % kWallDirections)
			{
				float a = g_wallCursor * 6.2831853f / kWallDirections, dx = std::cos(a), dz = std::sin(a);
				sr3::Vec3 from{ feet.x, y, feet.z }, to{ feet.x + dx * kWallReach, y, feet.z + dz * kWallReach };
				float f = 1.0f;
				bool hit = sr3::Raycast(from, to, f);
				float reach = kWallReach * f;
				// the columns the ray crossed are open
				for (float d = 0.5f; d < reach - 0.3f; d += 0.5f)
				{
					link::McPos p = link::ToMc(feet.x + dx * d, y, feet.z + dz * d);
					int bx = int(std::floor(p.x)), bz = int(std::floor(p.z));
					RemoveWall(bx, bz, batch);
				}
				if (!hit)
					continue;
				link::McPos p = link::ToMc(feet.x + dx * (reach + 0.15f), y, feet.z + dz * (reach + 0.15f));
				int bx = int(std::floor(p.x)), bz = int(std::floor(p.z));
				if (g_walls.count(Key(bx, bz)) || (bx == px && bz == pz))
					continue;  // (never where the player stands: hugging a wall would wall them in)
				g_walls[Key(bx, bz)] = bottom;
				batch.Place(bx, bz, bottom, (bottom + 3) * 8);
			}
		}

		int FloorDiv8(int eighths) { return eighths >= 0 ? eighths / 8 : -((-eighths + 7) / 8); }

		bool GroundUnder(float x, float y, float z, float& gy, float above = kAbove)
		{
			float f;
			sr3::Vec3 from{ x, y + above, z }, to{ x, y - kBelow, z };
			if (!sr3::Raycast(from, to, f))
				return false;
			gy = from.y + (to.y - from.y) * f;
			return true;
		}

		bool BuiltNear(int x, int z)
		{
			for (const auto& [bx, by, bz] : g_built)
				if (std::abs(bx - x) < kBuiltClearance && std::abs(bz - z) < kBuiltClearance)
					return true;
			return false;
		}

		// The floor under the player lands on a whole block: Minecraft y = SR3 y + offset.
		void Level(const sr3::Vec3& feet, float floorY)
		{
			g_levelAt = { feet.x, floorY, feet.z };
			double off = std::ceil(floorY) - floorY;
			link::SetYOffset(off);
			// everything of the host's ground near the player goes (this session's and any saved by earlier ones)
			link::McPos at = link::ToMc(feet.x, feet.y, feet.z);
			char clear[128];
			std::snprintf(clear, sizeof(clear), "{\"t\":\"clear\",\"around\":[%.2f,%.2f,%.2f]}", at.x, at.y, at.z);
			link::Send(clear);
			g_sent.clear();
			g_walls.clear();
			g_ceilings.clear();
			g_levelled = true;
			g_offLevel = 0;
			Log("ground: levelled (SR3 floor %.2f -> Minecraft %.0f, offset %.3f)", floorY, floorY + off, off);
		}

		void SafePoint()
		{
			sr3::Player player;
			if (!link::Connected() || sr3::Paused() || !sr3::GetPlayer(player))
				return;
			const sr3::Vec3 feet = player.feet;

			float floorY;
			if (GroundUnder(feet.x, feet.y, feet.z, floorY))
			{
				link::McPos f = link::ToMc(feet.x, floorY, feet.z);
				double frac = f.y - std::round(f.y);
				float moved = std::hypot(feet.x - g_levelAt.x, feet.z - g_levelAt.z), rose = std::fabs(floorY - g_levelAt.y);
				if (!g_levelled)
					Level(feet, floorY);
				else if (!g_holdLevel && std::fabs(frac) > 0.08 && ++g_offLevel > kOffFrames && (moved > kRelevelMove || rose > kRelevelRise) &&
					!BuiltNear(int(std::floor(f.x)), int(std::floor(f.z))))
					Level(feet, floorY);
				else if (std::fabs(frac) <= 0.08)
					g_offLevel = 0;
			}
			if (!g_levelled)
				return;

			link::McPos c = link::ToMc(feet.x, feet.y, feet.z);
			int cx = int(std::floor(c.x)), cz = int(std::floor(c.z));
			if (!g_centred || std::abs(cx - g_cx) > 8 || std::abs(cz - g_cz) > 8)
				g_cx = cx, g_cz = cz, g_centred = true, g_cursor = 0;

			const int side = 2 * kRadius + 1;
			Batch batch;
			for (int n = 0; n < kProbesPerFrame; n++, g_cursor = (g_cursor + 1) % (side * side))
			{
				int dx = g_cursor % side - kRadius, dz = g_cursor / side - kRadius;
				if (dx * dx + dz * dz > kRadius * kRadius)
					continue;
				int bx = g_cx + dx, bz = g_cz + dz;
				float sx, sy, sz;
				link::FromMc({ bx + 0.5, c.y, bz + 0.5 }, sx, sy, sz);
				if (std::abs(dx) <= kCeilRadius && std::abs(dz) <= kCeilRadius)
					ProbeCeiling(bx, bz, sx, sz, feet, batch);
				float gy;
				// right around the player, start under head height: an arch or a lamp above them isn't their floor
				// (it would make a pillar of the column they stand in)
				bool beside = std::abs(dx) <= 1 && std::abs(dz) <= 1;
				if (!GroundUnder(sx, feet.y, sz, gy, beside ? kNearAbove : kAbove))
					continue;
				// the ground's surface in eighths of a block: barriers up to it, a ground layer on top (GroundBlock)
				int top = int(std::lround(link::ToMc(sx, gy, sz).y * 8.0));
				static int logged;
				if (logged < 60 && std::abs(dx) <= 4 && std::abs(dz) <= 4 && GetFileAttributesW((DataDir() + L"logprobes").c_str()) != INVALID_FILE_ATTRIBUTES)
					logged++, Log("ground: probe %d,%d (SR3 %.2f %.2f) ground %.2f -> top %.3f (feet %.2f)", bx, bz, sx, sz, gy, top / 8.0, feet.y);
				auto it = g_sent.find(Key(bx, bz));
				if (it != g_sent.end() && it->second == top)
					continue;
				if (it != g_sent.end())  // the floor here moved (e.g. now seen past a table): lift the old column
					batch.Lift(bx, bz, FloorDiv8(it->second) - kDepth, FloorDiv8(it->second));
				g_sent[Key(bx, bz)] = top;
				batch.Place(bx, bz, FloorDiv8(top) - kDepth, top);
			}
			ProbeWalls(feet, batch);
			batch.Send();

			static ULONGLONG lastLog;
			if (GetTickCount64() - lastLog > 10000)
			{
				lastLog = GetTickCount64();
				Log("ground: %zu columns, %zu walls, %zu ceilings", g_sent.size(), g_walls.size(), g_ceilings.size());
			}
		}
	}

	void Install() { sr3::OnSafePoint(SafePoint); }

	bool Ready() { return g_levelled && g_sent.size() > 200; }

	void HoldLevel(bool hold) { g_holdLevel = hold; }

	void Reset()
	{
		g_sent.clear();
		g_walls.clear();
		g_ceilings.clear();
		g_levelled = false;
	}

	const std::set<std::tuple<int, int, int>>& Built() { return g_built; }

	void OnBuilt(int x, int y, int z, bool placed)
	{
		if (placed)
			g_built.insert({ x, y, z });
		else
			g_built.erase({ x, y, z });
	}
}
