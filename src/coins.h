#ifndef COINS_H
#define COINS_H
#include "gl.h"
#include "mathx.h"
#include "particles.h"

#define COIN_MAX 64
#define COIN_RADIUS 0.30f   /* metres */

typedef struct {
    V3 pos;          /* centre of the coin when resting (above the sand) */
    float phase;     /* spin / bob offset */
    float seed;      /* per-coin shader variation */
    float collect_t; /* <0 = still there, otherwise seconds since pickup */
} Coin;

typedef struct {
    Coin c[COIN_MAX];
    int n, collected;
    GLuint vao, vbo, ibo;
    GLsizei index_count;
} Coins;

/* Scatters coins over the terrain around `centre` (deterministic). Needs a current GL context. */
void coins_init(Coins *cs, V3 centre, float radius, int count);
/* Spins/bobs coins, collects any the player touches (sand burst + fly-up). Returns coins picked up this frame. */
int coins_update(Coins *cs, float dt, V3 player_pos, Particles *ps);
/* Draws every coin; the coin program must already be bound with its common uniforms and uViewProj set. */
void coins_draw(const Coins *cs, GLuint program, float time);

#endif
