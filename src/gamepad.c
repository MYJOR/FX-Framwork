#include "gamepad.h"
#include <windows.h>
#include <math.h>
#include <string.h>

/* minimal XInput definitions (same layout as xinput.h) */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XPad;
typedef struct { DWORD dwPacketNumber; XPad Gamepad; } XState;
typedef DWORD(WINAPI *XGetStateFn)(DWORD, XState *);

static XGetStateFn g_get;
static int g_tried;
static DWORD g_next_probe;   /* an absent pad is polled rarely: XInputGetState is slow when nothing is plugged in */
static int g_slot = -1;

static void stick(float x, float y, float dz, float *ox, float *oy) {
    float m = sqrtf(x * x + y * y);
    if (m < dz) { *ox = *oy = 0; return; }
    float s = (m - dz) / (1.0f - dz);
    if (s > 1.0f) s = 1.0f;
    *ox = x / m * s; *oy = y / m * s;
}

void gamepad_poll(Gamepad *g) {
    unsigned prev = g->buttons;
    if (!g_tried) {
        g_tried = 1;
        const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (int i = 0; i < 3 && !g_get; i++) {
            HMODULE m = LoadLibraryA(dlls[i]);
            if (m) g_get = (XGetStateFn)(void *)GetProcAddress(m, "XInputGetState");
        }
    }
    if (!g_get) { memset(g, 0, sizeof *g); return; }

    XState st;
    int ok = 0;
    if (g_slot >= 0) ok = g_get((DWORD)g_slot, &st) == 0;
    if (!ok) {
        g_slot = -1;
        DWORD now = GetTickCount();
        if ((int)(now - g_next_probe) >= 0) {
            g_next_probe = now + 1000;
            for (int i = 0; i < 4 && !ok; i++) if (g_get((DWORD)i, &st) == 0) { ok = 1; g_slot = i; }
        }
    }
    if (!ok) { memset(g, 0, sizeof *g); return; }

    g->connected = 1;
    const XPad *p = &st.Gamepad;
    stick(p->sThumbLX / 32767.0f, p->sThumbLY / 32767.0f, 0.22f, &g->lx, &g->ly);
    stick(p->sThumbRX / 32767.0f, p->sThumbRY / 32767.0f, 0.20f, &g->rx, &g->ry);
    g->lt = p->bLeftTrigger / 255.0f;
    g->rt = p->bRightTrigger / 255.0f;
    g->buttons = p->wButtons;
    g->pressed = g->buttons & ~prev;
}
