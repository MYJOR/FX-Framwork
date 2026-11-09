#ifndef GAMEPAD_H
#define GAMEPAD_H

/* Xbox controller via XInput (loaded at runtime, so no extra link dependency).
 * Sticks are -1..1 with a radial deadzone applied, y up. Triggers are 0..1. */
typedef struct {
    int connected;
    float lx, ly, rx, ry, lt, rt;
    unsigned buttons;      /* GP_* bits */
    unsigned pressed;      /* bits that went down this poll */
} Gamepad;

enum {
    GP_DUP = 0x0001, GP_DDOWN = 0x0002, GP_DLEFT = 0x0004, GP_DRIGHT = 0x0008,
    GP_START = 0x0010, GP_BACK = 0x0020, GP_LSTICK = 0x0040, GP_RSTICK = 0x0080,
    GP_LB = 0x0100, GP_RB = 0x0200, GP_A = 0x1000, GP_B = 0x2000, GP_X = 0x4000, GP_Y = 0x8000
};

void gamepad_poll(Gamepad *g);

#endif
