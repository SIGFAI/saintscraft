package dev.mcsr3.client.mixin;

import dev.mcsr3.client.InputBridge;
import net.minecraft.client.gui.screens.Screen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Saints Row keeps running while a Minecraft screen is open, so the Minecraft world does too. */
@Mixin(Screen.class)
abstract class ScreenMixin {
	@Inject(method = "isPauseScreen", at = @At("HEAD"), cancellable = true)
	private void mcsr3$neverPause(final CallbackInfoReturnable<Boolean> cir) {
		if (InputBridge.active()) {
			cir.setReturnValue(false);
		}
	}
}
