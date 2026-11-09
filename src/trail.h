#ifndef TRAIL_H
#define TRAIL_H
#include "gl.h"
#include "player.h"

#define TRAIL_N 2048           /* texels per side */
#define TRAIL_SIZE 64.0f       /* metres covered by the wrapping window */

/* Footprints and trails: a toroidal RG8 texture centred on the player.
   R = depression depth, G = raised rim. World XZ maps to uv = xz / TRAIL_SIZE
   with GL_REPEAT, so nothing is ever copied when the window moves: texels
   that leave the window on one side are cleared as they re-enter on the other. */
typedef struct {
    unsigned char *data; /* N*N*2 */
    GLuint tex;
    int cx, cz;          /* window centre in world texel units */
    int started;
    float decay_acc;     /* seconds since last fill-in step */
    float foot_accum;    /* distance walked since the last footprint */
    float groove_accum;
    float drag_accum;
    float path;          /* total distance travelled, drives smooth variation along the track */
    unsigned rng;
    int foot_side;
    V3 last_pos;
    int dirty_full;
    int dirty_row_min, dirty_row_max; /* inclusive; max < min means clean */
} Trail;

void trail_init(Trail *t, V3 start);
void trail_update(Trail *t, const Player *p, float dt, float time);
void trail_bind(const Trail *t); /* binds to texture unit 0 */

#endif
