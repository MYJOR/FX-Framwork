#include "platform.h"
#include "gl.h"
#include <windowsx.h>
#include <stdio.h>
#include <string.h>

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

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Platform *p = g_p;
    switch (msg) {
    case WM_CLOSE: if (p) p->quit = 1; return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE:
        if (p) { p->width = LOWORD(lp); p->height = HIWORD(lp); }
        return 0;
    case WM_KEYDOWN: if (p) p->keys[wp & 255] = 1; return 0;
    case WM_KEYUP: if (p) p->keys[wp & 255] = 0; return 0;
    case WM_KILLFOCUS: if (p) memset(p->keys, 0, sizeof p->keys); return 0;
    case WM_RBUTTONDOWN:
        if (p) p->rmb = 1;
        g_lastx = GET_X_LPARAM(lp); g_lasty = GET_Y_LPARAM(lp);
        SetCapture(h);
        return 0;
    case WM_RBUTTONUP:
        if (p) p->rmb = 0;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (p && p->rmb) { p->mouse_dx += x - g_lastx; p->mouse_dy += y - g_lasty; }
        g_lastx = x; g_lasty = y;
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

    PFN_wglSwapIntervalEXT swap = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap) swap(1);
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
