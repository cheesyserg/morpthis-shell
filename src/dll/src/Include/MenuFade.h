#pragma once

namespace Morpthis
{
	namespace Shell
	{
		// Fade-in for popup menus, like native Windows menus.
		//
		// The menu window paints a black background that DWM turns transparent
		// (DwmExtendFrameIntoClientArea), with the shadow/border layer and the
		// acrylic backdrop in separate windows. Changing the opacity of those
		// windows directly would break that composition, so instead:
		//
		//   1. Just before the menu becomes visible, snapshot the screen area it will cover.
		//   2. Show that snapshot in a click-through layered window owned by the menu
		//      window (owned windows always stay above their owner).
		//   3. Fade the snapshot out. Cross-fading "what was behind" to "the menu"
		//      looks the same as fading the whole menu in.
		//
		// Everything runs on the menu's UI thread through a window timer. If any step
		// fails the overlay is simply not shown, so the menu appears as before.
		class MenuFade
		{
			static constexpr auto ClassName = L"Morpthis.Shell.MenuFade";
			static constexpr UINT_PTR IDT_FADE = 0x4D46;
			static constexpr DWORD Duration = 150; // ms

			inline static ATOM _atom = 0;

			static LRESULT CALLBACK Proc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
			{
				switch(uMsg)
				{
					case WM_TIMER:
					{
						if(wParam != IDT_FADE)
							break;

						auto start = static_cast<DWORD>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
						auto elapsed = ::GetTickCount() - start;
						if(elapsed >= Duration)
						{
							::KillTimer(hWnd, IDT_FADE);
							::DestroyWindow(hWnd);
							return 0;
						}

						// menu opacity follows an ease-out curve; the overlay is its inverse
						double t = double(elapsed) / Duration;
						double shown = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
						set_alpha(hWnd, static_cast<BYTE>(255.0 * (1.0 - shown)));
						return 0;
					}
					case WM_NCHITTEST:
						return HTTRANSPARENT;
					case WM_MOUSEACTIVATE:
						return MA_NOACTIVATE;
				}
				return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
			}

			static void set_alpha(HWND hWnd, BYTE alpha)
			{
				// content is unchanged, so only the blend needs to be passed
				BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, 0 };
				::UpdateLayeredWindow(hWnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
			}

			static bool register_class()
			{
				if(!_atom)
				{
					WNDCLASSW wc{};
					wc.hInstance = Path::GetCurrentModule();
					wc.lpszClassName = ClassName;
					wc.lpfnWndProc = Proc;
					wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
					_atom = ::RegisterClassW(&wc);
				}
				return _atom != 0;
			}

		public:
			// Follows the system "Fade or slide menus into view" / animation settings.
			static bool enabled()
			{
				BOOL menu_animation = FALSE;
				if(!::SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &menu_animation, 0) || !menu_animation)
					return false;

				BOOL client_animation = TRUE;
				if(::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &client_animation, 0) && !client_animation)
					return false;

				HIGHCONTRASTW hc{ sizeof(hc) };
				if(::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON))
					return false;

				// reading back the screen is slow over remote desktop
				if(::GetSystemMetrics(SM_REMOTESESSION))
					return false;

				return true;
			}

			// Covers `rc` (screen coordinates) with a snapshot of what is on screen now,
			// then fades it out. Returns the overlay window, or nullptr if it wasn't shown.
			static HWND start(HWND owner, RECT rc)
			{
				const int width = rc.right - rc.left;
				const int height = rc.bottom - rc.top;
				if(width <= 0 || height <= 0 || !register_class())
					return nullptr;

				HWND hWnd = nullptr;
				HDC hdc_screen = ::GetDC(nullptr);
				if(!hdc_screen)
					return nullptr;

				HDC hdc_mem = ::CreateCompatibleDC(hdc_screen);
				BITMAPINFO bi{};
				bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
				bi.bmiHeader.biWidth = width;
				bi.bmiHeader.biHeight = -height; // top-down
				bi.bmiHeader.biPlanes = 1;
				bi.bmiHeader.biBitCount = 32;
				bi.bmiHeader.biCompression = BI_RGB;

				void *bits = nullptr;
				HBITMAP hbitmap = hdc_mem ? ::CreateDIBSection(hdc_screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;

				if(hbitmap)
				{
					auto old = ::SelectObject(hdc_mem, hbitmap);

					if(::BitBlt(hdc_mem, 0, 0, width, height, hdc_screen, rc.left, rc.top, SRCCOPY))
					{
						hWnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
												 ClassName, nullptr, WS_POPUP,
												 rc.left, rc.top, width, height,
												 owner, nullptr, Path::GetCurrentModule(), nullptr);
						if(hWnd)
						{
							POINT pt_dst{ rc.left, rc.top };
							POINT pt_src{ 0, 0 };
							SIZE size{ width, height };
							// constant alpha only: the screen copy has no meaningful per-pixel alpha
							BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, 0 };

							if(::UpdateLayeredWindow(hWnd, hdc_screen, &pt_dst, &size, hdc_mem, &pt_src, 0, &blend, ULW_ALPHA) &&
							   ::SetTimer(hWnd, IDT_FADE, USER_TIMER_MINIMUM, nullptr))
							{
								::SetWindowLongPtrW(hWnd, GWLP_USERDATA, static_cast<LONG_PTR>(::GetTickCount()));
								::ShowWindow(hWnd, SW_SHOWNOACTIVATE);
							}
							else
							{
								::DestroyWindow(hWnd);
								hWnd = nullptr;
							}
						}
					}

					::SelectObject(hdc_mem, old);
					::DeleteObject(hbitmap);
				}

				if(hdc_mem)
					::DeleteDC(hdc_mem);
				::ReleaseDC(nullptr, hdc_screen);
				return hWnd;
			}

			// Removes an overlay early (e.g. the menu closed before the fade finished).
			static void stop(HWND &hWnd)
			{
				if(hWnd && ::IsWindow(hWnd))
					::DestroyWindow(hWnd);
				hWnd = nullptr;
			}

			static void unregister()
			{
				if(_atom)
				{
					::UnregisterClassW(ClassName, Path::GetCurrentModule());
					_atom = 0;
				}
			}
		};
	}
}
