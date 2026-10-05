#pragma once
#include <cstdint>
#include <functional>

// Saints Row: The Third Remastered game state, in SR3's own space: metres, Y up, left-handed
// (x right, y up, z forward, like D3D). Found by reverse engineering (docs/RE.md); main thread only.
namespace mcsr3::sr3
{
	struct Vec3
	{
		float x, y, z;
	};

	struct Camera
	{
		Vec3 pos;
		Vec3 right, up, forward;  // unit rows of the view rotation
		float fovDeg;             // as stored: horizontal at a 4:3 aspect (Hor+; measured: 50 -> projection ys 2.859)
		float verticalFovDeg() const;
	};

	struct Player
	{
		Vec3 feet;
		Vec3 forward;  // body facing
		std::uint8_t* object;
	};

	bool Init();  // false (logged) if this build doesn't match what docs/RE.md describes
	bool GetCamera(Camera& out);
	bool GetPlayer(Player& out);  // needs a loaded save
	// An SR3 explosion at a point, by type name from misc_tables explosions.xtbl ("Grenade", "Satchel",
	// ...), owned by the player (it counts as theirs: notoriety, kills). Main thread.
	bool Explode(const Vec3& at, const char* type);

	struct Human
	{
		std::uint64_t handle;  // the object's address: checked against the object table before any use
		Vec3 feet;
	};
	// Humans (pedestrians, gang members, cops; not the player) within `radius` of `at`. Main thread.
	int NearbyHumans(const Vec3& at, float radius, Human* out, int max);
	struct Vehicle
	{
		std::uint64_t handle;  // the object's address, like Human's
		Vec3 pos, forward;
	};
	// Vehicles (parked, driven, the player's own) within `radius` of `at`. Main thread.
	int NearbyVehicles(const Vec3& at, float radius, Vehicle* out, int max);
	// Brings a vehicle to a stop (as vehicle_stop does for a scripted one). Main thread.
	bool StopVehicle(std::uint64_t handle);

	// Hit points of damage to a human, from the player (as character_damage does it). Main thread.
	bool DamageHuman(std::uint64_t handle, float hitPoints);
	// Sets a human on fire (as character_ignite does). Main thread.
	bool IgniteHuman(std::uint64_t handle);
	// Knocks a human over for `ms` (as character_ragdoll does). Main thread.
	bool RagdollHuman(std::uint64_t handle, int ms);

	// First person: the game's camera is moved to the player's eyes each frame (its rotation is kept).
	void SetFirstPerson(bool on);
	// First person looks up and down by itself (SR3's orbit camera stops short of the floor): mouse counts, + = down
	void FirstPersonLook(int dy);
	// While Minecraft drives the player: first person's eyes go here (feet) instead of the Boss's (null = the Boss)
	void SetDriveFeet(const Vec3* feet);
	// Puts the player's feet here now (his physics body: no teleport, no fade). Main thread.
	bool PlacePlayer(const Vec3& feet);
	bool FirstPerson();
	void UnhideForUpdate();  // top of each frame (main thread): the player's update must see them shown

	// The player and nearby people, refreshed on the main thread (UpdateSnapshot) for other threads (the
	// render thread runs Present) that mustn't call into the game.
	struct Snapshot
	{
		bool valid = false;
		Vec3 feet{};
		int humans = 0;
		Vec3 humanFeet[128]{};
	};
	void UpdateSnapshot();
	Snapshot GetSnapshot();

	// The game is paused (pause menu, map, phone...): its pause counter is non-zero. Any thread.
	bool Paused();

	// Called inside the game's own frame update (after its camera update, where its camera raycasts):
	// the place where raycasts are safe. Main thread.
	void OnSafePoint(std::function<void()> fn);
	// Closest static-world hit from `from` to `to` (people are never hit): fraction 0..1 along the ray.
	// Safe-point only.
	bool Raycast(const Vec3& from, const Vec3& to, float& fraction);
}
