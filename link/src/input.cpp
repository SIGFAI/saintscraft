#include "input.h"

#include "log.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <map>

namespace mcsr3::input
{
	namespace
	{
		decltype(&GetRawInputData) g_rawOrig;
		decltype(&GetKeyboardState) g_kbStateOrig;
		bool g_keyConsumed[256];  // per virtual key (mouse buttons too): Minecraft took its press, the game sees it up
		const UINT kButtonVk[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };

		// SR3 also reads the thread's key state, which Windows updates as each message is removed, before
		// FilterMessage turns it into WM_NULL: keys Minecraft has are cleared here too.
		BOOL WINAPI KeyboardStateHook(PBYTE keys)
		{
			BOOL ok = g_kbStateOrig(keys);
			if (ok && keys)
				for (int vk = 0; vk < 256; vk++)
					if (g_keyConsumed[vk])
						keys[vk] = 0;
			return ok;
		}
		std::function<bool(const Event&)> g_handler;
		std::atomic<int> g_rawMouse, g_rawKey, g_diState, g_diData;

		using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, void*);
		using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*);
		using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*, DWORD*, DWORD);
		GetStateFn g_stateOrig;
		GetDataFn g_dataOrig;

		bool Dispatch(const Event& e) { return g_handler && g_handler(e); }

		void ReportCounts()
		{
			static ULONGLONG last;
			if (GetTickCount64() - last < 10000)
				return;
			last = GetTickCount64();
			static int reports;
			if (reports++ < 6)
				Log("input: raw mouse %d, raw keyboard %d, DirectInput GetDeviceState %d, GetDeviceData %d", g_rawMouse.load(), g_rawKey.load(), g_diState.load(), g_diData.load());
		}

		// Raw input: rewrite what the game reads so a consumed event becomes a no-op.
		UINT WINAPI RawHook(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT headerSize)
		{
			UINT r = g_rawOrig(h, cmd, data, size, headerSize);
			ReportCounts();
			if (cmd != RID_INPUT || !data || r == UINT(-1))
				return r;
			auto* ri = static_cast<RAWINPUT*>(data);
			if (ri->header.dwType == RIM_TYPEMOUSE)
			{
				g_rawMouse++;
				USHORT& f = ri->data.mouse.usButtonFlags;
				static const USHORT down[5] = { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
				static const USHORT up[5] = { RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP };
				for (unsigned b = 0; b < 5; b++)
				{
					// like keys: whoever saw the press sees the release, and SR3's key state hides Minecraft's buttons
					bool& consumed = g_keyConsumed[kButtonVk[b]];
					if (f & down[b])
					{
						consumed = Dispatch({ Event::Button, b, 1 });
						if (consumed)
							f &= ~down[b];
					}
					if (f & up[b])
					{
						Dispatch({ Event::Button, b, 0 });
						if (consumed)
							f &= ~up[b];
						consumed = false;
					}
				}
				if ((f & RI_MOUSE_WHEEL) && Dispatch({ Event::Wheel, 0, SHORT(ri->data.mouse.usButtonData) / WHEEL_DELTA }))
				{
					f &= ~RI_MOUSE_WHEEL;
					ri->data.mouse.usButtonData = 0;
				}
				auto& m = ri->data.mouse;
				if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY) && Dispatch({ Event::Move, 0, int(m.lLastX), int(m.lLastY) }))
					m.lLastX = m.lLastY = 0;  // the camera stays put (a Minecraft screen has the cursor)
			}
			else if (ri->header.dwType == RIM_TYPEKEYBOARD)
			{
				g_rawKey++;
				RAWKEYBOARD& k = ri->data.keyboard;
				bool isDown = !(k.Flags & RI_KEY_BREAK);
				if (k.VKey && k.VKey != 0xFF && Dispatch({ Event::Key, k.VKey, isDown ? 1 : 0 }))
				{
					// 0xFF: "no key" (fake keys from multi-key sequences); the game skips it
					k.VKey = 0xFF;
					k.MakeCode = 0;
				}
			}
			return r;
		}

		// DirectInput (SR3 reads its keyboard here). Keys are DIK scan codes; events use virtual keys.
		struct DiObjectData
		{
			DWORD ofs, data, timeStamp, sequence;
			UINT_PTR appData;
		};
		bool g_consumed[256];  // per DIK: Minecraft took its last press, so the game keeps seeing it up
		std::map<void*, bool> g_isKeyboard;

		bool IsKeyboard(void* dev)
		{
			auto it = g_isKeyboard.find(dev);
			if (it != g_isKeyboard.end())
				return it->second;
			// IDirectInputDevice8::GetCapabilities (3): DIDEVCAPS, dwDevType low byte 0x13 = keyboard
			struct Caps { DWORD size, flags, devType, axes, buttons, povs, ffPeriod, ffRes, ffPeriodMin, fwRev, hwRev, driverVer; } caps{ sizeof(Caps) };
			using CapsFn = HRESULT(STDMETHODCALLTYPE*)(void*, Caps*);
			bool kb = SUCCEEDED((*static_cast<CapsFn**>(dev))[3](dev, &caps)) && (caps.devType & 0xFF) == 0x13;
			return g_isKeyboard[dev] = kb;
		}

		bool KeyEvent(DWORD dik, bool down)
		{
			UINT vk = MapVirtualKeyW(dik & 0x7F, MAPVK_VSC_TO_VK);
			if (dik == 0x41)  // F7 (no mapping surprises for the toggle key)
				vk = VK_F7;
			return vk && Dispatch({ Event::Key, vk, down ? 1 : 0 });
		}

		HRESULT STDMETHODCALLTYPE GetStateHook(void* dev, DWORD n, void* data)
		{
			HRESULT hr = g_stateOrig(dev, n, data);
			g_diState++;
			ReportCounts();
			if (FAILED(hr) || n != 256 || !data)
				return hr;
			static BYTE prev[256];
			auto* keys = static_cast<BYTE*>(data);
			for (int k = 0; k < 256; k++)
			{
				bool down = (keys[k] & 0x80) != 0;
				if (down != ((prev[k] & 0x80) != 0))
				{
					bool took = KeyEvent(k, down);
					g_consumed[k] = down ? took : false;
				}
				prev[k] = keys[k];
				if (g_consumed[k])
					keys[k] = 0;
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE GetDataHook(void* dev, DWORD n, void* data, DWORD* count, DWORD flags)
		{
			HRESULT hr = g_dataOrig(dev, n, data, count, flags);
			g_diData++;
			ReportCounts();
			if (FAILED(hr) || !data || !count || n != sizeof(DiObjectData) || (flags & 1) || !IsKeyboard(dev))  // 1 = DIGDD_PEEK
				return hr;
			auto* items = static_cast<DiObjectData*>(data);
			DWORD kept = 0;
			for (DWORD i = 0; i < *count; i++)
			{
				DWORD k = items[i].ofs & 0xFF;
				bool down = (items[i].data & 0x80) != 0;
				bool took = KeyEvent(k, down);
				if (down)
					g_consumed[k] = took;
				else if (g_consumed[k])
					took = true, g_consumed[k] = false;  // the release of a press the game never saw
				if (!took)
					items[kept++] = items[i];
			}
			*count = kept;
			return hr;
		}
	}

	namespace
	{
		// Windows scan code (set 1, lParam bits 16-23, extended bit 24) -> SDL scancode (USB HID usage).
		unsigned SdlScancode(unsigned scan, bool ext)
		{
			if (ext)
			{
				switch (scan)
				{
				case 0x1C: return 88;   // keypad enter
				case 0x1D: return 228;  // right ctrl
				case 0x35: return 84;   // keypad /
				case 0x38: return 230;  // right alt
				case 0x47: return 74;   // home
				case 0x48: return 82;   // up
				case 0x49: return 75;   // page up
				case 0x4B: return 80;   // left
				case 0x4D: return 79;   // right
				case 0x4F: return 77;   // end
				case 0x50: return 81;   // down
				case 0x51: return 78;   // page down
				case 0x52: return 73;   // insert
				case 0x53: return 76;   // delete
				default: return 0;
				}
			}
			static const unsigned char table[0x59] = {
				0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,       // 00 esc 1-0 - = bksp tab
				20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 224, 4, 22,       // 10 q-p [ ] enter lctrl a s
				7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 225, 49, 29, 27, 6, 25,        // 20 d-l ; ' ` lshift \ z x c v
				5, 17, 16, 54, 55, 56, 229, 85, 226, 44, 57, 58, 59, 60, 61, 62,     // 30 b n m , . / rshift kp* lalt space caps f1-f5
				63, 64, 65, 66, 67, 83, 71, 95, 96, 97, 86, 92, 93, 94, 87, 89,      // 40 f6-f10 numlock scroll kp7-9 kp- kp4-6 kp+ kp1
				90, 91, 98, 99, 0, 0, 100, 68, 69 };                                  // 50 kp2 kp3 kp0 kp. .. .. <> f11 f12
			return scan < sizeof(table) ? table[scan] : 0;
		}
	}

	bool FilterMessage(void* m)
	{
		auto* msg = static_cast<MSG*>(m);
		bool* consumed = g_keyConsumed;
		switch (msg->message)
		{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYUP:
		{
			unsigned vk = unsigned(msg->wParam) & 0xFF;
			bool down = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
			bool repeat = down && (msg->lParam & (1 << 30));
			Event e{ Event::Key, vk, down ? (repeat ? 2 : 1) : 0 };
			e.sdl = SdlScancode(unsigned(msg->lParam >> 16) & 0xFF, (msg->lParam >> 24) & 1);
			bool took = Dispatch(e);
			// whoever saw a key go down sees its release (no stuck keys across a mode switch)
			if (down && !repeat)
				consumed[vk] = took;
			bool swallow = consumed[vk];
			if (!down)
				consumed[vk] = false;
			if (swallow)
			{
				// Typed text (WM_CHAR, for Minecraft's chat and search boxes) is made from the key-down by the
				// game's TranslateMessage, which would only see WM_NULL: make it here first
				if (down)
					TranslateMessage(msg);
				msg->message = WM_NULL;
			}
			return swallow;
		}
		// SR3's window may also act on button messages: hide the ones whose press Minecraft has
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		{
			UINT wm = msg->message;
			UINT vk = wm <= WM_LBUTTONDBLCLK ? VK_LBUTTON : wm <= WM_RBUTTONDBLCLK ? VK_RBUTTON : wm <= WM_MBUTTONDBLCLK ? VK_MBUTTON
				: HIWORD(msg->wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
			// (the raw release may clear the flag first: a release SR3 never saw pressed is harmless)
			if (!consumed[vk])
				return false;
			msg->message = WM_NULL;
			return true;
		}
		case WM_CHAR:
			if (Dispatch({ Event::Char, unsigned(msg->wParam), 0 }))
			{
				msg->message = WM_NULL;
				return true;
			}
			return false;
		default:
			return false;
		}
	}

	void SetHandler(std::function<bool(const Event&)> handler) { g_handler = std::move(handler); }

	void OnDirectInput8(void* di8)
	{
		if (!di8 || g_stateOrig)
			return;
		// SR3 made its devices already; one device vtable serves them all, so make a keyboard of our
		// own to reach it. IDirectInput8: 3 CreateDevice; IDirectInputDevice8: 9 GetDeviceState, 10 GetDeviceData.
		static const GUID kSysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
		void* dev = nullptr;
		if (FAILED((*static_cast<CreateDeviceFn**>(di8))[3](di8, kSysKeyboard, &dev, nullptr)) || !dev)
		{
			Log("input: no DirectInput keyboard");
			return;
		}
		void** vt = *static_cast<void***>(dev);
		bool ok = MH_CreateHook(vt[9], (void*)GetStateHook, (void**)&g_stateOrig) == MH_OK && MH_CreateHook(vt[10], (void*)GetDataHook, (void**)&g_dataOrig) == MH_OK &&
			MH_EnableHook(vt[9]) == MH_OK && MH_EnableHook(vt[10]) == MH_OK;
		// IUnknown::Release (2)
		using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void*);
		(*static_cast<ReleaseFn**>(dev))[2](dev);
		Log("input: DirectInput devices %s", ok ? "hooked" : "NOT hooked");
	}

	bool Install()
	{
		if (MH_CreateHook((void*)&GetKeyboardState, (void*)KeyboardStateHook, (void**)&g_kbStateOrig) != MH_OK || MH_EnableHook((void*)&GetKeyboardState) != MH_OK)
			Log("input: hooking GetKeyboardState failed");
		if (MH_CreateHook((void*)&GetRawInputData, (void*)RawHook, (void**)&g_rawOrig) != MH_OK || MH_EnableHook((void*)&GetRawInputData) != MH_OK)
		{
			Log("input: hooking GetRawInputData failed");
			return false;
		}
		return true;
	}
}
