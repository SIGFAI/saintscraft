package dev.mcsr3;

import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.FireworkRocketEntity;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.entity.projectile.throwableitemprojectile.ThrownEnderpearl;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.TntBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/** Server-side half: the host's collision as invisible barrier blocks, commands, and events back to the host. */
public final class WorldBridge {
	private static volatile MinecraftServer server;
	/** Barriers we placed (so a reset only removes ours, never the player's builds). */
	private static final Set<BlockPos> barriers = ConcurrentHashMap.newKeySet();
	/** Block changes this tick (server thread only): true = now solid, false = gone. Sent at the end of the tick. */
	private static final Map<BlockPos, Boolean> changes = new LinkedHashMap<>();
	/** While placing or removing the host's own ground: those changes aren't news to the host (server thread only). */
	private static boolean placingGround;

	private WorldBridge() {
	}

	static void attach(final MinecraftServer s) {
		server = s;
	}

	static void detach() {
		server = null;
		barriers.clear();
	}

	public static boolean ready() {
		return server != null;
	}

	static MinecraftServer server() {
		return server;
	}

	/**
	 * Columns of solid ground from the host: {x, z, yBottom, top, ...}: barriers from block yBottom up to the surface at
	 * top/8 (eighths of a block), the last part a ground layer (GroundBlock). Only air is replaced.
	 */
	public static void solid(final int[] columns) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			BlockState barrier = Blocks.BARRIER.defaultBlockState();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			placingGround = true;
			for (int i = 0; i + 3 < columns.length; i += 4) {
				int top = columns[i + 3], full = Math.floorDiv(top, 8), part = Math.floorMod(top, 8);
				for (int y = columns[i + 2]; y <= full; y++) {
					BlockState state = y < full ? barrier : part > 0 ? GroundBlock.layers(part) : null;
					pos.set(columns[i], y, columns[i + 1]);
					if (state != null && level.isInWorldBounds(pos) && level.getBlockState(pos).isAir()) {
						level.setBlock(pos, state, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
						barriers.add(pos.immutable());
					}
				}
			}

			placingGround = false;
		});
	}

	/** How far around the player a clear also sweeps up host ground it didn't place this session (blocks). */
	private static final int CLEAR_RADIUS = 48, CLEAR_BELOW = 48, CLEAR_ABOVE = 16;

	/** Server thread: every host ground block in the box around {@code c}; sections without any are skipped. */
	private static int sweep(final ServerLevel level, final BlockPos c) {
		int removed = 0, y0 = c.getY() - CLEAR_BELOW, y1 = c.getY() + CLEAR_ABOVE;
		BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
		for (int cx = (c.getX() - CLEAR_RADIUS) >> 4; cx <= (c.getX() + CLEAR_RADIUS) >> 4; cx++) {
			for (int cz = (c.getZ() - CLEAR_RADIUS) >> 4; cz <= (c.getZ() + CLEAR_RADIUS) >> 4; cz++) {
				LevelChunk chunk = level.getChunk(cx, cz);
				for (int sy = Math.max(y0, level.getMinY()) >> 4; sy <= Math.min(y1, level.getMaxY()) >> 4; sy++) {
					LevelChunkSection section = chunk.getSection(chunk.getSectionIndexFromSectionY(sy));
					if (section.hasOnlyAir() || !section.maybeHas(GroundBlock::isHostGround)) {
						continue;
					}

					for (int y = Math.max(y0, sy << 4); y <= Math.min(y1, (sy << 4) + 15); y++) {
						for (int x = 0; x < 16; x++) {
							for (int z = 0; z < 16; z++) {
								if (removeHostGround(level, pos.set((cx << 4) + x, y, (cz << 4) + z))) {
									removed++;
								}
							}
						}
					}
				}
			}
		}

		return removed;
	}

	/** Server thread. */
	private static boolean removeHostGround(final ServerLevel level, final BlockPos pos) {
		if (!level.isInWorldBounds(pos) || !GroundBlock.isHostGround(level.getBlockState(pos))) {
			return false;
		}

		level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
		return true;
	}

	/** Ranges of the host's ground to take away: {x, z, y0, y1, ...} (a floor that moved, a wall that's gone). */
	public static void unsolid(final int[] ranges) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			placingGround = true;
			for (int i = 0; i + 3 < ranges.length; i += 4) {
				for (int y = ranges[i + 2]; y <= ranges[i + 3]; y++) {
					removeHostGround(level, pos.set(ranges[i], y, ranges[i + 1]));
				}
			}

			placingGround = false;
		});
	}

	/**
	 * Remove every barrier we placed (e.g. when the host teleports somewhere else), and with {@code around}, all host
	 * ground near there: earlier sessions' (saved with the world) would box the player in.
	 */
	public static void clearSolid(final BlockPos around) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			placingGround = true;
			for (BlockPos pos : barriers) {
				removeHostGround(level, pos);
			}

			if (around != null) {
				Passthrough.LOG.info("cleared {} old host ground blocks around {}", sweep(level, around), around);
			}

			placingGround = false;
			barriers.clear();
		});
	}

	/** Run a command as the server (op). Results go to the log, not to chat (send_command_feedback is off). */
	public static void command(final String command) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			Passthrough.LOG.info("command: {}", command);
			s.getCommands().performPrefixedCommand(s.createCommandSourceStack(), command);
		});
	}

	private static boolean solidForHost(final ServerLevel level, final BlockPos pos, final BlockState state) {
		// the Nether's ground is the host's own ground turned: nothing to collide with that isn't there already
		return !state.isAir() && !GroundBlock.isHostGround(state) && !state.getCollisionShape(level, pos).isEmpty() && !Nether.isGround(pos);
	}

	/** Arrows the host already hit something with (they stay where they hit and aren't reported again). */
	private static final String HIT_TAG = "passthrough_hit";

	/** Every server tick: block changes, and projectiles in flight for the host to trace through its own world. */
	static void tick(final MinecraftServer s) {
		flush(s);
		if (Passthrough.active) {
			reportProjectiles(s.overworld());
		}

		MobWar.tick(s);
		Nether.tick(s);
	}

	/**
	 * Steve's arrows (bow, crossbow), crossbow fireworks and ender pearls in flight: {"t":"proj","p":[[id,kind,x,y,z],...]}. Mobs'
	 * arrows aren't traced by the host: they hit its people's proxies here.
	 */
	private static void reportProjectiles(final ServerLevel level) {
		StringBuilder b = null;
		for (Entity e : level.getAllEntities()) {
			String kind = null;
			if (!(e instanceof Projectile projectile) || !(projectile.getOwner() instanceof Player)) {
				continue;
			}

			if (e instanceof AbstractArrow arrow && !arrow.entityTags().contains(HIT_TAG) && arrow.getDeltaMovement().lengthSqr() > 1.0E-4) {
				kind = "arrow";
			} else if (e instanceof FireworkRocketEntity rocket && rocket.isShotAtAngle()) {
				kind = "firework"; // not the ones boosting an elytra flight
			} else if (e instanceof ThrownEnderpearl) {
				kind = "pearl"; // the host lands it on its own world (past the ground Minecraft has) and moves the player there
			}

			if (kind != null) {
				b = b == null ? new StringBuilder("{\"t\":\"proj\",\"p\":[") : b.append(',');
				b.append(String.format(Locale.ROOT, "[%d,\"%s\",%.3f,%.3f,%.3f]", e.getId(), kind, e.getX(), e.getY(), e.getZ()));
			}
		}

		if (b != null) {
			Passthrough.events.accept(b.append("]}").toString());
		}
	}

	/**
	 * The host traced a projectile into something of its own: a firework bursts there; an arrow goes into a person
	 * or car (gone) or sticks where it hit a wall.
	 */
	public static void projectileHit(final int id, final double x, final double y, final double z, final boolean stick) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			Entity e = level.getEntity(id);
			if (e instanceof FireworkRocketEntity) {
				e.setPos(x, y, z);
				level.broadcastEntityEvent(e, (byte) 17);
				onExplosion(e.position(), 2.0F, "firework_rocket"); // (bursting on the host's own wall)
				e.discard();
			} else if (e instanceof ThrownEnderpearl) {
				e.discard(); // the host moved the player to where it landed
			} else if (e instanceof AbstractArrow) {
				if (stick) {
					e.setPos(x, y, z);
					e.setDeltaMovement(Vec3.ZERO);
					e.setNoGravity(true);
					e.addTag(HIT_TAG);
				} else {
					e.discard();
				}
			}
		});
	}

	/** Minecraft damage of one host bullet (half-hearts). */
	private static final float SHOT_DAMAGE = 6.0F;

	/**
	 * The host's player fired a gun along (from, dir), and its own world stops the bullet after maxDist: the first
	 * mob on the way is hit, or the first solid block (TNT goes off). The host's invisible ground doesn't stop it.
	 */
	public static void shot(final Vec3 from, final Vec3 dir, final double maxDist) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			Vec3 d = dir.normalize();
			double hitAt = maxDist;
			BlockPos tnt = null, last = null;
			for (double t = 0; t < maxDist; t += 0.1) {
				BlockPos p = BlockPos.containing(from.add(d.scale(t)));
				if (p.equals(last) || !level.hasChunkAt(p)) {
					continue;
				}

				last = p;
				BlockState state = level.getBlockState(p);
				if (state.isAir() || GroundBlock.isHostGround(state) || state.getCollisionShape(level, p).isEmpty()) {
					continue;
				}

				hitAt = t;
				if (state.getBlock() instanceof TntBlock) {
					tnt = p;
				}

				break;
			}

			Vec3 end = from.add(d.scale(hitAt));
			LivingEntity mob = null;
			double nearest = hitAt;
			for (Entity e : level.getEntities((Entity) null, new AABB(from.x, from.y, from.z, end.x, end.y, end.z).inflate(1.0),
				e -> e instanceof Mob && e.isAlive() && !MobWar.isProxy(e))) {
				var hit = e.getBoundingBox().inflate(0.2).clip(from, end);
				if (hit.isPresent() && hit.get().distanceTo(from) < nearest) {
					nearest = hit.get().distanceTo(from);
					mob = (LivingEntity) e;
				}
			}

			ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
			if (mob != null) {
				mob.hurtServer(level, player != null ? level.damageSources().playerAttack(player) : level.damageSources().generic(), SHOT_DAMAGE);
			} else if (tnt != null && TntBlock.prime(level, tnt)) {
				level.removeBlock(tnt, false);
			}
		});
	}

	/** Server thread, from Level.setBlock: remember the change; flushed once per tick. */
	public static void onBlockChanged(final ServerLevel level, final BlockPos pos, final BlockState state) {
		if (!Passthrough.active || level != level.getServer().overworld()) {
			return;
		}

		Nether.onBlockChanged(pos, state);
		if (!placingGround) {
			changes.put(pos.immutable(), solidForHost(level, pos, state));
		}
	}

	/** While on, block changes are the host's own ground being edited (not reported as blocks to collide with). */
	static void quietGround(final boolean on) {
		placingGround = on;
	}

	/** End of each server tick: {"t":"blocks","set":[x,y,z,...],"clear":[x,y,z,...]}. */
	static void flush(final MinecraftServer s) {
		if (changes.isEmpty()) {
			return;
		}

		StringBuilder set = new StringBuilder();
		StringBuilder clear = new StringBuilder();
		for (Map.Entry<BlockPos, Boolean> e : changes.entrySet()) {
			StringBuilder b = e.getValue() ? set : clear;
			BlockPos p = e.getKey();
			b.append(b.isEmpty() ? "" : ",").append(p.getX()).append(',').append(p.getY()).append(',').append(p.getZ());
		}

		changes.clear();
		Passthrough.events.accept("{\"t\":\"blocks\",\"set\":[" + set + "],\"clear\":[" + clear + "]}");
	}

	/** Every solid block within `radius` of the player, as one "blocks" message (the host's props start from this). */
	public static void sync(final int radius) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
			if (player == null) {
				return;
			}

			BlockPos c = player.blockPosition();
			BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
			int found = 0;
			for (int x = -radius; x <= radius && found < 3000; x++) {
				for (int z = -radius; z <= radius && found < 3000; z++) {
					for (int y = -24; y <= 40; y++) {
						p.set(c.getX() + x, c.getY() + y, c.getZ() + z);
						if (!level.isInWorldBounds(p) || !level.isLoaded(p)) {
							continue;
						}

						BlockState state = level.getBlockState(p);
						if (solidForHost(level, p, state)) {
							changes.put(p.immutable(), true);
							found++;
						}
					}
				}
			}

			flush(s);
		});
	}

	/** `source`: what exploded or was blown up, e.g. "tnt", "creeper", "fireball" (a ghast's). */
	public static void onExplosion(final Vec3 center, final float radius, final String source) {
		if (Passthrough.active) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"explosion\",\"pos\":[%.3f,%.3f,%.3f],\"r\":%.2f,\"src\":\"%s\"}",
				center.x, center.y, center.z, radius, source));
		}
	}
}
