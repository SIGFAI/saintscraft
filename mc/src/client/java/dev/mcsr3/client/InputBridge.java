package dev.mcsr3.client;

import com.google.gson.JsonObject;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonInfo;
import org.lwjgl.sdl.SDLKeyboard;

/**
 * Saints Row's keyboard and mouse, replayed into Minecraft's own input handlers as if its (off-screen) window
 * had the focus: hotbar keys, inventory, chat, menus, clicks and the cursor all work like real Minecraft.
 * A virtual keyboard answers InputConstants.isKeyDown. Adapted from SkyCraft's InputBridge (chasmlol, MIT).
 *
 * <p>{"t":"in","k":"key","sc":SDL scancode,"d":0|1}, {"t":"in","k":"btn","b":SDL button (1 L, 2 M, 3 R),"d":0|1},
 * {"t":"in","k":"scroll","v":notches}, {"t":"in","k":"cursor","x":px,"y":px}, {"t":"in","k":"text","c":code point},
 * {"t":"in","k":"release"}, {"t":"menu"}.
 */
public final class InputBridge {
	private static final boolean[] KEYS = new boolean[512];
	private static final boolean[] BUTTONS = new boolean[8];
	private static double cursorX, cursorY;
	private static int modifiers;

	private InputBridge() {
	}

	/** While a host is attached, Minecraft's input is ours, not SDL's. */
	public static boolean active() {
		return HostState.live() != null;
	}

	public static boolean isKeyDown(final int scancode) {
		return scancode >= 0 && scancode < KEYS.length && KEYS[scancode];
	}

	/** Client thread. */
	static void handle(final Minecraft minecraft, final JsonObject m) {
		long handle = minecraft.getWindow().handle();
		switch (m.get("k").getAsString()) {
			case "key" -> key(minecraft, handle, m.get("sc").getAsInt(), m.get("d").getAsInt() != 0);
			case "btn" -> {
				// Minecraft 26 numbers buttons like SDL (InputConstants.MOUSE_BUTTON_LEFT = 1, MIDDLE = 2, RIGHT = 3): as sent
				int b = m.get("b").getAsInt();
				boolean down = m.get("d").getAsInt() != 0;
				if (b > 0 && b < BUTTONS.length) {
					BUTTONS[b] = down;
				}

				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(b, modifiers), down ? 1 : 0);
			}
			case "scroll" -> minecraft.mouseHandler.onScroll(handle, 0.0, m.get("v").getAsDouble());
			case "cursor" -> {
				double x = m.get("x").getAsDouble(), y = m.get("y").getAsDouble();
				minecraft.mouseHandler.onMove(handle, x, y, x - cursorX, y - cursorY);
				cursorX = x;
				cursorY = y;
			}
			case "text" -> {
				if (minecraft.gui.screen() != null) {
					minecraft.keyboardHandler.textInput(handle, new String(Character.toChars(m.get("c").getAsInt())));
				}
			}
			case "release" -> releaseAll(minecraft);
			default -> {
			}
		}
	}

	static void openMenu(final Minecraft minecraft) {
		if (minecraft.gui.screen() == null && minecraft.player != null) {
			releaseAll(minecraft);
			minecraft.gui.setScreen(new PauseScreen(true));
		}
	}

	private static void key(final Minecraft minecraft, final long handle, final int scancode, final boolean down) {
		if (scancode <= 0 || scancode >= KEYS.length) {
			return;
		}

		boolean wasDown = KEYS[scancode];
		KEYS[scancode] = down;
		updateModifiers();
		int action = down ? (wasDown ? 2 : 1) : 0; // 2 = repeat
		int keycode = SDLKeyboard.SDL_GetKeyFromScancode(scancode, (short) modifiers, true);
		minecraft.keyboardHandler.keyPress(handle, action, new KeyEvent(scancode, keycode, modifiers));
	}

	private static void updateModifiers() {
		int m = 0;
		if (KEYS[225]) m |= 0x0001; // SDL_KMOD_LSHIFT
		if (KEYS[229]) m |= 0x0002; // SDL_KMOD_RSHIFT
		if (KEYS[224]) m |= 0x0040; // SDL_KMOD_LCTRL
		if (KEYS[228]) m |= 0x0080; // SDL_KMOD_RCTRL
		if (KEYS[226]) m |= 0x0100; // SDL_KMOD_LALT
		if (KEYS[230]) m |= 0x0200; // SDL_KMOD_RALT
		modifiers = m;
	}

	/** Lift every key and button we think is held (the host switched modes, or went away). */
	static void releaseAll(final Minecraft minecraft) {
		long handle = minecraft.getWindow().handle();
		for (int sc = 0; sc < KEYS.length; sc++) {
			if (KEYS[sc]) {
				KEYS[sc] = false;
				updateModifiers();
				minecraft.keyboardHandler.keyPress(handle, 0, new KeyEvent(sc, SDLKeyboard.SDL_GetKeyFromScancode(sc, (short) 0, true), modifiers));
			}
		}

		for (int b = 1; b < BUTTONS.length; b++) {
			if (BUTTONS[b]) {
				BUTTONS[b] = false;
				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(b, 0), 0);
			}
		}
	}
}
