package dev.mcsr3;

import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.tags.TagKey;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.StateDefinition;
import net.minecraft.world.level.block.state.properties.BlockStateProperties;
import net.minecraft.world.level.block.state.properties.IntegerProperty;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.VoxelShape;
import net.minecraft.core.Registry;

/**
 * The top of the host's ground: an invisible, unbreakable layer 1-8 eighths of a block high (like snow layers), on
 * the barrier column under it. SR3's floors aren't on Minecraft's block grid; with these a half-metre step is half a
 * block (walkable) instead of a whole one, and mobs stand on the floor instead of up to half a block above it.
 */
public final class GroundBlock extends Block {
	public static final IntegerProperty LAYERS = BlockStateProperties.LAYERS;
	private static final VoxelShape[] SHAPES = new VoxelShape[9];

	static {
		for (int i = 0; i <= 8; i++) {
			SHAPES[i] = Block.box(0.0, 0.0, 0.0, 16.0, i * 2.0, 16.0);
		}
	}

	public static final Identifier ID = Identifier.fromNamespaceAndPath("passthrough", "ground");
	public static final Block BLOCK = Registry.register(BuiltInRegistries.BLOCK, ID, new GroundBlock(BlockBehaviour.Properties.of()
		.setId(ResourceKey.create(Registries.BLOCK, ID)).strength(-1.0F, 3600000.0F).noLootTable().noOcclusion()
		.isValidSpawn((state, level, pos, type) -> false)));

	private GroundBlock(final BlockBehaviour.Properties properties) {
		super(properties);
		this.registerDefaultState(this.stateDefinition.any().setValue(LAYERS, 8));
	}

	/** Load the class (registers the block): call once from the mod initializer. */
	static void register() {
	}

	/** The host's own ground (barriers and these): the block tag that the host's commands use too. */
	public static final TagKey<Block> HOST_GROUND = TagKey.create(Registries.BLOCK, Identifier.fromNamespaceAndPath("passthrough", "host_ground"));

	public static boolean isHostGround(final BlockState state) {
		return state.is(HOST_GROUND);
	}

	public static BlockState layers(final int eighths) {
		return BLOCK.defaultBlockState().setValue(LAYERS, Math.max(1, Math.min(8, eighths)));
	}

	@Override
	protected void createBlockStateDefinition(final StateDefinition.Builder<Block, BlockState> builder) {
		builder.add(LAYERS);
	}

	@Override
	protected VoxelShape getShape(final BlockState state, final BlockGetter level, final BlockPos pos, final CollisionContext context) {
		return SHAPES[state.getValue(LAYERS)];
	}

	@Override
	protected VoxelShape getCollisionShape(final BlockState state, final BlockGetter level, final BlockPos pos, final CollisionContext context) {
		return SHAPES[state.getValue(LAYERS)];
	}

	@Override
	protected RenderShape getRenderShape(final BlockState state) {
		return RenderShape.INVISIBLE;
	}
}
