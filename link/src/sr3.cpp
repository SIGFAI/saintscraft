#include "sr3.h"

#include "log.h"

#include <MinHook.h>
#include "scan.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace mcsr3::sr3
{
	namespace
	{
		// Offsets in SRTTR.exe (Steam build of 2024-08, SRTTR.exe 21,474,968 bytes). Init() checks the
		// code at each before anything is called or read.
		constexpr std::uintptr_t kHumanLookup = 0x629ae0;  // human* (const char* script name, int)
		constexpr std::uintptr_t kCamera = 0x13ada40;      // the game camera (static)
		// Per-frame camera update (main thread): copies the computed camera (+0x70 position, +0xb0 rotation)
		// into the one the renderer uses (+0x60, +0x80). Found by a write watchpoint on +0x60.
		constexpr std::uintptr_t kCameraUpdate = 0xe2070;
		constexpr float kEyeHeight = 1.62f, kEyeForward = 0.3f;
		constexpr float kDriveLead = 0.3f;  // metres the eyes may be ahead of the Boss while Minecraft drives
		constexpr float kEyeClear = 0.25f;  // ...and how close to a wall they may get
		constexpr float kLookRadPerCount = 0.0022f, kMaxPitch = 1.553f;  // ~89 degrees, like Minecraft
		// Pause: a counter the pause menu/map/phone increment (inc 0x83cb30 / dec 0x83cb50). Found by
		// diffing the game's statics over 5 pause/unpause cycles; 1 while paused, 0 in play.
		constexpr std::uintptr_t kPauseCount = 0x295b624;
		constexpr std::uintptr_t kPauseInc = 0x83cb30;
		// havokRaycast (query*, results*, bool): see docs/RE.md. Results are freed like its callers do.
		constexpr std::uintptr_t kRaycast = 0x32ede0;
		constexpr std::uintptr_t kAllocator = 0x976e40;
		constexpr std::uintptr_t kFree = 0x976870;
		// explosion_create (Lua C function 0x642c10) internals: type lookup by name, and the spawn.
		constexpr std::uintptr_t kExplosionInfo = 0x11b330;  // info* (const char* name)
		constexpr std::uintptr_t kExplosionSpawn = 0x118db0; // see Explode
		constexpr std::uintptr_t kIdentity3x3 = 0x11ac380;   // a static identity rotation the game passes
		// The object table behind handle -> object (0x48910, used by get_num_humans_in_trigger 0x645880): an
		// open-addressing hash of object pointers, capacity u32 at +0x8, slots at +0x10, empty = *kEmpty.
		// Walked directly: the game's box query (0x32bfe0, Havok's broadphase) crashed when called from the
		// top of the frame, where physics may be stepping on other threads.
		constexpr std::uintptr_t kHandleTable = 0x117f458;
		constexpr std::uintptr_t kEmptySlot = 0xe473f0;
		constexpr std::uintptr_t kHandleResolve = 0x48910;   // only checked (prologue), as the layout's witness
		constexpr std::uintptr_t kTypeTable = 0x28e4760;     // per object type (obj+0x4c): flags; +0xe bit 0 = human
		constexpr std::uintptr_t kVehicleStop = 0x712110;    // (vehicle handle, true), as vehicle_stop_do 0x6671b0
		constexpr std::size_t kHumanHealth = 0x1ea8;         // float hit points (a pedestrian: 350)
		constexpr std::uint8_t kVehicleType = 7;            // obj+0x4c of cars (rays logged moving along a road; flags 07 03 8f)
		// character_damage (0x63aa20) internals
		constexpr std::uintptr_t kHumanDamage = 0x4fadc0;    // (human*, attacker*, float hp, 0, 0, u32* flags)
		constexpr std::uintptr_t kHumanIgnite = 0x509f00;    // (human*, 0, 0, 0, false, false, true), as character_ignite 0x63b160
		constexpr std::uintptr_t kHumanRagdoll = 0x628ff0;   // (human*, int ms, -1.0f, nullptr, true, false), as character_ragdoll 0x63bf50

		using HumanLookupFn = std::uint8_t* (*)(const char*, int);
		using ExplosionInfoFn = void* (*)(const char*);
		using ExplosionSpawnFn = std::uint64_t (*)(void* info, void* source, void* owner, const float* orient, const float* pos, void*, bool, bool, void*, void*,
			bool, bool, float, void*, const std::uint16_t* zone);
		using HumanDamageFn = void (*)(void*, void*, float, std::uint64_t, std::uint64_t, std::uint32_t*);
		bool g_objectTable;
		HumanDamageFn g_damage;
		using HumanIgniteFn = void (*)(void*, void*, void*, void*, bool, bool, bool);
		HumanIgniteFn g_ignite;
		using HumanRagdollFn = void (*)(void*, int, float, const char*, bool, bool);
		HumanRagdollFn g_ragdoll;
		HumanLookupFn g_lookup;
		std::atomic<bool> g_firstPerson;
		std::atomic<float> g_pitch{ NAN };  // first person's own pitch (radians, + = up); NaN = take the game's
		bool g_drive;                       // Minecraft drives: first person's eyes over g_driveFeet. Main thread only
		Vec3 g_driveFeet;
		std::uint8_t* g_hiddenPlayer;  // whose +0x53 hidden bit we set for this frame's rendering
		using CameraUpdateFn = void (*)(void*, void*, void*, void*);
		CameraUpdateFn g_cameraUpdateOrig;

		std::vector<std::function<void()>> g_safePoint;
		const std::int32_t* g_pauseCount;
		using RaycastFn = bool (*)(void* query, void* results, bool);
		using AllocatorFn = void* (*)();
		using FreeFn = void (*)(void* alloc, void* p, std::uint32_t, std::uint32_t capacity);
		RaycastFn g_raycast;
		AllocatorFn g_allocator;
		FreeFn g_free;
		std::mutex g_snapMutex;
		Snapshot g_snap;
		ExplosionInfoFn g_explosionInfo;
		ExplosionSpawnFn g_explosionSpawn;
		const std::uint8_t* g_cam;

		Vec3 V(const std::uint8_t* p) { Vec3 v; std::memcpy(&v, p, sizeof(v)); return v; }
		float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
	}

	namespace
	{
		void CameraUpdateHook(void* a, void* b, void* c, void* d)
		{
			g_cameraUpdateOrig(a, b, c, d);
			for (auto& f : g_safePoint)
				f();
			if (!g_firstPerson || !g_lookup || !g_cam)
				return;
			const std::uint8_t* player = g_lookup("#PLAYER1#", 0);
			if (!player)
				return;
			// The render camera (+0x60) goes to the eyes, a little forward so the head doesn't show; the
			// game's own camera rotation (the mouse) stays.
			Vec3 feet = V(player + 0x60), fwd = V(g_cam + 0xa0);
			if (g_drive)
			{
				// Where Minecraft has the player now (the Boss gets there next frame), but never more than
				// kDriveLead sideways from the Boss: where SR3 refuses him (a wall or prop Minecraft doesn't
				// know), the eyes would be inside it and the screen black until the resync
				float dx = g_driveFeet.x - feet.x, dz = g_driveFeet.z - feet.z, lead = std::sqrt(dx * dx + dz * dz);
				float k = lead > kDriveLead ? kDriveLead / lead : 1.0f;
				feet = { feet.x + dx * k, g_driveFeet.y, feet.z + dz * k };
			}
			float hx = fwd.x, hz = fwd.z, hl = std::sqrt(hx * hx + hz * hz);
			if (hl < 1e-4f)
				return;
			hx /= hl, hz /= hl;
			std::uint8_t* cam = const_cast<std::uint8_t*>(g_cam);
			float eye[3] = { feet.x + hx * kEyeForward, feet.y + kEyeHeight, feet.z + hz * kEyeForward };
			if (g_drive)
			{
				// Never through a wall from the Boss's own eyes (he is always somewhere SR3 allows): stop
				// kEyeClear short of the first hit along the way (plus kEyeClear past the eyes, the near plane)
				Vec3 head = V(player + 0x60);
				head.y += kEyeHeight;
				float dx = eye[0] - head.x, dy = eye[1] - head.y, dz = eye[2] - head.z;
				float len = std::sqrt(dx * dx + dy * dy + dz * dz), f;
				if (len > 1e-3f)
				{
					float s = (len + kEyeClear) / len;
					if (Raycast(head, { head.x + dx * s, head.y + dy * s, head.z + dz * s }, f))
					{
						float k = std::max(0.0f, f * s - kEyeClear / len);
						eye[0] = head.x + dx * k, eye[1] = head.y + dy * k, eye[2] = head.z + dz * k;
					}
				}
			}
			std::memcpy(cam + 0x60, eye, sizeof(eye));
			// The game's yaw, our pitch: forward and up turn about the (level) right axis the game already has
			float pitch = g_pitch;
			if (std::isnan(pitch))
				g_pitch = pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
			float cp = std::cos(pitch), sp = std::sin(pitch);
			float f[3] = { hx * cp, sp, hz * cp }, u[3] = { -hx * sp, cp, -hz * sp }, r[3] = { hz, 0.0f, -hx };
			if (Dot(V(cam + 0x80), { r[0], r[1], r[2] }) < 0.0f)
				r[0] = -r[0], r[2] = -r[2];  // keep the game's handedness
			std::memcpy(cam + 0x80, r, sizeof(r));
			std::memcpy(cam + 0x90, u, sizeof(u));
			std::memcpy(cam + 0xa0, f, sizeof(f));
			// The Boss isn't drawn in first person. +0x53 bit 0 is character_hide's flag, which the renderer
			// reads, but it also stops the character's update: so it is set only from here (the frame's
			// update is done) until the top of the next frame (EndFrameHide), for the renderer alone.
			std::uint8_t* p = const_cast<std::uint8_t*>(player);
			if (!(p[0x53] & 1))
			{
				p[0x53] |= 1;
				g_hiddenPlayer = p;
			}
		}
	}

	void SetFirstPerson(bool on)
	{
		if (on && !g_firstPerson)
			g_pitch = NAN;  // start from wherever the game camera looks
		g_firstPerson = on;
	}

	void SetDriveFeet(const Vec3* feet)
	{
		g_drive = feet != nullptr;
		if (feet)
			g_driveFeet = *feet;
	}

	bool PlacePlayer(const Vec3& feet)
	{
		// The game copies the player's position (+0x60) from his Havok body every frame (docs/RE.md): the
		// body's transform translation is human+0xe30 -> +0x20 -> +0x90 -> +0x30, +0x30. Written there, he is
		// simply there next frame. (teleport_to_object fades to black while the world streams in.)
		std::uint8_t* h = g_lookup ? g_lookup("#PLAYER1#", 0) : nullptr;
		auto next = [](std::uint8_t* p, std::size_t off) { return p ? *reinterpret_cast<std::uint8_t**>(p + off) : nullptr; };
		std::uint8_t* body = next(next(next(next(h, 0xe30), 0x20), 0x90), 0x30);
		if (!body)
			return false;
		float* t = reinterpret_cast<float*>(body + 0x30);
		// Only if it still holds the player's position (a different layout would be anything else)
		if (std::fabs(t[0] - V(h + 0x60).x) > 2.0f || std::fabs(t[2] - V(h + 0x60).z) > 2.0f)
			return false;
		t[0] = feet.x, t[1] = feet.y, t[2] = feet.z;
		return true;
	}

	void FirstPersonLook(int dy)
	{
		float p = g_pitch;
		if (g_firstPerson && !std::isnan(p))
			g_pitch = std::clamp(p - float(dy) * kLookRadPerCount, -kMaxPitch, kMaxPitch);
	}

	void OnSafePoint(std::function<void()> fn) { g_safePoint.push_back(std::move(fn)); }

	bool Paused() { return g_pauseCount && *reinterpret_cast<const volatile std::int32_t*>(g_pauseCount) > 0; }


	void UnhideForUpdate()
	{
		if (g_hiddenPlayer && g_lookup && g_lookup("#PLAYER1#", 0) == g_hiddenPlayer)
			g_hiddenPlayer[0x53] &= ~1;
		g_hiddenPlayer = nullptr;
	}
	bool FirstPerson() { return g_firstPerson; }

	void UpdateSnapshot()
	{
		Snapshot s;
		Player p;
		if (GetPlayer(p))
		{
			s.valid = true;
			s.feet = p.feet;
			Human h[128];
			s.humans = NearbyHumans(p.feet, 48.0f, h, 128);
			for (int i = 0; i < s.humans; i++)
				s.humanFeet[i] = h[i].feet;
		}
		std::lock_guard lock(g_snapMutex);
		g_snap = s;
	}

	Snapshot GetSnapshot()
	{
		std::lock_guard lock(g_snapMutex);
		return g_snap;
	}

	float Camera::verticalFovDeg() const
	{
		// The game's projection matrix (found in its constant buffers) has ys = 1 / (tan(fov/2) * 3/4).
		return float(2.0 * std::atan(std::tan(fovDeg * 0.5 * 0.017453292519943295) * 0.75) * 57.29577951308232);
	}

	bool Init()
	{
		const auto base = scan::ImageBase();
		// The lookup's first bytes, as of the build above (checked so a different build fails loudly).
		static const std::uint8_t lookupHead[] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9 };
		if (std::memcmp(reinterpret_cast<void*>(base + kHumanLookup), lookupHead, sizeof(lookupHead)) != 0)
		{
			Log("sr3: human lookup at SRTTR+0x%llx doesn't match this build", (unsigned long long)kHumanLookup);
			return false;
		}
		g_lookup = reinterpret_cast<HumanLookupFn>(base + kHumanLookup);
		static const std::uint8_t infoHead[] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x07 };
		static const std::uint8_t spawnHead[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 };
		if (std::memcmp(reinterpret_cast<void*>(base + kExplosionInfo), infoHead, sizeof(infoHead)) == 0 &&
			std::memcmp(reinterpret_cast<void*>(base + kExplosionSpawn), spawnHead, sizeof(spawnHead)) == 0)
		{
			g_explosionInfo = reinterpret_cast<ExplosionInfoFn>(base + kExplosionInfo);
			g_explosionSpawn = reinterpret_cast<ExplosionSpawnFn>(base + kExplosionSpawn);
		}
		else
			Log("sr3: explosion functions don't match this build; Minecraft explosions stay in Minecraft");
		static const std::uint8_t resolveHead[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10 };
		static const std::uint8_t damageHead[] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 };
		if (std::memcmp(reinterpret_cast<void*>(base + kHandleResolve), resolveHead, sizeof(resolveHead)) == 0 &&
			std::memcmp(reinterpret_cast<void*>(base + kHumanDamage), damageHead, sizeof(damageHead)) == 0)
		{
			g_objectTable = true;
			g_damage = reinterpret_cast<HumanDamageFn>(base + kHumanDamage);
			static const std::uint8_t igniteHead[] = { 0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
			if (std::memcmp(reinterpret_cast<void*>(base + kHumanIgnite), igniteHead, sizeof(igniteHead)) == 0)
				g_ignite = reinterpret_cast<HumanIgniteFn>(base + kHumanIgnite);
			else
				Log("sr3: ignite doesn't match this build; SR3 people don't catch Minecraft's fire");
			static const std::uint8_t ragdollHead[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x78, 0x10, 0x55 };
			if (std::memcmp(reinterpret_cast<void*>(base + kHumanRagdoll), ragdollHead, sizeof(ragdollHead)) == 0)
				g_ragdoll = reinterpret_cast<HumanRagdollFn>(base + kHumanRagdoll);
			else
				Log("sr3: ragdoll doesn't match this build; SR3 people don't fall over when hit");
		}
		else
			Log("sr3: human query/damage functions don't match this build; no fighting SR3 people");
		g_cam = reinterpret_cast<const std::uint8_t*>(base + kCamera);
		static const std::uint8_t camHead[] = { 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
		static const std::uint8_t incHead[] = { 0xFF, 0x05 };
		if (std::memcmp(reinterpret_cast<void*>(base + kPauseInc), incHead, 2) == 0 &&
			scan::Rip(base + kPauseInc + 2, base + kPauseInc + 6) == base + kPauseCount)
			g_pauseCount = reinterpret_cast<const std::int32_t*>(base + kPauseCount);
		else
			Log("sr3: pause counter doesn't match this build");
		static const std::uint8_t rayHead[] = { 0x48, 0x8B, 0xC4, 0x55 };
		static const std::uint8_t allocHead[] = { 0x48, 0x8D, 0x05 };
		if (std::memcmp(reinterpret_cast<void*>(base + kRaycast), rayHead, sizeof(rayHead)) == 0 &&
			std::memcmp(reinterpret_cast<void*>(base + kAllocator), allocHead, sizeof(allocHead)) == 0)
		{
			g_raycast = reinterpret_cast<RaycastFn>(base + kRaycast);
			g_allocator = reinterpret_cast<AllocatorFn>(base + kAllocator);
			g_free = reinterpret_cast<FreeFn>(base + kFree);
		}
		else
			Log("sr3: raycast doesn't match this build; no ground for Minecraft");
		void* camUpdate = reinterpret_cast<void*>(base + kCameraUpdate);
		if (std::memcmp(camUpdate, camHead, sizeof(camHead)) != 0 || MH_CreateHook(camUpdate, (void*)CameraUpdateHook, (void**)&g_cameraUpdateOrig) != MH_OK ||
			MH_EnableHook(camUpdate) != MH_OK)
			Log("sr3: camera update doesn't match this build; no first person");
		return true;
	}

	bool GetCamera(Camera& out)
	{
		if (!g_cam)
			return false;
		out.pos = V(g_cam + 0x60);
		out.right = V(g_cam + 0x80);
		out.up = V(g_cam + 0x90);
		out.forward = V(g_cam + 0xA0);
		std::memcpy(&out.fovDeg, g_cam + 0xE0, 4);
		// A rotation: unit, orthogonal rows. Anything else means this isn't the camera (wrong build).
		auto unit = [](const Vec3& v) { return std::fabs(Dot(v, v) - 1.0f) < 0.01f; };
		bool ok = Finite(out.pos) && unit(out.right) && unit(out.up) && unit(out.forward) && std::fabs(Dot(out.right, out.forward)) < 0.01f &&
			out.fovDeg > 5.0f && out.fovDeg < 170.0f;
		static bool warned;
		if (!ok && !warned)
		{
			warned = true;
			Log("sr3: camera at SRTTR+0x%llx doesn't look like a camera; off", (unsigned long long)kCamera);
		}
		return ok;
	}

	bool Explode(const Vec3& at, const char* type)
	{
		if (!g_explosionSpawn || !g_lookup)
			return false;
		void* info = g_explosionInfo(type);
		std::uint8_t* player = g_lookup("#PLAYER1#", 0);
		if (!info || !player)
			return false;
		// As explosion_create calls it: no exploding object, the player as owner, identity rotation, the
		// position, then its fixed flags; the zone word comes from an object near the spot (the player).
		std::uint16_t zone;
		std::memcpy(&zone, player + 0x92, 2);
		const float pos[3] = { at.x, at.y, at.z };
		const float* identity = reinterpret_cast<const float*>(scan::ImageBase() + kIdentity3x3);
		g_explosionSpawn(info, nullptr, player, identity, pos, nullptr, true, false, nullptr, nullptr, false, false, 1.0f, nullptr, &zone);
		return true;
	}

	namespace
	{
		// A live human, as get_num_humans_in_trigger checks: not flagged gone (+0x4b bits 0x10, 4), human type.
		bool IsHuman(const std::uint8_t* o)
		{
			if (!o || (o[0x4b] & 0x14) || o[0x4c] == 0xff)
				return false;
			const std::uint8_t* type = *reinterpret_cast<const std::uint8_t* const*>(scan::ImageBase() + kTypeTable + std::size_t(o[0x4c]) * 8);
			return type && (type[0xe] & 1);
		}

		// ...and not dead: hit points (+0x1ea8, found by damaging a pedestrian: 350 -> 340) above 0
		bool IsLiveHuman(const std::uint8_t* o)
		{
			float hp;
			return IsHuman(o) && (std::memcpy(&hp, o + kHumanHealth, 4), hp > 0.0f);
		}

		// Every object in the game's object table (main thread: objects come and go there).
		template <class F>
		void ForEachObject(F&& f)
		{
			const auto base = scan::ImageBase();
			const std::uint8_t* table = reinterpret_cast<const std::uint8_t*>(base + kHandleTable);
			std::uint32_t capacity;
			std::memcpy(&capacity, table + 8, 4);
			auto* const* slots = *reinterpret_cast<std::uint8_t* const* const*>(table + 0x10);
			std::uint8_t* empty = *reinterpret_cast<std::uint8_t* const*>(base + kEmptySlot);
			if (!slots || capacity > (1u << 20))
				return;
			for (std::uint32_t i = 0; i < capacity; i++)
				if (slots[i] && slots[i] != empty)
					f(slots[i]);
		}
	}

	namespace
	{
		// handle -> object as the game does it (0x48910 on the object table), then: a person or a vehicle?
		void ForEachObjectForHandle(std::uint64_t handle, bool& skip)
		{
			using ResolveFn = std::uint8_t* (*)(const void*, const std::uint64_t*, const void*);
			const auto base = scan::ImageBase();
			auto* o = reinterpret_cast<ResolveFn>(base + kHandleResolve)(reinterpret_cast<void*>(base + kHandleTable), &handle, reinterpret_cast<void*>(base + 0x28e4748));
			// (vehicles count as people here: they drive off, and would leave Minecraft blocks in the road)
			skip = o && (IsHuman(o) || o[0x4c] == kVehicleType);
		}
	}

	namespace
	{
		// A vehicle as the game's own checks have it (vehicle_stop_do): not gone, type flags +0xa bit 0x80.
		bool IsVehicle(const std::uint8_t* o)
		{
			if (!o || (o[0x4b] & 0x14) || o[0x4c] == 0xff)
				return false;
			const std::uint8_t* type = *reinterpret_cast<const std::uint8_t* const*>(scan::ImageBase() + kTypeTable + std::size_t(o[0x4c]) * 8);
			return type && (type[0xa] & 0x80);
		}

		std::uint8_t* Resolve(std::uint64_t handle)
		{
			using ResolveFn = std::uint8_t* (*)(const void*, const std::uint64_t*, const void*);
			const auto base = scan::ImageBase();
			return reinterpret_cast<ResolveFn>(base + kHandleResolve)(reinterpret_cast<void*>(base + kHandleTable), &handle, reinterpret_cast<void*>(base + 0x28e4748));
		}

		// An object's own handle (what the game's functions take): the 32-bit field the game resolves back to
		// that object, found once on the player.
		bool HandleOf(const std::uint8_t* o, std::uint64_t& handle)
		{
			static int field = -1;
			if (field < 0 && g_lookup)
			{
				std::uint8_t* player = g_lookup("#PLAYER1#", 0);
				for (int k = 0; player && k < 0x100 && field < 0; k += 4)
				{
					std::uint32_t v;
					std::memcpy(&v, player + k, 4);
					if (v && v != 0xffffffffu && Resolve(v) == player)
						field = k, Log("sr3: an object's handle is at +0x%x", k);
				}
				if (field < 0)
					field = 0x10000;  // (not found: never again)
			}
			if (field >= 0x10000)
				return false;
			std::uint32_t v;
			std::memcpy(&v, o + field, 4);
			handle = v;
			return Resolve(handle) == o;
		}
	}

	int NearbyVehicles(const Vec3& at, float radius, Vehicle* out, int max)
	{
		if (!g_objectTable)
			return 0;
		int count = 0;
		ForEachObject([&](std::uint8_t* o) {
			if (count >= max || !IsVehicle(o))
				return;
			Vec3 p = V(o + 0x60);
			float dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
			if (dx * dx + dy * dy + dz * dz <= radius * radius)
				out[count++] = { reinterpret_cast<std::uint64_t>(o), p, V(o + 0x84) };
		});
		return count;
	}

	bool StopVehicle(std::uint64_t vehicle)
	{
		static const std::uint8_t stopHead[] = { 0x48, 0x85, 0xC9, 0x0F, 0x84 };
		static const bool ok = std::memcmp(reinterpret_cast<void*>(scan::ImageBase() + kVehicleStop), stopHead, sizeof(stopHead)) == 0;
		std::uint8_t* target = nullptr;
		ForEachObject([&](std::uint8_t* o) {
			if (reinterpret_cast<std::uint64_t>(o) == vehicle && IsVehicle(o))
				target = o;
		});
		std::uint64_t handle;
		if (!ok || !target || !HandleOf(target, handle))
			return false;
		reinterpret_cast<void (*)(std::uint64_t, bool)>(scan::ImageBase() + kVehicleStop)(handle, true);
		return true;
	}

	int NearbyHumans(const Vec3& at, float radius, Human* out, int max)
	{
		if (!g_objectTable || !g_lookup)
			return 0;
		std::uint8_t* player = g_lookup("#PLAYER1#", 0);
		int count = 0;
		ForEachObject([&](std::uint8_t* o) {
			if (count >= max || o == player || !IsLiveHuman(o))
				return;
			Vec3 p = V(o + 0x60);
			float dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
			if (dx * dx + dy * dy + dz * dz <= radius * radius)
				out[count++] = { reinterpret_cast<std::uint64_t>(o), p };
		});
		return count;
	}

	namespace
	{
		// The id is the object's address: use it only if it's still in the table and still a live human.
		std::uint8_t* LiveHuman(std::uint64_t handle)
		{
			std::uint8_t* target = nullptr;
			ForEachObject([&](std::uint8_t* o) {
				if (reinterpret_cast<std::uint64_t>(o) == handle && IsLiveHuman(o))
					target = o;
			});
			return target;
		}
	}

	bool DamageHuman(std::uint64_t handle, float hitPoints)
	{
		std::uint8_t* target = g_damage && g_lookup ? LiveHuman(handle) : nullptr;
		std::uint8_t* player = g_lookup ? g_lookup("#PLAYER1#", 0) : nullptr;
		if (!target || !player)
			return false;
		// character_damage's flags: 0x100, and 0x200 = no on_damage script callbacks
		std::uint32_t flags = 0x100 | 0x200;
		g_damage(target, player, hitPoints, 0, 0, &flags);
		return true;
	}

	bool RagdollHuman(std::uint64_t handle, int ms)
	{
		std::uint8_t* target = g_ragdoll ? LiveHuman(handle) : nullptr;
		if (!target)
			return false;
		g_ragdoll(target, ms, -1.0f, nullptr, true, false);
		return true;
	}

	bool IgniteHuman(std::uint64_t handle)
	{
		std::uint8_t* target = g_ignite ? LiveHuman(handle) : nullptr;
		if (!target)
			return false;
		g_ignite(target, nullptr, nullptr, nullptr, false, false, true);
		return true;
	}

	bool GetPlayer(Player& out)
	{
		if (!g_lookup)
			return false;
		std::uint8_t* h = g_lookup("#PLAYER1#", 0);
		if (!h)
			return false;
		out.object = h;
		out.feet = V(h + 0x60);   // object position (get_object_pos reads the same)
		out.forward = V(h + 0x84);  // third row of the 3x3 orientation at +0x6c
		return Finite(out.feet);
	}

	namespace
	{
		// Ray filter: (object handle, user data, ?) -> keep the hit. 0 is the static world; people and vehicles never count.
		bool __fastcall RayFilter(std::uint64_t handle, void*, std::uint64_t)
		{
			if (!handle)
				return true;
			bool skip = false;
			ForEachObjectForHandle(handle, skip);
			return !skip;
		}
	}

	bool Raycast(const Vec3& from, const Vec3& to, float& fraction)
	{
		if (!g_raycast)
			return false;
		// The query as the game's callers fill it (0x1400b8a80, 0x14071c210): from/to as float4, filter
		// callback +0x68 with its data +0x60, collision filter +0x70, mode +0x74 (0: closest), layer +0x78.
		alignas(16) std::uint8_t q[0x80] = {};
		float* f = reinterpret_cast<float*>(q);
		f[0] = from.x, f[1] = from.y, f[2] = from.z;
		f[4] = to.x, f[5] = to.y, f[6] = to.z;
		static std::uint64_t ignore[4] = {};
		*reinterpret_cast<void**>(q + 0x60) = ignore;
		*reinterpret_cast<void**>(q + 0x68) = reinterpret_cast<void*>(&RayFilter);
		*reinterpret_cast<std::uint32_t*>(q + 0x70) = 0x50;
		struct Results { std::uint8_t* data; std::int32_t count; std::uint32_t capacity; } r{ nullptr, 0, 0x80000000u };
		bool hit = g_raycast(q, &r, true);
		bool ok = hit && r.count > 0 && r.data;
		// a hit (0x60 bytes): position +0x00 (and +0x10), surface normal +0x20, fraction along the ray +0x30
		if (ok)
			std::memcpy(&fraction, r.data + 0x30, 4);
		if (!(r.capacity & 0x80000000u) && r.data)
			g_free(g_allocator(), r.data, 0x50, r.capacity & 0x3fffffff);
		return ok && fraction >= 0 && fraction <= 1;
	}
}
