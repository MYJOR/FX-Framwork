#ifndef KNOCK_H
#define KNOCK_H
#include "mathx.h"

/* Something knocks on the red door at the end of the hallway. Nobody knows what. Every so often it
   raps one to four times, never to a pattern, and that is all it does.

   The sound is made here, not loaded: knuckles on a heavy panelled door (a click of contact, the
   thump of the wood's low modes, a little of the hallway ringing after it), rendered for where the
   listener stands when the first knock lands: quieter with distance, to the side the door is on, and
   dull through a wall. Played through winmm; with no sound device the knocks are silent. */

#define KNOCK_MAX 4
#define KNOCK_RATE 44100

typedef struct {
    unsigned seed;
    float wait;              /* seconds until it knocks again */
    int count;               /* knocks in the sequence being played */
    float at[KNOCK_MAX];     /* when each lands, seconds from the start of the sequence */
    float clock;             /* seconds since the sequence started; < 0: silent */
    int muted;               /* 1: keeps time, plays nothing (screenshots, benchmarks) */
} Knock;

void knock_init(Knock *k, unsigned seed);
/* Advances the time. door: where the knocking comes from. ear, right: the listener and the direction
   to their right (normalised). walls: how many walls stand between the two (0 = same room).
   Returns how many knocks the sequence that started this frame has, usually 0. */
int knock_update(Knock *k, float dt, V3 door, V3 ear, V3 right, int walls);
/* Test: renders one sequence of `count` knocks as heard from 2 m in front of the door and writes it
   as a 16-bit stereo WAV. Returns 0 if the file cannot be written. */
int knock_write_wav(const char *path, int count, unsigned seed);

#endif
