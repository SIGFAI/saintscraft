package dev.mcsr3.mixin;

import dev.mcsr3.MobWar;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Host people's proxies: hits on them go to the host instead of doing damage (enchantments' effects still apply); they neither push nor get pushed
 * (they follow their person every tick). Steve's crosshair targets them like any mob, so his sword hits SR3's people.
 */
@Mixin(LivingEntity.class)
abstract class ProxyMixin {
	@Inject(method = "hurtServer(Lnet/minecraft/server/level/ServerLevel;Lnet/minecraft/world/damagesource/DamageSource;F)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$proxyHurt(final ServerLevel level, final DamageSource source, final float amount, final CallbackInfoReturnable<Boolean> cir) {
		LivingEntity self = (LivingEntity) (Object) this;
		if (MobWar.isProxy(self)) {
			MobWar.onProxyHit(self, source, amount);
			// "hurt" without the damage (the host applies it): the attacker's after-hit effects still run, so a fire
			// aspect sword or a flame bow sets the proxy (and so the real person) on fire
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "isPushable()Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$proxyNotPushable(final CallbackInfoReturnable<Boolean> cir) {
		if (MobWar.isProxy((LivingEntity) (Object) this)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(method = "pushEntities()V", at = @At("HEAD"), cancellable = true)
	private void passthrough$proxyNoPush(final CallbackInfo ci) {
		if (MobWar.isProxy((LivingEntity) (Object) this)) {
			ci.cancel();
		}
	}
}
