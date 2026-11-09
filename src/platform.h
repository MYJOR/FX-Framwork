#ifndef PLATFORM_H
#define PLATFORM_H

/* Thin platform layer: window + GL 3.3 core context + input. */
typedef struct {
    int width, height;
    unsigned char keys[256]; /* indexed by Win32 virtual-key code */
    int mouse_dx, mouse_dy;  /* accumulated while right mouse is held */
    int rmb;
    int quit;
} Platform;

/* Raw input events, forwarded as they arrive so a UI layer can consume them. */
typedef enum { PE_MOUSE_MOVE, PE_MOUSE_BUTTON, PE_WHEEL, PE_KEY, PE_CHAR, PE_MOUSE_LEAVE, PE_FOCUS } PlatEventType;
enum { PMOD_SHIFT = 1, PMOD_CTRL = 2, PMOD_ALT = 4 };
typedef struct {
    PlatEventType type;
    float x, y;        /* MOUSE_MOVE: position; WHEEL: x = horizontal, y = vertical lines (+ = up) */
    int a;             /* MOUSE_BUTTON: 0 left, 1 right, 2 middle; KEY: virtual-key; FOCUS: 1 = focused */
    int down;          /* MOUSE_BUTTON, KEY */
    int mods;          /* KEY: PMOD_* */
    char utf8[8];      /* CHAR */
} PlatEvent;
typedef void (*PlatEventFn)(void *user, const PlatEvent *e);

int platform_init(Platform *p, const char *title, int w, int h);
void platform_poll(Platform *p);
void platform_swap(Platform *p);
double platform_time(void);
void platform_set_event_hook(PlatEventFn fn, void *user);
void platform_set_vsync(int on);
/* Resolves any GL function, including the 1.1 ones that wglGetProcAddress refuses. */
void *platform_gl_proc(const char *name);

#endif
