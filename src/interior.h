#ifndef INTERIOR_H
#define INTERIOR_H
#include "gl.h"
#include "mathx.h"
#include "shader.h"

/* Interior renderer: a lighting model for enclosed, artificially lit rooms.
 *
 *   direct    rectangular area lights (analytic form factor) with contact-hardening soft shadows
 *   indirect  a grid of irradiance probes (L1 spherical harmonics) baked on the GPU at load time,
 *             several bounces, so walls and ceiling pick up colour from the floor
 *   specular  one reflection probe, box-projected onto the room, plus box proxies for furniture
 *   contact   screen-space ambient obscurance, blurred until noise-free
 *   image     HDR, MSAA resolved after tonemapping, neutral tonemapper, dithered output
 *
 * Shadows, probes and the reflection probe are baked: once in interior_bake(), and again
 * incrementally by interior_bake_step() while the scene is edited. A frame is a depth/normal
 * prepass, AO, one forward pass and the resolve. */

#define INT_MAX_LIGHTS 8
#define INT_MAX_MATERIALS 24
#define INT_MAX_DRAWS 32
#define INT_MAX_PROXIES 8
#define INT_MAX_ENVS 3
#define INT_MAX_PORTALS 2

enum {
    MAT_TWO_SIDED = 1,    /* no backface culling, normal flipped on back faces */
    MAT_CASTS_SHADOW = 2, /* rendered into the shadow maps (furniture; the room shell only receives) */
    MAT_SHELL = 4,        /* part of the room box: the only geometry the reflection probe captures */
    MAT_DECAL = 8         /* geometry laid just over a surface, drawn in the final image only. Its albedo texture
                             stains what is behind it (multiplied; white = untouched), its normal map dents it. */
};

typedef struct {
    GLuint albedo, normal, orm; /* textures, 0 = none. orm: R = roughness, G = metallic */
    float tint[3];
    float desaturate;           /* 0..1, applied to the albedo texture before the tint */
    float roughness, metallic;  /* multiply the orm texture when there is one */
    float specular;             /* reflectance at normal incidence for non-metals: 0.04 plain, higher = lacquered */
    float rough_variation;      /* roughness follows the albedo brightness by this much (wood grain) */
    float normal_strength;
    float emissive[3];          /* radiance; non-zero makes the surface a pure emitter (light diffusers) */
    unsigned flags;
} IntMaterial;

typedef struct { int first, count, material; } IntDraw;

/* One-sided rectangular emitter. */
typedef struct {
    V3 pos;            /* centre */
    V3 half_u, half_v; /* half extents */
    V3 normal;         /* emitting side */
    V3 radiance;
} IntLight;

/* Solid box that stands in for furniture in reflections. */
typedef struct { V3 min, max; } IntProxy;

/* A reflection probe: captured at pos, box-projected onto the room it stands in. */
typedef struct { V3 pos, box_min, box_max; } IntEnv;

/* The level may be several rooms in a row along the X axis, each parted from the next by a wall
   across X with one arched doorway in it. Light crosses between rooms only through those openings. */
typedef struct {
    float x0, x1;   /* the wall's two faces */
    float z;        /* centre of the doorway */
    float half_w;   /* half its width, and the radius of the arch */
    float spring;   /* height where the straight sides end and the arch begins */
} IntPortal;

typedef struct {
    GLuint vao, vbo;
    int vertex_count;
    IntMaterial mats[INT_MAX_MATERIALS]; int mat_count;
    IntDraw draws[INT_MAX_DRAWS];        int draw_count;
    IntLight lights[INT_MAX_LIGHTS];     int light_count;
    IntProxy proxies[INT_MAX_PROXIES];   int proxy_count;
    V3 proxy_albedo;
    V3 room_min, room_max; /* bounds of the whole level: the irradiance probes fill this box */
    IntEnv envs[INT_MAX_ENVS]; int env_count; /* one per room, in order along x: portal_count + 1 of them */
    IntPortal portals[INT_MAX_PORTALS]; int portal_count; /* portals[i] parts envs[i] from envs[i + 1]; 0: one room */
} IntScene;

