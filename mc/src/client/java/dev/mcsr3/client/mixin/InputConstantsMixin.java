package dev.mcsr3.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.mcsr3.client.InputBridge;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** While Saints Row is attached, the keyboard state is its, and the real mouse is never grabbed (SkyCraft's approach). */
@Mixin(InputConstants.class)
abstract class InputConstantsMixin {
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void mcsr3$isKeyDown(final int key, final CallbackInfoReturnable<Boolean> cir) {
		if (InputBridge.active()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void mcsr3$grabMouse(final Window window, final double x, final double y, final CallbackInfo ci) {
		if (InputBridge.active()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void mcsr3$releaseMouse(final Window window, final double x, final double y, final CallbackInfo ci) {
		if (InputBridge.active()) {
			ci.cancel();
		}
	}
}
