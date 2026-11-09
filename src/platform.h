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

int platform_init(Platform *p, const char *title, int w, int h);
void platform_poll(Platform *p);
void platform_swap(Platform *p);
double platform_time(void);

#endif
