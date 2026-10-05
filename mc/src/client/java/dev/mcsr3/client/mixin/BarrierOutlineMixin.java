package dev.mcsr3.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.state.level.LevelRenderState;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Saints Row's ground is invisible barrier blocks: no selection outline on it (only on blocks you built). */
@Mixin(LevelRenderer.class)
abstract class BarrierOutlineMixin {
	@Inject(method = "submitBlockOutline", at = @At("HEAD"), cancellable = true)
	private void mcsr3$noBarrierOutline(final PoseStack pose, final SubmitNodeCollector collector, final LevelRenderState state, final CallbackInfo ci) {
		Minecraft minecraft = Minecraft.getInstance();
		if (minecraft.level != null && minecraft.hitResult instanceof BlockHitResult hit && hit.getType() == HitResult.Type.BLOCK
			&& dev.mcsr3.GroundBlock.isHostGround(minecraft.level.getBlockState(hit.getBlockPos()))) {
			ci.cancel();
		}
	}
}