/* The look. Everything a lighting artist would tune lives here. */
typedef struct {
    float fov;             /* vertical, radians */
    float exposure;
    float shadow_softness; /* radius in metres of the light as the penumbra sees it; smaller = crisper */
    float bounce_saturation; /* colour kept in bounced light: 1 = physical; lower stops a strongly
                                coloured floor from dyeing the walls and ceiling. Needs a rebake. */
    float ao_radius;       /* metres */
    float ao_strength;     /* 0 = off */
    float ao_direct;       /* how much of the AO also darkens direct light (0 = physically strict) */
    float reflection;      /* scale on all specular */
    float saturation, contrast, vignette;
    float dof_strength;    /* near-field depth of field: 0 = off, 1 = the default blur size */
    float dof_start;       /* metres: anything nearer than this starts to go soft */
    float dof_edge;        /* blur towards the borders of the screen, like a soft lens: 0 = none, 1 = full blur in the corners */
    float dof_full;        /* metres: fully blurred at this distance and closer */
    float dof_defocus;     /* whole picture out of focus, 0..1: for a lens that is still finding focus */
    float dof_hand;        /* 0..1: the lens is focused on what the player holds (IntRenderer.held_draw) instead of
                              the room. At 1 the held thing is sharp and the room behind it goes soft. */
    float dof_hand_blur;   /* how soft the held thing is while the lens is on the room, 0..1 of the full blur */
    float dof_room_blur;   /* how soft the room gets while the lens is on the held thing, 0..1 of the full blur */
    float dof_hand_dist;   /* metres: where the lens focuses for the held thing */
    float dof_room_full;   /* metres: the room is at dof_room_blur from here on; nearer things are less soft */
    float bloom;           /* glow around what is brighter than the display can show. 0 = off */
    float air;             /* how much the torch beam shows in the air. 0 = off */
    float grain, grain_size; /* film grain over the whole picture (0 = none), in pixels */
    float grade_cast, black_crush; /* green-grey cast 0..1; share of the darkest tones crushed to black */
    float chroma;          /* colour fringing towards the screen edges, 0 = none */
    float lens_dirt;       /* smears on the lens that light up near bright things, 0 = none */
    float rough_min, rough_scale; /* smallest roughness allowed; scale on every material's roughness (lower = wetter) */
    float adapt_max;       /* eye adaptation: how many times the exposure may rise in the dark. 1 = off */
    float lens_curve;      /* barrel distortion: straight lines bow outwards towards the screen sides. 0 = none */
    int msaa;              /* 2, 4 or 8 */
} IntLook;

enum {
    INT_VIEW_FINAL, INT_VIEW_ALBEDO, INT_VIEW_NORMAL, INT_VIEW_LIGHTING, INT_VIEW_DIRECT,
    INT_VIEW_INDIRECT, INT_VIEW_AO, INT_VIEW_SHADOW, INT_VIEW_ROUGHNESS, INT_VIEW_SPECULAR, INT_VIEW_COUNT
};
const char *interior_view_name(int view);

/* Something the player holds up in front of the camera (the motion tracker). It is drawn by its owner,
   after the room and into the same HDR target, so it gets the same exposure, bloom, grade and lens. The
   owner must write alpha 0: that is how the depth of field tells it from the room (see IntLook.dof_hand).
   On entry: depth test and depth writes on, blending off, culling off. Leave it that way. */
typedef struct {
    M4 view;            /* world -> eye, with the camera's roll */
    V3 cam;
    float aspect;
    float near_z, far_z; /* the scene's depth range: use the same one */
    float sh[3][4];     /* light arriving at the hand, per colour channel: irradiance / pi = L0 + dot(L1, n) */
    float scene_light;  /* the lights of the room the camera is in, 0..1: scales sh, the lights and env_tex */
    GLuint env_tex;     /* reflection probe (cube map) of the room the camera is in */
    float env_max_lod;  /* its mip for roughness 1 */
    GLuint exposure_tex; /* one texel, r = the gain the eye adaptation adds */
} IntHeldView;
typedef void (*IntHeldFn)(void *user, const IntScene *s, const IntHeldView *v);

/* Things that come and go on the room's surfaces while the game runs (the handprints): drawn by their
   owner over the lit room, before whatever the player holds. On entry: depth test on, depth writes
   off, a polygon offset towards the camera, blending ONE, ONE_MINUS_SRC_ALPHA: write premultiplied
   colour, and alpha = how much of the surface underneath is hidden. */
typedef struct {
    M4 view_proj;
    V3 cam;
    float scene_light;  /* the lights of the room the camera is in, 0..1 */
    GLuint env_tex;     /* reflection probe (cube map) of the room the camera is in */
    float env_max_lod;
    float flash;        /* the torch: brightness (0 = off), where it is, where it points, colour, beam narrowing */
    V3 flash_pos, flash_dir, flash_color;
    float flash_cone;
} IntOverlayView;
typedef void (*IntOverlayFn)(void *user, const IntScene *s, const IntOverlayView *v);

