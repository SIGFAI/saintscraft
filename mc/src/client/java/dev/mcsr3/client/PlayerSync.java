package dev.mcsr3.client;

import net.minecraft.client.CameraType;
import net.minecraft.core.BlockPos;
import net.minecraft.client.Minecraft;
import dev.mcsr3.Passthrough;
import java.util.Locale;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.player.Abilities;
import net.minecraft.world.phys.Vec3;

/** Keeps the Minecraft player on the host's player: it stands where they stand and looks where the host camera looks. */
public final class PlayerSync {
	private static final double TELEPORT_SQ = 64.0 * 64.0;
	/** How far the host's player moved over the last client tick (drives the walk animation). */
	private static float tickDistance;
	private static double lastX = Double.NaN, lastZ;

	private PlayerSync() {
	}

	public static float tickDistance() {
		return tickDistance;
	}

	/** The host found Minecraft's player far from theirs (a respawn, a fall): back to the host's, and take over again. */
	public static void resync(final double x, final double y, final double z) {
		LocalPlayer player = Minecraft.getInstance().player;
		if (player != null) {
			Passthrough.playerDrives = false;
			hold(player, x, y, z);
		}
	}

	private static void hold(final LocalPlayer player, final double x, final double y, final double z) {
		player.setPos(x, y, z);
		player.xo = player.xOld = x;
		player.yo = player.yOld = y;
		player.zo = player.zOld = z;
		player.setDeltaMovement(Vec3.ZERO);
		if (!player.getAbilities().flying) {
			player.getAbilities().flying = true;
			player.onUpdateAbilities();
		}
	}

	/** The chunk is here and something solid (the host's ground as barriers) is within 3 blocks under the feet. */
	private static boolean groundBelow(final Minecraft minecraft, final LocalPlayer player) {
		BlockPos feet = player.blockPosition();
		if (minecraft.level == null || !minecraft.level.hasChunkAt(feet)) {
			return false;
		}

		for (int dy = 0; dy <= 3; dy++) {
			if (!minecraft.level.getBlockState(feet.below(dy)).isAir()) {
				return true;
			}
		}

		return false;
	}

	/** Every frame, before the camera update: position, rotation, and first/third person to match the host. */
	public static void frame(final float partialTick) {
		HostState.Pose p = HostState.frame();
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		if (p == null || player == null) {
			return;
		}

		if (!p.drive() && Passthrough.playerDrives) {
			Passthrough.playerDrives = false;
		}

		if (p.drive() && !Passthrough.playerDrives) {
			// The host wants Minecraft to drive: hover on the host's player until the ground under them has reached
			// this client (else the player drops through unloaded chunks into the void), then walk from there.
			hold(player, p.px(), p.py(), p.pz());
			if (groundBelow(minecraft, player)) {
				Passthrough.playerDrives = true;
				player.getAbilities().flying = false;
				player.onUpdateAbilities();
			}
		}

		if (p.drive()) {
			// Minecraft moves the player (its walking, jumping, sprinting and creative flight): it only takes where
			// to look, and tells the host where it is
			player.setInvisible(!p.show());
			player.setYRot(p.yaw());
			player.setXRot(p.pitch());
			player.yRotO = p.yaw();
			player.xRotO = p.pitch();
			player.yHeadRot = player.yHeadRotO = p.yaw();
			CameraType cameraType = p.firstPerson() ? CameraType.FIRST_PERSON : CameraType.THIRD_PERSON_BACK;
			if (minecraft.options.getCameraType() != cameraType) {
				minecraft.options.setCameraType(cameraType);
			}

			if (!Passthrough.playerDrives) {
				return; // not taken over yet: the host isn't told to follow
			}

			// where the player is drawn this frame (feet): the host puts the Boss and its camera there
			Vec3 at = player.getPosition(partialTick);
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"mcpos\",\"pos\":[%.4f,%.4f,%.4f]}", at.x, at.y, at.z));
			return;
		}

		player.setInvisible(!p.show());
		player.setYRot(p.yaw());
		player.setXRot(p.pitch());
		player.yRotO = p.yaw();
		player.xRotO = p.pitch();
		player.yHeadRot = player.yHeadRotO = p.yaw();
		player.yBodyRot = player.yBodyRotO = p.firstPerson() ? p.yaw() : p.bodyYaw();
		// the model stands exactly where the host's player is this frame (not a tick behind, interpolating)
		double x = p.firstPerson() ? p.x() : p.px();
		double y = p.firstPerson() ? p.y() - player.getEyeHeight() : p.py();
		double z = p.firstPerson() ? p.z() : p.pz();
		player.setPos(x, y, z);
		player.xo = player.xOld = x;
		player.yo = player.yOld = y;
		player.zo = player.zOld = z;
		CameraType cameraType = p.firstPerson() ? CameraType.FIRST_PERSON : CameraType.THIRD_PERSON_BACK;
		if (minecraft.options.getCameraType() != cameraType) {
			minecraft.options.setCameraType(cameraType);
		}
	}

	/**
	 * Every client tick, at the start of the player's tick (the old position is already saved, so the model
	 * interpolates and walks): in first person the player's eyes are at the host camera, in third person
	 * their feet are at the host player's.
	 */
	public static void tick(final LocalPlayer player) {
		HostState.Pose p = HostState.live();
		if (p == null || p.drive()) {
			return;
		}

		double x = p.firstPerson() ? p.x() : p.px();
		double y = p.firstPerson() ? p.y() - player.getEyeHeight() : p.py();
		double z = p.firstPerson() ? p.z() : p.pz();
		tickDistance = Double.isNaN(lastX) ? 0.0F : (float) Math.min(Math.hypot(x - lastX, z - lastZ), 1.0);
		lastX = x;
		lastZ = z;
		boolean teleport = player.distanceToSqr(x, y, z) > TELEPORT_SQ;
		player.setPos(x, y, z);
		if (teleport) {
			player.xo = player.xOld = x;
			player.yo = player.yOld = y;
			player.zo = player.zOld = z;
		}

		player.setDeltaMovement(Vec3.ZERO);
		Abilities abilities = player.getAbilities();
		if (abilities.mayfly && !abilities.flying) {
			abilities.flying = true;
			player.onUpdateAbilities();
		}
	}
}
