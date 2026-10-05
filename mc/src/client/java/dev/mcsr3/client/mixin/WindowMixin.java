package dev.mcsr3.client.mixin;

import com.mojang.blaze3d.platform.Window;
import dev.mcsr3.client.InputBridge;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Saints Row has the real focus; Minecraft's off-screen window acts focused while it's attached. */
@Mixin(Window.class)
abstract class WindowMixin {
	@Inject(method = "isFocused", at = @At("HEAD"), cancellable = true)
	private void mcsr3$focused(final CallbackInfoReturnable<Boolean> cir) {
		if (InputBridge.active()) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "isIconified", at = @At("HEAD"), cancellable = true)
	private void mcsr3$notIconified(final CallbackInfoReturnable<Boolean> cir) {
		if (InputBridge.active()) {
			cir.setReturnValue(false);
		}
	}
}