typedef struct {
    Program fwd, depth, gbuf, ao, blur, post, prefilter, dof, dof_mix, meter, adapt, bounce, glow, glow_blur;
    GLuint fwd_cached; GLint loc[40];
    GLuint fs_vao, white_tex, flat_normal_tex;

    /* baked once */
    GLuint shadow_fbo, shadow_tex, blocker_tex;
    int shadow_lights;                 /* lights the shadow atlas is sized for */
    GLuint blocker_depth;              /* the shadow views again, at blocker map resolution */
    float *shadow_depth, *blocker_px;  /* scratch for building the blocker map */
    M4 shadow_mat[INT_MAX_LIGHTS];
    float shadow_rect[INT_MAX_LIGHTS][4];
    GLuint sh_tex[3];
    int probe_n[3];
    V3 probe_origin, probe_extent;
    GLuint capture_fbo, capture_tex, capture_depth;
    GLuint env_fbo, env_raw, env_tex[INT_MAX_ENVS], env_depth; /* raw capture; env_tex = convolved per roughness */
    float bake_softness;
    double bake_ms;

    /* live rebake */
    float *sh_cpu[3];        /* the probes' SH, as uploaded to sh_tex */
    float (*probe_dir)[4];   /* per capture texel: direction and solid angle */
    float *probe_pixels;
    unsigned char *probe_alive; /* 0: the probe is inside a wall or outside the rooms, and is never captured or read */
    int *probe_ids;
    int probe_stride, probe_cursor;
    int probes_pending;      /* probe captures left before the indirect light has settled */
    unsigned dirty;          /* INT_DIRTY_* */
    double step_ms;          /* cost of the last interior_bake_step */

    float cam_roll;          /* radians about the view axis, positive leans the head right */
    /* A hand torch: a spot light with its own shadow map, redrawn every frame. Its bounce is one point light where the beam lands. */
    float flash;             /* brightness, 0 = off */
    V3 flash_pos, flash_dir; /* dir normalised */
    V3 flash_color;
    float flash_cone;        /* beam narrowing: 1 = as wide as authored, 1.5 = two thirds of it */
    float flash_falloff;     /* extra fade with distance, on top of the inverse square */
    float flash_harsh;       /* 0..1: squeeze the beam's energy into its core */
    float flash_spec;        /* highlight strength */
    float flash_bounce;      /* how much light comes back off what the beam lands on */
    GLuint flash_fbo, flash_shadow, flash_cookie; /* shadow map; the beam's pattern */
    GLuint bcap_fbo, bcap_tex, bcap_depth, bounce_fbo[2], bounce_tex[2]; int bounce_cur; /* what the beam lands on */
    float frame_dt;          /* seconds since the last frame, for the eye adaptation; 0 = adapt at once */
    float grain_seed;        /* changes every frame so the low light noise moves */
    GLuint meter_fbo, meter_tex, expo_fbo[2], expo_tex[2]; int expo_cur; /* light meter, adapted exposure gain */
    float scene_light;       /* scales everything the scene's own lights contribute: 1 = on, 0 = dark. No rebake needed. */
    float room_light[INT_MAX_ENVS]; /* the same per room (in the order of IntScene.envs), on top of scene_light: a room's
                                own switch. A dark room still gets what comes in through its doorways. */
    IntHeldFn held_draw;     /* draws what the player holds, or NULL. Final view only. */
    void *held_user;
    IntOverlayFn overlay_draw; /* draws on the room's surfaces, or NULL. Final view only. */
    void *overlay_user;
    GLuint out_fbo;          /* where the finished frame goes: 0 = the window */
    int out_x, out_y;        /* its lower left corner there, in pixels (an editor draws the view inside its panels) */

    /* sized to the window */
    int w, h, samples;
    GLuint scene_fbo, scene_color, scene_depth;
    GLuint pre_fbo, pre_normal, pre_depth;
    GLuint ao_fbo[2], ao_tex[2];
    GLuint ldr_fbo, ldr_tex;   /* graded frame, near-field blur amount in alpha */
    GLuint glow_fbo[2], glow_tex[2]; /* quarter resolution: bloom and the beam in the air */
    GLuint dof_fbo, dof_tex;   /* half resolution: the blurred foreground and how much of it covers each pixel */
} IntRenderer;

IntLook interior_default_look(void);
int interior_init(IntRenderer *r);
/* Renders shadow maps, irradiance probes and the reflection probe from scratch (about a second). */
void interior_bake(IntRenderer *r, const IntScene *s, const IntLook *look);

/* Live rebake, for scenes that are being edited. Call interior_invalidate when lights or geometry
   moved, then interior_bake_step once per frame: shadows are redone at once, the bounce light and
   the reflections catch up over the following frames within the time budget. */
enum {
    INT_DIRTY_SHADOWS = 1,  /* shadow casters or lights moved, or the softness changed */
    INT_DIRTY_LIGHTING = 2, /* anything that changes the light in the room */
    INT_DIRTY_ENV = 4       /* internal: reflection probe is stale */
};
void interior_invalidate(IntRenderer *r, unsigned what);
int interior_bake_step(IntRenderer *r, const IntScene *s, const IntLook *look, double budget_ms); /* 1 = more to do */
float interior_bake_progress(const IntRenderer *r); /* 0..1, 1 = settled */
/* Hot reload; returns 1 if a shader that affects the bake changed (the caller should rebake). */
int interior_reload_shaders(IntRenderer *r);
/* Debug: the exposure gain the eye adaptation has settled on, and the metered scene luminance. */
float interior_exposure_gain(IntRenderer *r, float *metered);
/* Sets the uniforms portalVis() in interior_common.glsl reads, on the program that is bound. */
void interior_set_portals(GLuint program, const IntScene *s);
void interior_render(IntRenderer *r, const IntScene *s, const IntLook *look, V3 cam, V3 fwd, int w, int h, int view);

#endif
