package dev.mcsr3.mixin;

import dev.mcsr3.WorldBridge;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.projectile.FireworkRocketEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** A crossbow's firework bursting (on a person, a block, or at the end of its flight) is an explosion in the host too. */
@Mixin(FireworkRocketEntity.class)
abstract class FireworkMixin {
	@Inject(method = "explode(Lnet/minecraft/server/level/ServerLevel;)V", at = @At("HEAD"))
	private void passthrough$burst(final ServerLevel level, final CallbackInfo ci) {
		FireworkRocketEntity self = (FireworkRocketEntity) (Object) this;
		if (self.isShotAtAngle()) {
			WorldBridge.onExplosion(self.position(), 2.0F, "firework_rocket");
		}
	}
}
