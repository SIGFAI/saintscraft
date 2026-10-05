package dev.mcsr3.mixin;

import dev.mcsr3.MobWar;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.npc.villager.Villager;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Right-clicking an SR3 person (their invisible villager proxy) never opens a trading screen: the click passes on
 * to the held item (blocks, bow, flint and steel). On the client a proxy is any invisible villager (the tag is server-side).
 */
@Mixin(Villager.class)
abstract class ProxyInteractMixin {
	@Inject(method = "mobInteract", at = @At("HEAD"), cancellable = true)
	private void mcsr3$noTrading(final Player player, final InteractionHand hand, final CallbackInfoReturnable<InteractionResult> cir) {
		Villager self = (Villager) (Object) this;
		if (MobWar.isProxy(self) || (self.level().isClientSide() && self.isInvisible())) {
			cir.setReturnValue(InteractionResult.PASS);
		}
	}
}
