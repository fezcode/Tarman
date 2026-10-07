/* Custom title bar without losing what Windows gives a real caption.
 *
 * The window keeps its normal WS_OVERLAPPEDWINDOW style; we subclass the
 * GLFW window procedure and
 *   - WM_NCCALCSIZE: let Windows compute the standard frame, then give the
 *     caption strip back to the client area. Left/right/bottom keep their
 *     (invisible on Windows 11) resize borders. When maximized, Windows
 *     places the window frame-thickness past the monitor edge; we inset the
 *     top by that much so nothing is cut off -- the classic maximized-margin
 *     bug of borderless windows.
 *   - WM_NCHITTEST: the strip the app draws as a title bar answers HTCAPTION
 *     (drag, double-click maximize, Aero Snap, shake), the maximize button
 *     answers HTMAXBUTTON (Windows 11 snap layouts flyout), a thin top band
 *     answers HTTOP for resizing, and the app's own buttons stay HTCLIENT.
 *   - The maximize button is non-client, so its hover/press are tracked
 *     here and handled without DefWindowProc (which would paint the classic
 *     caption button over ours). */

#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>

#include <stdbool.h>

#include "sys.h"

static WNDPROC g_old;
static HWND    g_hwnd;
static int     g_cap_h;
static RECT    g_max;
static RECT    g_client[8];
static int     g_nclient;
static bool    g_max_hover, g_max_down, g_tracking;

static int frame_y(HWND h)
{
    UINT dpi = GetDpiForWindow(h);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

static LRESULT CALLBACK titlebar_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_NCCALCSIZE:
        if (wp) {
            NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
            LONG top = p->rgrc[0].top;
            LRESULT r = CallWindowProcW(g_old, h, m, wp, lp);
            p->rgrc[0].top = top;
            if (IsZoomed(h)) p->rgrc[0].top += frame_y(h);
            return r;
        }
        break;
    case WM_NCHITTEST: {
        LRESULT r = CallWindowProcW(g_old, h, m, wp, lp);
        if (r != HTCLIENT) return r;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        if (!IsZoomed(h) && pt.y < 5) return HTTOP;
        if (pt.y < g_cap_h) {
            if (PtInRect(&g_max, pt)) return HTMAXBUTTON;
            for (int i = 0; i < g_nclient; ++i) if (PtInRect(&g_client[i], pt)) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_NCMOUSEMOVE:
        g_max_hover = wp == HTMAXBUTTON;
        if (!g_tracking) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE | TME_NONCLIENT, h, 0 };
            g_tracking = TrackMouseEvent(&t) != 0;
        }
        if (wp == HTMAXBUTTON) return 0;
        break;
    case WM_NCMOUSELEAVE:
        g_tracking = false;
        g_max_hover = g_max_down = false;
        break;
    case WM_MOUSEMOVE:
        g_max_hover = false;
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTMAXBUTTON) { g_max_down = true; return 0; }
        break;
    case WM_NCLBUTTONUP:
        if (wp == HTMAXBUTTON) {
            if (g_max_down) ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
            g_max_down = false;
            return 0;
        }
        break;
    case WM_NCRBUTTONUP:
    case WM_NCRBUTTONDOWN:
        if (wp == HTMAXBUTTON) return 0;
        break;
    }
    return CallWindowProcW(g_old, h, m, wp, lp);
}

void sys_titlebar_install(void* hwnd)
{
    HWND h = (HWND)hwnd;
    if (!h || g_old) return;
    g_hwnd = h;
    g_old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)titlebar_proc);
    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void sys_titlebar_update(int caption_h, const int max_btn[4], const int (*client)[4], int nclient)
{
    g_cap_h = caption_h;
    g_max = (RECT){ max_btn[0], max_btn[1], max_btn[0] + max_btn[2], max_btn[1] + max_btn[3] };
    g_nclient = nclient > 8 ? 8 : nclient;
    for (int i = 0; i < g_nclient; ++i)
        g_client[i] = (RECT){ client[i][0], client[i][1], client[i][0] + client[i][2], client[i][1] + client[i][3] };
}

bool sys_titlebar_max_hover(void) { return g_max_hover; }
bool sys_titlebar_max_down(void)  { return g_max_down && g_max_hover; }
bool sys_window_active(void)      { return g_hwnd && GetForegroundWindow() == g_hwnd; }
