#include "platform.h"
#include "gl.h"
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef HGLRC(WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef BOOL(WINAPI *PFN_wglSwapIntervalEXT)(int);

static HWND g_hwnd;
static HDC g_hdc;
static Platform *g_p;
static int g_lastx, g_lasty;
static int g_buttons, g_tracking;
static PlatEventFn g_hook;
static void *g_hook_user;
static PFN_wglSwapIntervalEXT g_swap_interval;

static void emit(const PlatEvent *e) { if (g_hook) g_hook(g_hook_user, e); }
void platform_set_event_hook(PlatEventFn fn, void *user) { g_hook = fn; g_hook_user = user; }
static double g_refresh_per_frame = 2.0;
static int g_vsync_interval = 1; /* vblanks per frame: refresh rate / target fps, so 60 Hz -> 2 for an even 30 fps */
static int g_vsync_on = 1;
typedef HRESULT(WINAPI *PFN_DwmFlush)(void);
static PFN_DwmFlush g_dwm_flush;
/* Frame pacing uses the desktop compositor's refresh beat (DwmFlush) when it is there: SwapBuffers does not
   reliably wait for vblank in a window, so the swap interval is left at 0 and we wait for N refreshes instead. */
void platform_set_vsync(int on) { g_vsync_on = on; if (g_swap_interval) g_swap_interval((on && !g_dwm_flush) ? g_vsync_interval : 0); }
void platform_set_target_fps(int fps) {
    HDC dc = GetDC(NULL);
    int hz = dc ? GetDeviceCaps(dc, VREFRESH) : 60;
    if (dc) ReleaseDC(NULL, dc);
    if (hz <= 1) hz = 60;
    int n = (hz + fps / 2) / fps;
    g_vsync_interval = n < 1 ? 1 : n;
    g_refresh_per_frame = (double)hz / (double)fps; /* 60 Hz -> 2.0, 165 Hz -> 5.5 */
}
void *platform_gl_proc(const char *name) {
    void *f = (void *)wglGetProcAddress(name);
    if (f == (void *)1 || f == (void *)2 || f == (void *)3 || f == (void *)-1) f = NULL;
    if (!f) {
        static HMODULE gl;
        if (!gl) gl = LoadLibraryA("opengl32.dll");
        f = (void *)GetProcAddress(gl, name);
    }
    return f;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Platform *p = g_p;
    switch (msg) {
    case WM_CLOSE: if (p) p->quit = 1; return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE:
        if (p) { p->width = LOWORD(lp); p->height = HIWORD(lp); }
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP: case WM_SYSKEYUP: {
        int down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        if (p) p->keys[wp & 255] = (unsigned char)down;
        PlatEvent e = {PE_KEY};
        e.a = (int)wp; e.down = down;
        e.mods = ((GetKeyState(VK_SHIFT) & 0x8000) ? PMOD_SHIFT : 0) |
                 ((GetKeyState(VK_CONTROL) & 0x8000) ? PMOD_CTRL : 0) |
                 ((GetKeyState(VK_MENU) & 0x8000) ? PMOD_ALT : 0);
        emit(&e);
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) break; /* keep Alt+F4 */
        return 0;
    }
    case WM_CHAR: {
        static unsigned hi;
        unsigned c = (unsigned)wp;
        if (c >= 0xD800 && c < 0xDC00) { hi = c; return 0; }
        if (c >= 0xDC00 && c < 0xE000) c = 0x10000 + ((hi - 0xD800) << 10) + (c - 0xDC00);
        if (c < 32 || c == 127) return 0;
        PlatEvent e = {PE_CHAR};
        if (c < 0x80) e.utf8[0] = (char)c;
        else if (c < 0x800) { e.utf8[0] = (char)(0xC0 | (c >> 6)); e.utf8[1] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { e.utf8[0] = (char)(0xE0 | (c >> 12)); e.utf8[1] = (char)(0x80 | ((c >> 6) & 63)); e.utf8[2] = (char)(0x80 | (c & 63)); }
        else { e.utf8[0] = (char)(0xF0 | (c >> 18)); e.utf8[1] = (char)(0x80 | ((c >> 12) & 63)); e.utf8[2] = (char)(0x80 | ((c >> 6) & 63)); e.utf8[3] = (char)(0x80 | (c & 63)); }
        emit(&e);
        return 0;
    }
    case WM_KILLFOCUS: {
        if (p) memset(p->keys, 0, sizeof p->keys);
        PlatEvent e = {PE_FOCUS}; e.a = 0; emit(&e);
        return 0;
    }
    case WM_SETFOCUS: { PlatEvent e = {PE_FOCUS}; e.a = 1; emit(&e); return 0; }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        int down = (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN);
        int btn = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 :
                  (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1 : 2;
        PlatEvent e = {PE_MOUSE_BUTTON};
        e.a = btn; e.down = down;
        e.x = (float)GET_X_LPARAM(lp); e.y = (float)GET_Y_LPARAM(lp);
        emit(&e);
        if (down) { g_buttons |= 1 << btn; SetCapture(h); }
        else { g_buttons &= ~(1 << btn); if (!g_buttons) ReleaseCapture(); }
        if (btn == 1) {
            if (p) p->rmb = down;
            g_lastx = GET_X_LPARAM(lp); g_lasty = GET_Y_LPARAM(lp);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        PlatEvent e = {PE_WHEEL};
        float d = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (msg == WM_MOUSEWHEEL) e.y = d; else e.x = d;
        emit(&e);
        return 0;
    }
    case WM_MOUSELEAVE: {
        g_tracking = 0;
        PlatEvent e = {PE_MOUSE_LEAVE}; emit(&e);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (!g_tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            g_tracking = 1;
        }
        if (p && p->rmb) { p->mouse_dx += x - g_lastx; p->mouse_dy += y - g_lasty; }
        g_lastx = x; g_lasty = y;
        PlatEvent e = {PE_MOUSE_MOVE};
        e.x = (float)x; e.y = (float)y;
        emit(&e);
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int platform_init(Platform *p, const char *title, int w, int h) {
    g_p = p;
    memset(p, 0, sizeof *p);
    p->width = w; p->height = h;

    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "DesertWnd";
    RegisterClassA(&wc);

    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) return 0;
    g_hdc = GetDC(g_hwnd);

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
    int fmt = ChoosePixelFormat(g_hdc, &pfd);
    if (!fmt || !SetPixelFormat(g_hdc, fmt, &pfd)) return 0;

    HGLRC legacy = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, legacy);
    PFN_wglCreateContextAttribsARB create =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create) { fprintf(stderr, "OpenGL 3.3 context creation not supported\n"); return 0; }
    int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                     WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    HGLRC ctx = create(g_hdc, NULL, attribs);
    if (!ctx) { fprintf(stderr, "failed to create GL 3.3 core context\n"); return 0; }
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    wglMakeCurrent(g_hdc, ctx);

    g_swap_interval = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
    platform_set_vsync(1);
    return gl_load_functions();
}

void platform_poll(Platform *p) {
    p->mouse_dx = p->mouse_dy = 0;
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) p->quit = 1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void platform_swap(Platform *p) { (void)p; SwapBuffers(g_hdc); }

double platform_time(void) {
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER now;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
}

void platform_set_title(const char *t) { if (g_hwnd) SetWindowTextA(g_hwnd, t); }

/* Sleeps until the next frame boundary (fps), using a high-resolution timer for the bulk and a short spin for
   the last stretch, so frames come out evenly spaced instead of whenever Sleep() wakes up. */
void platform_frame_limit(double fps) {
    static double next;
    static HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    double period = 1.0 / fps, now = platform_time();
    if (next == 0.0) next = now;
    next += period;
    if (next < now - period) next = now;            /* fell far behind: don't try to catch up in a burst */
    double left = next - now;
    if (left > 0.002 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((left - 0.0015) * 1e7);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100);
    } else if (left > 0.002) {
        Sleep((DWORD)((left - 0.0015) * 1000.0));
    }
    while (platform_time() < next) { /* spin the last ~1.5 ms */ }
}

/* One frame of pacing, called right after the buffer swap. With vsync on and a compositor available this waits
   exactly `interval` display refreshes (60 Hz, 30 fps -> 2), which gives perfectly even frame times. */
void platform_pace(double fps) {
    if (g_vsync_on && g_dwm_flush) {
        /* Hold each frame until its slot (1/fps apart) by riding the compositor's refresh beat: flush until
           the slot is less than half a refresh away. Presentation lands on a vblank, and the long-run average
           is exact even when fps does not divide the refresh rate (165 Hz / 30 fps = 5.5 refreshes). */
        static double next;
        double period = 1.0 / fps, vperiod = g_refresh_per_frame > 0 ? period / g_refresh_per_frame : 1.0 / 60.0;
        double now = platform_time();
        if (next == 0.0) next = now;
        next += period;
        if (next < now - period) next = now;      /* hitched: resync instead of bursting to catch up */
        int guard = 0;
        while (now + 0.5 * vperiod < next && guard++ < 64) { g_dwm_flush(); now = platform_time(); }
        return;
    }
    platform_frame_limit(fps);
}
void platform_pace_init(void) {
    HMODULE m = LoadLibraryA("dwmapi.dll");
    if (m) g_dwm_flush = (PFN_DwmFlush)(void *)GetProcAddress(m, "DwmFlush");
    platform_set_vsync(g_vsync_on);
}
