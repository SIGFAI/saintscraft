#include "link.h"

#include "ground.h"
#include "input.h"
#include "log.h"
#include "lua.h"
#include "sr3.h"
#include "ws.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mcsr3::link
{
	namespace
	{
		constexpr double kRadToDeg = 57.29577951308232;
		WsClient g_ws;
		int g_generation;
		long long g_frame;

		// Minecraft yaw (0 = facing +Z, 90 = facing -X) and pitch (positive = down) of an SR3 direction.
		void YawPitch(const sr3::Vec3& f, float& yaw, float& pitch)
		{
			double mx = f.x, my = f.y, mz = -f.z;
			yaw = float(std::atan2(-mx, mz) * kRadToDeg);
			pitch = float(-std::asin(my < -1 ? -1 : my > 1 ? 1 : my) * kRadToDeg);
		}

		// SR3's window client size: Minecraft renders at the same size so the frames line up 1:1.
		bool WindowSize(int& w, int& h)
		{
			struct Find { DWORD pid; HWND wnd; } find{ GetCurrentProcessId(), nullptr };
			EnumWindows([](HWND wnd, LPARAM p) -> BOOL {
				auto* f = reinterpret_cast<Find*>(p);
				DWORD pid;
				GetWindowThreadProcessId(wnd, &pid);
				if (pid == f->pid && IsWindowVisible(wnd) && !GetWindow(wnd, GW_OWNER))
				{
					f->wnd = wnd;
					return FALSE;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&find));
			RECT r;
			if (!find.wnd || !GetClientRect(find.wnd, &r))
				return false;
			w = r.right - r.left;
			h = r.bottom - r.top;
			return w > 0 && h > 0;
		}

		// Two modes, F7 switches. Minecraft mode (on foot): first person in both games, Minecraft's hand and
		// HUD, and the mouse buttons, wheel and hotbar keys are Minecraft's. SR3 mode (and always in a
		// vehicle): plain Saints Row in third person; Minecraft shows only the blocks built in the world.
		std::atomic<bool> g_mcMode{ true };
		std::atomic<bool> g_inVehicle{ false };
		bool g_sentMode = false, g_lastMode = false;

		bool McMode() { return g_mcMode && !g_inVehicle && g_ws.connected(); }  // no Minecraft: plain SR3

		// Minecraft mode: the Boss is unarmed and SR3's weapon wheel is off (no guns going off with the clicks, no
		// weapon dial on screen). Said again every 2 s there (a respawn or a mission can hand weapons back), and
		// undone once on the way out, so SR3's own scripts keep the last word in Saints Row mode.
		void ApplySr3Side(bool mc)
		{
			static int state = -1;  // -1 unknown, 0 enabled, 1 disabled
			static ULONGLONG last;
			if (mc ? (state == 1 && GetTickCount64() - last < 2000) : state == 0)
				return;
			last = GetTickCount64();
			state = mc ? 1 : 0;
			// (gravity too: the Boss follows Minecraft's player, who can jump and fly)
			lua::Run("mcsr3_weapons", mc ? "hud_inventory_disable(true) inv_weapon_disable_all_slots(true) human_gravity_enable(LOCAL_PLAYER, false)"
				: "hud_inventory_disable(false) inv_weapon_disable_all_slots(false) human_gravity_enable(LOCAL_PLAYER, true)", nullptr);

			// SR3's HUD through its own display states (what missions use): the weapon dial (with the health ring
			// around it) and SR3's crosshair go; the minimap, money/respect and objectives stay (the user's choice)
			// (the display state's handle while Minecraft mode has one; kHudNone, or kHudPending while it's being made)
			constexpr int kHudNone = -1, kHudPending = -2;
			static std::atomic<int> hud{ kHudNone };
			if (mc && hud == kHudNone)
			{
				hud = kHudPending;
				lua::Run("mcsr3_hud", "local h = hud_display_create_state() hud_display_set_element(h, HUD_ELEM_WEAPONS, HUD_FADE_HIDDEN) "
					"hud_display_set_element(h, HUD_ELEM_RETICLE, HUD_FADE_HIDDEN) hud_display_commit_state(h) mclog(h)",
					[](bool ok, const std::string& out) {
						// Back in Saints Row mode before this ran (F7 twice quickly): take it straight off again,
						// or SR3's weapon dial, wheel and scopes stay hidden
						if (ok && !McMode())
							lua::Run("mcsr3_hud", "hud_display_remove_state(" + std::to_string(std::atoi(out.c_str())) + ")");
						hud = ok && McMode() ? std::atoi(out.c_str()) : kHudNone;
					});
			}
			else if (!mc && hud >= 0)
				lua::Run("mcsr3_hud", "hud_display_remove_state(" + std::to_string(hud.exchange(kHudNone)) + ")");
		}

		// Minecraft mode: Minecraft moves the player (walking speed, jumping, sprinting, creative flight) and the
		// Boss is put where it is every tick (his physics body, so no teleport fade), with SR3's gravity off so
		// he stays wherever Minecraft has him.
		constexpr float kMaxDriveStep = 8.0f;  // metres per tick; more means Minecraft's player isn't ours any more
		constexpr float kDriftMax = 0.8f;      // the Boss this far from it (sideways)...
		constexpr ULONGLONG kDriftMs = 400;    // ...for this long: SR3's physics won't have him there
		// Minecraft's player's feet (SR3 coordinates) from its latest "mcpos", and when it came. Main thread only
		// (messages are read in Tick).
		sr3::Vec3 g_driveFeet;
		ULONGLONG g_driveAt;
		ULONGLONG g_jumpAt;  // Minecraft teleported its player (ender pearl, /tp): the Boss goes too, however far

		// Each tick while Minecraft drives: the Boss goes where Minecraft's player is. Returns where that is
		// (false: not driving, or no fresh position from Minecraft).
		bool Drive(bool mc, const sr3::Player& player, sr3::Vec3& feet)
		{
			feet = g_driveFeet;
			bool live = mc && g_driveAt && GetTickCount64() - g_driveAt < 500;
			sr3::SetDriveFeet(live ? &feet : nullptr);
			if (!live)
				return false;
			float dx = feet.x - player.feet.x, dz = feet.z - player.feet.z, d2 = dx * dx + dz * dz;
			// SR3's physics pushes the Boss out of what it knows is solid (furniture, a wall Minecraft doesn't have
			// yet): if he stays away from Minecraft's player, that player comes back to him.
			static ULONGLONG behindSince;
			const ULONGLONG now = GetTickCount64();
			if (d2 <= kDriftMax * kDriftMax || now - g_jumpAt < 1000)
				behindSince = 0;
			else if (!behindSince)
				behindSince = now;
			if ((d2 > kMaxDriveStep * kMaxDriveStep && now - g_jumpAt >= 1000) || (behindSince && now - behindSince > kDriftMs))
			{
				// Minecraft's player is somewhere else (respawned, fell, through a wall): never drag the Boss there
				Log("link: Minecraft's player is %.1f m from the Boss: back to the Boss", std::sqrt(d2));
				behindSince = 0;
				g_driveAt = 0;
				sr3::SetDriveFeet(nullptr);
				McPos b = ToMc(player.feet.x, player.feet.y, player.feet.z);
				char m[128];
				std::snprintf(m, sizeof(m), "{\"t\":\"resync\",\"p\":[%.4f,%.4f,%.4f]}", b.x, b.y, b.z);
				Send(m);
				return false;
			}
			if (!sr3::PlacePlayer(feet))
			{
				static bool warned;
				if (!warned)
					warned = true, Log("link: can't place the Boss (physics body not found)");
			}
			return true;
		}

		// SR3's cars don't know Minecraft's blocks (its physics has no such thing): one whose front reaches a block the
		// player built is brought to a stop there, checked every kCarMs.
		constexpr ULONGLONG kCarMs = 100, kBlockSyncMs = 15000;
		constexpr float kCarRange = 60.0f, kCarFront = 2.6f, kCarHalfWidth = 1.1f, kCarHeight = 1.6f;

		void StopCarsAtBlocks(const sr3::Player& player)
		{
			static ULONGLONG last, synced;
			// (the blocks built in earlier sessions too: Minecraft lists those around the player, every so often)
			if (GetTickCount64() - synced > kBlockSyncMs)
				synced = GetTickCount64(), Send("{\"t\":\"blocksync\",\"r\":48}");
			const auto& built = ground::Built();
			if (built.empty() || GetTickCount64() - last < kCarMs)
				return;
			last = GetTickCount64();
			sr3::Vehicle cars[64];
			int n = sr3::NearbyVehicles(player.feet, kCarRange, cars, 64);
			for (int i = 0; i < n; i++)
			{
				const sr3::Vehicle& v = cars[i];
				for (const auto& [bx, by, bz] : built)
				{
					// the block's centre in SR3's space; is it in the strip from the car's centre to its front?
					float x, y, z;
					FromMc({ bx + 0.5, double(by), bz + 0.5 }, x, y, z);
					if (y + 1.0f < v.pos.y + 0.15f || y > v.pos.y + kCarHeight)
						continue;
					float dx = x - v.pos.x, dz = z - v.pos.z;
					float along = dx * v.forward.x + dz * v.forward.z, side = std::fabs(dx * v.forward.z - dz * v.forward.x);
					if (along > -0.5f && along < kCarFront + 0.5f && side < kCarHalfWidth + 0.5f)
					{
						static ULONGLONG lastLog;
						bool ok = sr3::StopVehicle(v.handle);
						if (GetTickCount64() - lastLog > 2000)
							lastLog = GetTickCount64(), Log("link: a car reached a Minecraft block at %d %d %d: stopped (%s)", bx, by, bz, ok ? "ok" : "failed");
						break;
					}
				}
			}
		}

		// Steve's arrows, fireworks and ender pearls in flight ("proj", each Minecraft tick) are traced through SR3's
		// world, which Minecraft only has as ground and walls near the player: an arrow sticks in an SR3 wall, a
		// firework bursts on it, and a pearl lands on it, however far (the Boss goes there, Steve after him).
		// Saints Row mode: the Boss's gunfire also reaches Minecraft's blocks (TNT goes off) and mobs. While the fire
		// button is held with a gun out, a shot every kShotMs along the camera's aim, as far as SR3's world lets it.
		constexpr ULONGLONG kShotMs = 120;
		constexpr float kShotRange = 300.0f;
		std::atomic<bool> g_firing{ false }, g_firearm{ false };

		void TraceShots()
		{
			static ULONGLONG last;
			sr3::Camera cam;
			if (!g_firing || !g_firearm || McMode() || sr3::Paused() || !g_ws.connected() || GetTickCount64() - last < kShotMs || !sr3::GetCamera(cam))
				return;
			last = GetTickCount64();
			const sr3::Vec3& f = cam.forward;
			float t = 1.0f;
			sr3::Raycast(cam.pos, { cam.pos.x + f.x * kShotRange, cam.pos.y + f.y * kShotRange, cam.pos.z + f.z * kShotRange }, t);
			McPos p = ToMc(cam.pos.x, cam.pos.y, cam.pos.z);
			char m[192];
			std::snprintf(m, sizeof(m), "{\"t\":\"shot\",\"p\":[%.3f,%.3f,%.3f],\"d\":[%.4f,%.4f,%.4f],\"max\":%.2f}", p.x, p.y, p.z, f.x, f.y, -f.z,
				kShotRange * t + 0.3f);
			Send(m);
		}

		struct Flight
		{
			int id;
			bool pearl, arrow;
			sr3::Vec3 from, to;
		};
		std::mutex g_flightMutex;
		std::vector<Flight> g_flights;                     // to trace at the next safe point
		std::unordered_map<int, sr3::Vec3> g_flightLast;   // main thread: each projectile's previous position

		void OnProjectiles(const std::string& m)
		{
			// {"t":"proj","p":[[id,"kind",x,y,z],...]}
			std::unordered_map<int, sr3::Vec3> now;
			std::vector<Flight> add;
			for (std::size_t i = m.find("[["); i != std::string::npos; i = m.find(",[", i))
			{
				const char* p = m.c_str() + i + 2;  // past "[[" or ",["
				char* end;
				int id = int(std::strtol(p, &end, 10));
				const char* kind = std::strchr(end, '"');
				const char* close = kind ? std::strchr(kind + 1, '"') : nullptr;
				if (!close)
					break;
				double v[3];
				p = close + 2;
				for (double& c : v)
				{
					c = std::strtod(p, &end);
					p = end + 1;
				}
				sr3::Vec3 at;
				FromMc({ v[0], v[1], v[2] }, at.x, at.y, at.z);
				now[id] = at;
				auto last = g_flightLast.find(id);
				if (last != g_flightLast.end())
					add.push_back({ id, std::strncmp(kind + 1, "pearl", 5) == 0, std::strncmp(kind + 1, "arrow", 5) == 0, last->second, at });
				i = close - m.c_str();
			}
			g_flightLast = std::move(now);
			std::lock_guard lock(g_flightMutex);
			g_flights.insert(g_flights.end(), add.begin(), add.end());
		}

		void TraceProjectiles()
		{
			std::vector<Flight> flights;
			{
				std::lock_guard lock(g_flightMutex);
				flights.swap(g_flights);
			}
			for (const Flight& f : flights)
			{
				float t;
				if (!sr3::Raycast(f.from, f.to, t))
					continue;
				sr3::Vec3 d{ f.to.x - f.from.x, f.to.y - f.from.y, f.to.z - f.from.z };
				float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
				// a little short of the surface (stuck arrows show; the player isn't put inside the wall)
				float back = f.pearl ? 0.4f : 0.05f, k = len > 1e-4f ? std::max(0.0f, t - back / len) : 0.0f;
				sr3::Vec3 hit{ f.from.x + d.x * k, f.from.y + d.y * k, f.from.z + d.z * k };
				McPos mh = ToMc(hit.x, hit.y, hit.z);
				char m[160];
				std::snprintf(m, sizeof(m), "{\"t\":\"projhit\",\"id\":%d,\"pos\":[%.3f,%.3f,%.3f],\"stick\":%s}", f.id, mh.x, mh.y, mh.z, f.arrow ? "true" : "false");
				Send(m);
				if (!f.pearl || !McMode())
					continue;
				// The pearl's landing: the floor under where it hit (a wall hit drops to the ground below it)
				sr3::Vec3 feet = hit;
				float down;
				if (sr3::Raycast({ hit.x, hit.y + 0.5f, hit.z }, { hit.x, hit.y - 60.0f, hit.z }, down))
					feet.y = hit.y + 0.5f - 60.5f * down;
				sr3::PlacePlayer(feet);
				g_jumpAt = GetTickCount64();
				g_driveAt = 0;  // Minecraft takes over again once the ground there has reached it
				McPos b = ToMc(feet.x, feet.y, feet.z);
				std::snprintf(m, sizeof(m), "{\"t\":\"resync\",\"p\":[%.4f,%.4f,%.4f]}", b.x, b.y, b.z);
				Send(m);
				Log("link: ender pearl landed on SR3's world at %.1f %.1f %.1f", feet.x, feet.y, feet.z);
			}
		}

		void ApplyMode()
		{
			bool mc = McMode();
			sr3::SetFirstPerson(mc);
			if (sr3::GetSnapshot().valid)
				ApplySr3Side(mc);
			if (!g_ws.connected() || (g_sentMode && mc == g_lastMode))
				return;
			g_sentMode = true;
			g_lastMode = mc;
			Send(mc ? "{\"t\":\"hud\",\"hidden\":false}" : "{\"t\":\"hud\",\"hidden\":true}");
			Log("link: %s mode", mc ? "Minecraft" : "Saints Row");
		}

		// A Minecraft screen (inventory, chat, menu) is open: Minecraft gets every key and click, and the mouse
		// moves a cursor (window pixels; the compositor draws it) instead of SR3's camera.
		std::atomic<bool> g_screenOpen{ false };
		std::atomic<float> g_cursorX{ 0 }, g_cursorY{ 0 };
		int g_viewW = 1280, g_viewH = 720;

		void SendKey(unsigned sdl, int value)
		{
			if (!sdl)
				return;
			char m[96];
			std::snprintf(m, sizeof(m), "{\"t\":\"in\",\"k\":\"key\",\"sc\":%u,\"d\":%d}", sdl, value ? 1 : 0);
			Send(m);
		}

		void SendButton(unsigned button, int down)
		{
			char m[96];
			std::snprintf(m, sizeof(m), "{\"t\":\"in\",\"k\":\"btn\",\"b\":%u,\"d\":%d}", button + 1, down);  // SDL: 1 left, 2 middle, 3 right
			Send(m);
		}

		// What SR3 keeps in Minecraft mode: the pause menu and mouse-look. Minecraft walks, jumps, sprints, sneaks
		// and flies the player (the Boss follows, see Drive).
		bool Sr3Key(unsigned vk) { return vk == VK_ESCAPE; }

		bool OnInput(const input::Event& e)
		{
			using E = input::Event;
			if (e.kind == E::Button && e.code == 0)
				g_firing = e.value != 0;  // (SR3 still gets it: its own gun fires too)
			if (e.kind == E::Key && e.code == VK_F7)
			{
				if (e.value == 1)
				{
					g_mcMode = !g_mcMode;
					Send("{\"t\":\"in\",\"k\":\"release\"}");
				}
				return true;
			}
			// Only in play: SR3's own menus, pause screen and loading keep every key and click.
			if (!McMode() || !sr3::GetSnapshot().valid || sr3::Paused())
				return false;
			const bool screen = g_screenOpen;
			char m[96];
			switch (e.kind)
			{
			case E::Button:
				if (e.code > 2)
					return false;
				// no middle click in the world: "pick block" on SR3's invisible ground hands out barriers
				if (e.code == 2 && !screen)
					return true;
				SendButton(e.code == 1 ? 2 : e.code == 2 ? 1 : 0, e.value);
				if (e.value)
					Log("link: mouse %s -> Minecraft", e.code == 0 ? "left" : e.code == 1 ? "right" : "middle");
				return true;
			case E::Wheel:
				std::snprintf(m, sizeof(m), "{\"t\":\"in\",\"k\":\"scroll\",\"v\":%d}", e.value);
				Send(m);
				return true;
			case E::Move:
				if (!screen)
				{
					sr3::FirstPersonLook(e.value2);
					return false;  // SR3 turns its camera (its yaw is first person's)
				}
				g_cursorX = std::clamp(g_cursorX + float(e.value), 0.0f, float(g_viewW - 1));
				g_cursorY = std::clamp(g_cursorY + float(e.value2), 0.0f, float(g_viewH - 1));
				std::snprintf(m, sizeof(m), "{\"t\":\"in\",\"k\":\"cursor\",\"x\":%.1f,\"y\":%.1f}", g_cursorX.load(), g_cursorY.load());
				Send(m);
				return true;
			case E::Char:
				if (!screen)
					return false;
				std::snprintf(m, sizeof(m), "{\"t\":\"in\",\"k\":\"text\",\"c\":%u}", e.code);
				if (e.code >= 32 && e.code != 127)
					Send(m);
				return true;
			case E::Key:
				if (screen)
				{
					SendKey(e.sdl, e.value);  // Esc closes the screen in Minecraft
					return true;
				}
				if (Sr3Key(e.code))
					return false;
				if (e.code == 'O')
				{
					if (e.value == 1)
						Send("{\"t\":\"menu\"}");
					return true;
				}
				if (e.value != 2)  // Minecraft makes its own repeats in the world
					SendKey(e.sdl, e.value);
				if (e.value == 1 && e.code >= '1' && e.code <= '9')
					Log("link: key %c -> Minecraft hotbar", char(e.code));
				return true;
			}
			return false;
		}

		// Polled from Lua every half second: in a vehicle SR3 has every control (vehicles are SR3's); and whether
		// the Boss holds a gun (his shots then reach Minecraft's blocks and mobs, see TraceShots).
		void PollVehicle()
		{
			static ULONGLONG last;
			if (GetTickCount64() - last < 500)
				return;
			last = GetTickCount64();
			lua::Run("mcsr3_vehicle", "mclog((character_is_in_vehicle(LOCAL_PLAYER) and 1 or 0) .. (inv_item_is_firearm_equipped(LOCAL_PLAYER) and 1 or 0))",
				[](bool ok, const std::string& out) {
				g_firearm = ok && out.size() > 1 && out[1] == '1';
				bool in = ok && !out.empty() && out[0] == '1';
				if (in != g_inVehicle)
				{
					g_inVehicle = in;
					Log("link: %s a vehicle", in ? "in" : "out of");
				}
			});
		}

		// Tiny readers for Minecraft's flat JSON messages ({"t":"explosion","pos":[x,y,z],"r":4.0}).
		std::string Field(const std::string& m, const char* key)
		{
			std::string k = std::string("\"") + key + "\":\"";
			auto i = m.find(k);
			if (i == std::string::npos)
				return {};
			i += k.size();
			return m.substr(i, m.find('"', i) - i);
		}

		bool Numbers(const std::string& m, const char* key, double* out, int n)
		{
			std::string k = std::string("\"") + key + "\":";
			auto i = m.find(k);
			if (i == std::string::npos)
				return false;
			const char* p = m.c_str() + i + k.size();
			if (*p == '[')
				p++;
			for (int j = 0; j < n; j++)
			{
				char* end;
				out[j] = std::strtod(p, &end);
				if (end == p)
					return false;
				p = end + (*end == ',' ? 1 : 0);
			}
			return true;
		}

		// SR3 people -> Minecraft proxies. Minecraft keys them by int: small ids stand in for SR3's 64-bit handles.
		constexpr float kPedRadius = 48.0f;
		constexpr float kDamageScale = 20.0f;  // Minecraft hearts -> SR3 hit points (diamond sword 7 -> 140; a pedestrian has 350)
		constexpr int kHitRagdollMs = 1500;
		std::unordered_map<std::uint64_t, int> g_pedId;
		std::unordered_map<int, std::uint64_t> g_pedHandle;
		int g_nextPedId = 1;

		void SendPeds(const sr3::Player& player)
		{
			static ULONGLONG last;
			if (GetTickCount64() - last < 100)
				return;
			last = GetTickCount64();
			sr3::Human humans[128];
			int n = sr3::NearbyHumans(player.feet, kPedRadius, humans, 128);
			static ULONGLONG lastLog;
			if (GetTickCount64() - lastLog > 10000)
			{
				lastLog = GetTickCount64();
				Log("link: %d SR3 people within %.0f m", n, kPedRadius);
			}
			std::string m = "{\"t\":\"peds\",\"p\":[";
			for (int i = 0; i < n; i++)
			{
				auto [it, added] = g_pedId.try_emplace(humans[i].handle, g_nextPedId);
				if (added)
					g_pedHandle[g_nextPedId++] = humans[i].handle;
				McPos p = ToMc(humans[i].feet.x, humans[i].feet.y, humans[i].feet.z);
				char e[96];
				std::snprintf(e, sizeof(e), "%s[%d,%.3f,%.3f,%.3f]", i ? "," : "", it->second, p.x, p.y, p.z);
				m += e;
			}
			Send(m + "]}");
		}

		void OnMessage(const std::string& m)
		{
			std::string t = Field(m, "t");
			if (t == "screen")
			{
				bool open = m.find("\"open\":true") != std::string::npos;
				if (open == g_screenOpen)
					return;  // (Minecraft repeats it every 2 s)
				if (open)
					g_cursorX = g_viewW * 0.5f, g_cursorY = g_viewH * 0.5f;
				g_screenOpen = open;
				Log("link: Minecraft screen %s", open ? "open" : "closed");
				return;
			}
			if (t == "mcpos")
			{
				// {"t":"mcpos","pos":[x,y,z],...}: where Minecraft's player is (feet), while it drives
				double pos[3];
				if (Numbers(m, "pos", pos, 3))
				{
					FromMc({ pos[0], pos[1], pos[2] }, g_driveFeet.x, g_driveFeet.y, g_driveFeet.z);
					g_driveAt = GetTickCount64();
				}
				return;
			}
			if (t == "pteleport")
			{
				g_jumpAt = GetTickCount64();
				Log("link: Minecraft teleported its player (ender pearl, /tp): the Boss follows");
				return;
			}
			if (t == "mobhit")
			{
				double h = 0, d = 0;
				if (!Numbers(m, "h", &h, 1) || !Numbers(m, "d", &d, 1))
					return;
				auto it = g_pedHandle.find(int(h));
				if (it == g_pedHandle.end())
					return;
				// Minecraft's fire on them (fire, lava, a burning arrow): SR3's own burning, at most every few seconds each
				if (m.find("\"fire\":true") != std::string::npos)
				{
					static std::unordered_map<int, ULONGLONG> lit;
					ULONGLONG& at = lit[int(h)];
					if (GetTickCount64() - at > 4000)
					{
						at = GetTickCount64();
						Log("link: SR3 person %d caught Minecraft's fire (%s)", int(h), sr3::IgniteHuman(it->second) ? "ok" : "gone");
					}
				}
				if (d <= 0)
					return;
				bool ok = sr3::DamageHuman(it->second, float(d) * kDamageScale);
				// Steve's hits (sword, arrows) knock them over, like Minecraft's knockback: SR3's script damage has
				// no hit reaction of its own
				if (ok && Field(m, "k") == "player")
					sr3::RagdollHuman(it->second, kHitRagdollMs);
				Log("link: %s hit SR3 person %d for %.1f (%s)", Field(m, "k").c_str(), int(h), d * kDamageScale, ok ? "ok" : "gone");
				return;
			}
			if (t == "explosion")
			{
				double pos[3], r = 4;
				if (!Numbers(m, "pos", pos, 3))
					return;
				Numbers(m, "r", &r, 1);
				float x, y, z;
				FromMc({ pos[0], pos[1], pos[2] }, x, y, z);
				// Minecraft power 4 (TNT, creeper) -> SR3's grenade (radius 4); bigger -> satchel charge (9)
				const char* type = r > 5 ? "Satchel" : "Grenade";
				bool ok = sr3::Explode({ x, y, z }, type);
				Log("link: Minecraft explosion r %.1f -> SR3 %s at %.1f %.1f %.1f%s", r, type, x, y, z, ok ? "" : " (failed)");
			}
			else if (t == "blocks")
			{
				// {"t":"blocks","set":[x,y,z,...],"clear":[x,y,z,...]}: what the player built or broke
				for (const char* key : { "set", "clear" })
				{
					auto i = m.find(std::string("\"") + key + "\":[");
					if (i == std::string::npos)
						continue;
					const char* p = m.c_str() + i + std::strlen(key) + 4;
					int v[3], n = 0;
					char* end;
					while (*p && *p != ']')
					{
						v[n++] = int(std::strtol(p, &end, 10));
						if (end == p)
							break;
						p = end + (*end == ',' ? 1 : 0);
						if (n == 3)
							ground::OnBuilt(v[0], v[1], v[2], key[0] == 's'), n = 0;
					}
				}
			}
			else if (t == "proj")
				OnProjectiles(m);
			else if (t != "mobs" && t != "hot" && m.size() < 300)
				Log("link: from Minecraft: %s", m.c_str());
		}

		// Minecraft renders at SR3's window size (sent on connecting, and again whenever it changes: fullscreen,
		// a new resolution), so its frames line up 1:1.
		void SendView(bool always)
		{
			static ULONGLONG last;
			if (!always && GetTickCount64() - last < 1000)
				return;
			last = GetTickCount64();
			int w, h;
			if (!WindowSize(w, h) || w <= 0 || h <= 0 || (!always && w == g_viewW && h == g_viewH))
				return;
			char m[96];
			std::snprintf(m, sizeof(m), "{\"t\":\"view\",\"w\":%d,\"h\":%d}", w, h);
			Send(m);
			g_viewW = w, g_viewH = h;
			Log("link: SR3's window is %dx%d: Minecraft renders at that size", w, h);
		}

		void OnConnect()
		{
			Log("link: connected to Minecraft");
			// The ground starts over (levelling clears everything around the player, earlier sessions' too).
			// Barrier items (picked off the invisible ground) make every barrier show: take them away.
			Send("{\"t\":\"cmd\",\"c\":\"clear @a minecraft:barrier\"}");
			g_sentMode = false;
			ground::Reset();
			SendView(true);
			g_screenOpen = false;
		}
	}

	bool Cursor(float& x, float& y)
	{
		x = g_cursorX;
		y = g_cursorY;
		return g_screenOpen && McMode() && g_ws.connected();
	}

	namespace
	{
		std::atomic<double> g_yOff{ 0.0 };
	}

	void SetYOffset(double off) { g_yOff = off; }

	McPos ToMc(float x, float y, float z) { return { double(x), double(y) + g_yOff, -double(z) }; }

	void FromMc(const McPos& m, float& x, float& y, float& z)
	{
		x = float(m.x);
		y = float(m.y - g_yOff);
		z = float(-m.z);
	}

	void Start()
	{
		g_ws.start("127.0.0.1", 25599);
		input::SetHandler(OnInput);
		sr3::OnSafePoint(TraceProjectiles);
		sr3::OnSafePoint(TraceShots);
	}
	bool Connected() { return g_ws.connected(); }
	void Send(const std::string& json) { g_ws.send(json); }

	void Tick()
	{
		if (g_ws.generation() != g_generation && g_ws.connected())
		{
			g_generation = g_ws.generation();
			OnConnect();
		}
		std::string in;
		while (g_ws.poll(in))
			OnMessage(in);

		PollVehicle();
		if (g_ws.connected())
			SendView(false);
		sr3::UpdateSnapshot();
		static bool wasPaused;
		bool paused = sr3::Paused();
		if (paused && !wasPaused)
			Send("{\"t\":\"in\",\"k\":\"release\"}");
		wasPaused = paused;
		ApplyMode();
		sr3::Camera cam;
		sr3::Player player;
		if (!g_ws.connected() || !sr3::GetCamera(cam) || !sr3::GetPlayer(player))
			return;
		SendPeds(player);
		StopCarsAtBlocks(player);
		McPos c = ToMc(cam.pos.x, cam.pos.y, cam.pos.z);
		const bool mc = McMode();
		// Minecraft drives once the ground is there to walk on (else the player would fall into the void)
		const bool driving = mc && ground::Ready();
		ground::HoldLevel(driving);
		sr3::Vec3 driven{};
		const bool drive = Drive(driving, player, driven);
		McPos feet = drive ? ToMc(driven.x, driven.y, driven.z) : ToMc(player.feet.x, player.feet.y, player.feet.z);
		float yaw, pitch, bodyYaw, bodyPitch;
		YawPitch(cam.forward, yaw, pitch);
		YawPitch(player.forward, bodyYaw, bodyPitch);
		char m[512];
		std::snprintf(m, sizeof(m),
			"{\"t\":\"cam\",\"f\":%lld,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,0],\"fov\":%.3f,\"fp\":%s,\"show\":%s,\"drive\":%s,\"pl\":[%.4f,%.4f,%.4f],\"h\":%.3f}",
			++g_frame, c.x, c.y, c.z, yaw, pitch, cam.verticalFovDeg(), mc ? "true" : "false", mc ? "true" : "false", driving ? "true" : "false", feet.x, feet.y, feet.z, bodyYaw);
		Send(m);
	}
}
