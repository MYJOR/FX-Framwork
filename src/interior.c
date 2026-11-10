#include "interior.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f

/* Shadow maps: one tile per light in a single atlas, looking straight out of the emitter. */
#define SHADOW_RES 1024
#define BLOCKER_RES 256
#define SHADOW_NEAR 0.3f
#define SHADOW_FAR 3.4f
#define SHADOW_FOV (120.0f * PI / 180.0f)
#define BLOCKER_BLUR 3 /* box radius in blocker texels, applied twice */

/* Irradiance probes: a cell-centred grid filling the room. */
#define PROBE_SPACING 0.6f
#define PROBE_FACE 8      /* cube face resolution; irradiance is smooth, this is plenty for L1 */
#define PROBE_BOUNCES 4
#define PROBE_ROW 20      /* probes per atlas row */
#define LIVE_SWEEPS 4      /* passes over all probes after a change before the live update rests */

#define ENV_RES 128
#define ENV_MAX_LOD 5.0f

#define FLASH_RES 768
#define FLASH_FOV (64.0f * PI / 180.0f) /* the shadow map and the beam pattern cover this cone */
#define COOKIE_RES 256
#define BOUNCE_RES 16
#define BOUNCE_FOV (36.0f * PI / 180.0f)
#define VIEW_BOUNCE_CAPTURE 10 /* internal debug view of the forward shader: albedo, distance in alpha */

#define ADAPT_KEY 0.05f     /* scene luminance below which the eye starts to open up */
#define ADAPT_FLOOR 0.004f  /* the meter reads black as this */

#define CAM_NEAR 0.05f
#define CAM_FAR 60.0f

/* Texture units of the forward shader. */
enum { TU_ALBEDO, TU_NORMAL, TU_ORM, TU_SHADOW, TU_BLOCKER, TU_SH_R, TU_SH_G, TU_SH_B, TU_ENV, TU_AO, TU_FLASH_SHADOW, TU_FLASH_COOKIE, TU_FLASH_BOUNCE };
/* Uniforms set per view or per draw, looked up once. */
enum { U_VIEWPROJ, U_CAM, U_TINT, U_MAT, U_MAT2, U_EMISSIVE, U_CACHED };

enum { PASS_FINAL, PASS_PROBE, PASS_ENV };

static const char *VIEW_NAMES[INT_VIEW_COUNT] = {"final", "albedo", "normals", "lighting", "direct light",
                                                 "indirect light", "ambient obscurance", "shadows",
                                                 "roughness / metallic", "specular"};
const char *interior_view_name(int view) { return view >= 0 && view < INT_VIEW_COUNT ? VIEW_NAMES[view] : "?"; }

IntLook interior_default_look(void) {
    IntLook l;
    l.fov = 62.0f * PI / 180.0f;
    l.exposure = 1.25f;
    l.shadow_softness = 0.26f;
    l.bounce_saturation = 0.42f;
    l.ao_radius = 0.40f;
    l.ao_strength = 0.55f;
    l.ao_direct = 0.15f;
    l.reflection = 1.0f;
    l.saturation = 1.04f;
    l.contrast = 1.06f;
    l.vignette = 0.10f;
    l.dof_strength = 1.0f;
    l.dof_start = 0.80f;
    l.dof_full = 0.22f;
    l.dof_edge = 0.65f;
    l.dof_defocus = 0.0f;
    l.lens_curve = 0.035f;
    l.adapt_max = 5.0f;
    l.grain = 0.0f; l.grain_size = 1.0f;
    l.grade_cast = 0.0f; l.black_crush = 0.0f;
    l.chroma = 0.0f;
    l.lens_dirt = 0.0f;
    l.rough_min = 0.22f; l.rough_scale = 1.0f;
    l.bloom = 0.06f;
    l.air = 0.007f;
    l.msaa = 4;
    return l;
}

/* ---- small GL helpers --------------------------------------------------------------------------- */

static GLint ul(GLuint p, const char *name) { return glGetUniformLocation(p, name); }
static void set_v3(GLuint p, const char *name, V3 v) { glUniform3f(ul(p, name), v.x, v.y, v.z); }

static GLuint make_tex(GLenum target, GLint filter, GLint wrap) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(target, t);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter == GL_LINEAR_MIPMAP_LINEAR ? GL_LINEAR : filter);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap);
    if (target == GL_TEXTURE_3D || target == GL_TEXTURE_CUBE_MAP) glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap);
    return t;
}

static GLuint make_tex2d(GLint internal, int w, int h, GLenum format, GLenum type, const void *px, GLint filter) {
    GLuint t = make_tex(GL_TEXTURE_2D, filter, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, px);
    return t;
}

static void check_fbo(const char *what) {
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[interior] framebuffer incomplete: %s\n", what);
}

/* View matrix looking along fwd with an explicit up (m4_view assumes +Y up, which fails for vertical views). */
static M4 look_matrix(V3 pos, V3 fwd, V3 up) {
    V3 r = v3_norm(v3_cross(fwd, up));
    V3 u = v3_cross(r, fwd);
    M4 m = {{0}};
    m.m[0] = r.x;    m.m[4] = r.y;    m.m[8] = r.z;     m.m[12] = -v3_dot(r, pos);
    m.m[1] = u.x;    m.m[5] = u.y;    m.m[9] = u.z;     m.m[13] = -v3_dot(u, pos);
    m.m[2] = -fwd.x; m.m[6] = -fwd.y; m.m[10] = -fwd.z; m.m[14] = v3_dot(fwd, pos);
    m.m[15] = 1.0f;
    return m;
}

/* Cube map faces in GL order (+X -X +Y -Y +Z -Z): view direction and up. */
static const float CUBE_FWD[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
static const float CUBE_UP[6][3] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};

static M4 cube_view_proj(V3 pos, int face) {
    M4 view = look_matrix(pos, v3(CUBE_FWD[face][0], CUBE_FWD[face][1], CUBE_FWD[face][2]),
                          v3(CUBE_UP[face][0], CUBE_UP[face][1], CUBE_UP[face][2]));
    return m4_mul(m4_perspective(PI * 0.5f, 1.0f, 0.02f, 40.0f), view);
}

/* ---- scene drawing ------------------------------------------------------------------------------ */

/* Draws every range whose material has all `require` flags. With `materials` the forward program is
   bound and gets the material; otherwise only geometry is drawn (shadow maps, prepass). */
static void draw_scene(const IntRenderer *r, const IntScene *s, int materials, unsigned require, int cull) {
    glBindVertexArray(s->vao);
    for (int i = 0; i < s->draw_count; i++) {
        const IntDraw *d = &s->draws[i];
        const IntMaterial *m = &s->mats[d->material];
        if ((m->flags & require) != require) continue;
        if (cull && !(m->flags & MAT_TWO_SIDED)) glEnable(GL_CULL_FACE);
        else glDisable(GL_CULL_FACE);
        if (materials) {
            glActiveTexture(GL_TEXTURE0 + TU_ALBEDO);
            glBindTexture(GL_TEXTURE_2D, m->albedo ? m->albedo : r->white_tex);
            glActiveTexture(GL_TEXTURE0 + TU_NORMAL);
            glBindTexture(GL_TEXTURE_2D, m->normal ? m->normal : r->flat_normal_tex);
            glActiveTexture(GL_TEXTURE0 + TU_ORM);
            glBindTexture(GL_TEXTURE_2D, m->orm ? m->orm : r->white_tex);
            glUniform4f(r->loc[U_TINT], m->tint[0], m->tint[1], m->tint[2], m->desaturate);
            glUniform4f(r->loc[U_MAT], m->roughness, m->metallic, 0.0f, m->normal ? m->normal_strength : 0.0f);
            glUniform3f(r->loc[U_EMISSIVE], m->emissive[0], m->emissive[1], m->emissive[2]);
            glUniform4f(r->loc[U_MAT2], m->rough_variation, m->orm ? 1.0f : 0.0f,
                        (m->flags & MAT_TWO_SIDED) ? 1.0f : 0.0f, m->specular);
        }
        glDrawArrays(GL_TRIANGLES, d->first, d->count);
    }
    glDisable(GL_CULL_FACE);
}

/* Binds the forward program with everything that does not change between views. */
static void fwd_begin(IntRenderer *r, const IntScene *s, const IntLook *look, int pass, int view) {
    GLuint p = r->fwd.id;
    glUseProgram(p);
    if (r->fwd_cached != p) {
        static const char *names[U_CACHED] = {"uViewProj", "uCam", "uTint", "uMat", "uMat2", "uEmissive"};
        for (int i = 0; i < U_CACHED; i++) r->loc[i] = ul(p, names[i]);
        glUniform1i(ul(p, "uAlbedoTex"), TU_ALBEDO);
        glUniform1i(ul(p, "uNormalTex"), TU_NORMAL);
        glUniform1i(ul(p, "uOrmTex"), TU_ORM);
        glUniform1i(ul(p, "uShadowTex"), TU_SHADOW);
        glUniform1i(ul(p, "uBlockerTex"), TU_BLOCKER);
        glUniform1i(ul(p, "uShR"), TU_SH_R);
        glUniform1i(ul(p, "uShG"), TU_SH_G);
        glUniform1i(ul(p, "uShB"), TU_SH_B);
        glUniform1i(ul(p, "uEnvTex"), TU_ENV);
        glUniform1i(ul(p, "uAOTex"), TU_AO);
        glUniform1i(ul(p, "uFlashShadow"), TU_FLASH_SHADOW);
        glUniform1i(ul(p, "uFlashCookie"), TU_FLASH_COOKIE);
        glUniform1i(ul(p, "uFlashBounceTex"), TU_FLASH_BOUNCE);
        r->fwd_cached = p;
    }
    glUniform1i(ul(p, "uPass"), pass);
    glUniform1i(ul(p, "uDebugView"), view);

    int n = s->light_count;
    float pos[INT_MAX_LIGHTS * 3], hu[INT_MAX_LIGHTS * 3], hv[INT_MAX_LIGHTS * 3], nr[INT_MAX_LIGHTS * 3],
          rad[INT_MAX_LIGHTS * 3], mats[INT_MAX_LIGHTS * 16], rects[INT_MAX_LIGHTS * 4];
    for (int i = 0; i < n; i++) {
        const IntLight *l = &s->lights[i];
        memcpy(pos + i * 3, &l->pos, 12);
        memcpy(hu + i * 3, &l->half_u, 12);
        memcpy(hv + i * 3, &l->half_v, 12);
        memcpy(nr + i * 3, &l->normal, 12);
        memcpy(rad + i * 3, &l->radiance, 12);
        memcpy(mats + i * 16, r->shadow_mat[i].m, 64);
        memcpy(rects + i * 4, r->shadow_rect[i], 16);
    }
    glUniform1i(ul(p, "uLightCount"), n);
    if (n) {
        glUniform3fv(ul(p, "uLightPos"), n, pos);
        glUniform3fv(ul(p, "uLightU"), n, hu);
        glUniform3fv(ul(p, "uLightV"), n, hv);
        glUniform3fv(ul(p, "uLightN"), n, nr);
        glUniform3fv(ul(p, "uLightRadiance"), n, rad);
        glUniformMatrix4fv(ul(p, "uShadowMat"), n, GL_FALSE, mats);
        glUniform4fv(ul(p, "uShadowRect"), n, rects);
    }
    glUniform4f(ul(p, "uShadowProj"), SHADOW_NEAR, SHADOW_FAR, tanf(SHADOW_FOV * 0.5f), r->bake_softness);
    glUniform2f(ul(p, "uShadowTexel"), 1.0f / SHADOW_RES, 1.0f / BLOCKER_RES);

    set_v3(p, "uProbeOrigin", r->probe_origin);
    glUniform3f(ul(p, "uProbeInvExtent"), 1.0f / r->probe_extent.x, 1.0f / r->probe_extent.y, 1.0f / r->probe_extent.z);
    set_v3(p, "uEnvPos", s->env_pos);
    set_v3(p, "uRoomMin", s->room_min);
    set_v3(p, "uRoomMax", s->room_max);
    glUniform2f(ul(p, "uEnv"), ENV_MAX_LOD, look->reflection);
    glUniform1i(ul(p, "uProxyCount"), s->proxy_count);
    if (s->proxy_count) {
        float mn[INT_MAX_PROXIES * 3], mx[INT_MAX_PROXIES * 3];
        for (int i = 0; i < s->proxy_count; i++) {
            memcpy(mn + i * 3, &s->proxies[i].min, 12);
            memcpy(mx + i * 3, &s->proxies[i].max, 12);
        }
        glUniform3fv(ul(p, "uProxyMin"), s->proxy_count, mn);
        glUniform3fv(ul(p, "uProxyMax"), s->proxy_count, mx);
    }
    set_v3(p, "uProxyAlbedo", s->proxy_albedo);
    glUniform2f(ul(p, "uAO"), 0.0f, 0.0f);
    glUniform1f(ul(p, "uSceneLight"), 1.0f); /* the bakes always see the lights on, and no torch */
    glUniform4f(ul(p, "uFlashPos"), 0.0f, 0.0f, 0.0f, 0.0f);
    glUniform2f(ul(p, "uGloss"), 0.22f, 1.0f); /* ... and the materials as authored */

    glActiveTexture(GL_TEXTURE0 + TU_SHADOW);  glBindTexture(GL_TEXTURE_2D, r->shadow_tex);
    glActiveTexture(GL_TEXTURE0 + TU_BLOCKER); glBindTexture(GL_TEXTURE_2D, r->blocker_tex);
    for (int c = 0; c < 3; c++) { glActiveTexture(GL_TEXTURE0 + TU_SH_R + c); glBindTexture(GL_TEXTURE_3D, r->sh_tex[c]); }
    /* the reflection probe must not be bound while it is being rendered */
    glActiveTexture(GL_TEXTURE0 + TU_ENV);     glBindTexture(GL_TEXTURE_CUBE_MAP, pass == PASS_ENV ? 0 : r->env_tex);
    glActiveTexture(GL_TEXTURE0 + TU_AO);      glBindTexture(GL_TEXTURE_2D, r->white_tex);
    glActiveTexture(GL_TEXTURE0 + TU_FLASH_SHADOW); glBindTexture(GL_TEXTURE_2D, r->flash_shadow);
    glActiveTexture(GL_TEXTURE0 + TU_FLASH_COOKIE); glBindTexture(GL_TEXTURE_2D, r->flash_cookie);
    glActiveTexture(GL_TEXTURE0 + TU_FLASH_BOUNCE); glBindTexture(GL_TEXTURE_2D, r->white_tex);
    glActiveTexture(GL_TEXTURE0);
}

static void fwd_view(const IntRenderer *r, M4 vp, V3 cam) {
    glUniformMatrix4fv(r->loc[U_VIEWPROJ], 1, GL_FALSE, vp.m);
    glUniform3f(r->loc[U_CAM], cam.x, cam.y, cam.z);
}

/* The torch's beam as it lands on a wall, looking down the axis: hot centre, the dimmer disc the
   reflector throws around it with its faint rings and bright rim, and none of it quite even. */
static GLuint make_cookie(void) {
    static float px[COOKIE_RES * COOKIE_RES];
    for (int y = 0; y < COOKIE_RES; y++)
        for (int x = 0; x < COOKIE_RES; x++) {
            float u = ((float)x + 0.5f) / COOKIE_RES * 2.0f - 1.0f, v = ((float)y + 0.5f) / COOKIE_RES * 2.0f - 1.0f;
            float a = atan2f(v, u), r = sqrtf(u * u + v * v);
            r *= 1.0f + 0.018f * sinf(3.0f * a + 0.7f) + 0.010f * sinf(7.0f * a + 2.1f) + 0.006f * sinf(13.0f * a); /* a dented reflector */
            float t = (r - 0.10f) / 0.40f; t = t < 0 ? 0 : (t > 1 ? 1 : t);
            float core = 1.0f - t * t * (3.0f - 2.0f * t);
            t = (r - 0.76f) / 0.09f; t = t < 0 ? 0 : (t > 1 ? 1 : t);
            float disc = 1.0f - t * t * (3.0f - 2.0f * t);
            float rings = 0.022f * sinf(r * 31.0f) * (r > 0.3f ? 1.0f : r / 0.3f);
            float rim = 0.09f * expf(-(r - 0.73f) * (r - 0.73f) / (0.035f * 0.035f));
            float hole = 0.10f * expf(-r * r / (0.045f * 0.045f)); /* the bulb's own shadow */
            float smudge = 1.0f + 0.05f * sinf(u * 9.0f + 1.3f) * sinf(v * 7.0f + 0.4f) + 0.03f * sinf(u * 23.0f - v * 17.0f);
            float val = (core * 0.62f + disc * (0.34f + rings + rim) - hole * core) * smudge;
            px[y * COOKIE_RES + x] = val > 0 ? val : 0;
        }
    return make_tex2d(GL_R16F, COOKIE_RES, COOKIE_RES, GL_RED, GL_FLOAT, px, GL_LINEAR);
}

/* ---- init --------------------------------------------------------------------------------------- */

int interior_init(IntRenderer *r) {
    memset(r, 0, sizeof *r);
    shader_set_common("interior_common.glsl");
    if (!shader_program_load(&r->fwd, "interior.vert", "interior.frag") ||
        !shader_program_load(&r->depth, "interior.vert", "interior_depth.frag") ||
        !shader_program_load(&r->gbuf, "interior.vert", "interior_normal.frag") ||
        !shader_program_load(&r->ao, "interior_fs.vert", "interior_ao.frag") ||
        !shader_program_load(&r->blur, "interior_fs.vert", "interior_ao_blur.frag") ||
        !shader_program_load(&r->post, "interior_fs.vert", "interior_post.frag") ||
        !shader_program_load(&r->dof, "interior_fs.vert", "interior_dof.frag") ||
        !shader_program_load(&r->dof_mix, "interior_fs.vert", "interior_dof_mix.frag") ||
        !shader_program_load(&r->meter, "interior_fs.vert", "interior_meter.frag") ||
        !shader_program_load(&r->adapt, "interior_fs.vert", "interior_adapt.frag") ||
        !shader_program_load(&r->bounce, "interior_fs.vert", "interior_bounce.frag") ||
        !shader_program_load(&r->glow, "interior_fs.vert", "interior_glow.frag") ||
        !shader_program_load(&r->glow_blur, "interior_fs.vert", "interior_glow_blur.frag") ||
        !shader_program_load(&r->prefilter, "interior_fs.vert", "interior_prefilter.frag"))
        return 0;
    glGenVertexArrays(1, &r->fs_vao);
    static const unsigned char white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255};
    r->white_tex = make_tex2d(GL_RGBA8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white, GL_NEAREST);
    r->flat_normal_tex = make_tex2d(GL_RGBA8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, flat, GL_NEAREST);
    glGenFramebuffers(1, &r->shadow_fbo);
    glGenFramebuffers(1, &r->capture_fbo);
    glGenFramebuffers(1, &r->env_fbo);
    glGenFramebuffers(1, &r->scene_fbo);
    glGenFramebuffers(1, &r->pre_fbo);
    glGenFramebuffers(2, r->ao_fbo);
    glGenFramebuffers(1, &r->ldr_fbo);
    glGenFramebuffers(1, &r->dof_fbo);
    /* torch: shadow map and beam pattern */
    r->flash_shadow = make_tex2d(GL_DEPTH_COMPONENT24, FLASH_RES, FLASH_RES, GL_DEPTH_COMPONENT, GL_FLOAT, NULL, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &r->flash_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->flash_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, r->flash_shadow, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    check_fbo("torch shadow");
    r->flash_cookie = make_cookie();
    /* what the torch is pointed at: a tiny view down the beam, averaged into one texel */
    r->bcap_tex = make_tex2d(GL_RGBA16F, BOUNCE_RES, BOUNCE_RES, GL_RGBA, GL_FLOAT, NULL, GL_NEAREST);
    r->bcap_depth = make_tex2d(GL_DEPTH_COMPONENT24, BOUNCE_RES, BOUNCE_RES, GL_DEPTH_COMPONENT, GL_FLOAT, NULL, GL_NEAREST);
    glGenFramebuffers(1, &r->bcap_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->bcap_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->bcap_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, r->bcap_depth, 0);
    check_fbo("torch bounce capture");
    glGenFramebuffers(2, r->bounce_fbo);
    for (int i = 0; i < 2; i++) {
        static const float grey[4] = {0.5f, 0.5f, 0.5f, 3.0f};
        r->bounce_tex[i] = make_tex2d(GL_RGBA16F, 1, 1, GL_RGBA, GL_FLOAT, grey, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, r->bounce_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->bounce_tex[i], 0);
        check_fbo("torch bounce");
    }
    glGenFramebuffers(2, r->glow_fbo);
    /* eye adaptation: the meter, and the gain it settles on (two texels, each frame reads one and writes the other) */
    r->meter_tex = make_tex2d(GL_RGBA16F, 8, 8, GL_RGBA, GL_FLOAT, NULL, GL_NEAREST);
    glGenFramebuffers(1, &r->meter_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->meter_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->meter_tex, 0);
    check_fbo("light meter");
    glGenFramebuffers(2, r->expo_fbo);
    for (int i = 0; i < 2; i++) {
        static const float one[4] = {1.0f, 1.0f, 0.0f, 1.0f};
        r->expo_tex[i] = make_tex2d(GL_RGBA16F, 1, 1, GL_RGBA, GL_FLOAT, one, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, r->expo_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->expo_tex[i], 0);
        check_fbo("exposure");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    r->scene_light = 1.0f;
    r->flash_color = v3(1.0f, 0.96f, 0.88f);
    r->flash_cone = 1.0f; r->flash_spec = 1.0f; r->flash_bounce = 1.0f;
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    return 1;
}

int interior_reload_shaders(IntRenderer *r) {
    GLuint before = r->fwd.id, before_pf = r->prefilter.id;
    shader_reload_if_changed(&r->fwd);
    shader_reload_if_changed(&r->prefilter);
    shader_reload_if_changed(&r->depth);
    shader_reload_if_changed(&r->gbuf);
    shader_reload_if_changed(&r->ao);
    shader_reload_if_changed(&r->blur);
    shader_reload_if_changed(&r->post);
    shader_reload_if_changed(&r->dof);
    shader_reload_if_changed(&r->dof_mix);
    shader_reload_if_changed(&r->meter);
    shader_reload_if_changed(&r->adapt);
    shader_reload_if_changed(&r->bounce);
    shader_reload_if_changed(&r->glow);
    shader_reload_if_changed(&r->glow_blur);
    /* probes and the reflection probe are rendered with the forward shader */
    return r->fwd.id != before || r->prefilter.id != before_pf;
}

/* ---- bake: shadows ------------------------------------------------------------------------------ */

/* Separable min / box filters on a square float image (n <= BLOCKER_RES), in place via tmp. Both
   run in time independent of the radius (van Herk for the minimum, prefix sums for the box), so
   the blocker map can be rebuilt every frame. Windows are clipped at the borders. */
static void filter_rows(float *img, float *tmp, int n, int radius, int take_min) {
    float line[BLOCKER_RES], g[BLOCKER_RES], h[BLOCKER_RES];
    double sum[BLOCKER_RES + 1];
    int k = 2 * radius + 1;
    for (int pass = 0; pass < 2; pass++) { /* horizontal, then vertical */
        int stride = pass ? n : 1;
        for (int row = 0; row < n; row++) {
            const float *src = img + (pass ? row : row * n);
            float *dst = tmp + (pass ? row : row * n);
            for (int i = 0; i < n; i++) line[i] = src[i * stride];
            if (take_min) {
                /* g: running minimum from the start of each block of k; h: from its end */
                for (int i = 0; i < n; i++) g[i] = (i % k == 0 || line[i] < g[i - 1]) ? line[i] : g[i - 1];
                for (int i = n - 1; i >= 0; i--) h[i] = (i == n - 1 || i % k == k - 1 || line[i] < h[i + 1]) ? line[i] : h[i + 1];
                for (int i = 0; i < n; i++) {
                    int lo = i - radius < 0 ? 0 : i - radius, hi = i + radius > n - 1 ? n - 1 : i + radius;
                    float v;
                    if (lo / k == hi / k) v = lo % k == 0 ? g[hi] : h[lo]; /* clipped window inside one block */
                    else v = h[lo] < g[hi] ? h[lo] : g[hi];
                    dst[i * stride] = v;
                }
            } else {
                sum[0] = 0.0;
                for (int i = 0; i < n; i++) sum[i + 1] = sum[i] + (double)line[i];
                for (int i = 0; i < n; i++) {
                    int lo = i - radius < 0 ? 0 : i - radius, hi = i + radius > n - 1 ? n - 1 : i + radius;
                    dst[i * stride] = (float)((sum[hi + 1] - sum[lo]) / (double)(hi - lo + 1));
                }
            }
        }
        memcpy(img, tmp, (size_t)n * n * sizeof(float));
    }
}

static void bake_shadows(IntRenderer *r, const IntScene *s, const IntLook *look) {
    int n = s->light_count > 0 ? s->light_count : 1;
    int w = SHADOW_RES * n;
    r->bake_softness = look->shadow_softness;

    if (r->shadow_lights != n) { /* (re)create the atlas and the scratch buffers for this many lights */
        if (r->shadow_tex) { glDeleteTextures(1, &r->shadow_tex); glDeleteTextures(1, &r->blocker_tex); }
        r->shadow_tex = make_tex2d(GL_DEPTH_COMPONENT24, w, SHADOW_RES, GL_DEPTH_COMPONENT, GL_FLOAT, NULL, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        r->blocker_tex = make_tex2d(GL_R16F, BLOCKER_RES * n, BLOCKER_RES, GL_RED, GL_FLOAT, NULL, GL_LINEAR);
        if (r->blocker_depth) glDeleteTextures(1, &r->blocker_depth);
        r->blocker_depth = make_tex2d(GL_DEPTH_COMPONENT24, BLOCKER_RES * n, BLOCKER_RES, GL_DEPTH_COMPONENT, GL_FLOAT, NULL, GL_NEAREST);
        r->shadow_depth = (float *)realloc(r->shadow_depth, (size_t)BLOCKER_RES * n * BLOCKER_RES * sizeof(float));
        r->blocker_px = (float *)realloc(r->blocker_px, (size_t)BLOCKER_RES * (n + 2) * BLOCKER_RES * sizeof(float));
        r->shadow_lights = n;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, r->shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, r->shadow_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    check_fbo("shadow atlas");

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glViewport(0, 0, w, SHADOW_RES);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.0f);
    glUseProgram(r->depth.id);
    for (int i = 0; i < s->light_count; i++) {
        const IntLight *l = &s->lights[i];
        V3 up = fabsf(l->normal.y) > 0.9f ? v3(0, 0, -1) : v3(0, 1, 0);
        M4 view = look_matrix(l->pos, l->normal, up);
        r->shadow_mat[i] = m4_mul(m4_perspective(SHADOW_FOV, 1.0f, SHADOW_NEAR, SHADOW_FAR), view);
        r->shadow_rect[i][0] = 1.0f / (float)n; r->shadow_rect[i][1] = 1.0f;
        r->shadow_rect[i][2] = (float)i / (float)n; r->shadow_rect[i][3] = 0.0f;
        glViewport(i * SHADOW_RES, 0, SHADOW_RES, SHADOW_RES);
        glUniformMatrix4fv(ul(r->depth.id, "uViewProj"), 1, GL_FALSE, r->shadow_mat[i].m);
        draw_scene(r, s, 0, MAT_CASTS_SHADOW, 0);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);

    /* the same views again at the blocker map's resolution: that is all the CPU needs to read back,
       which keeps this cheap enough to redo every frame while an object is dragged */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, r->blocker_depth, 0);
    glViewport(0, 0, BLOCKER_RES * n, BLOCKER_RES);
    glClear(GL_DEPTH_BUFFER_BIT);
    for (int i = 0; i < s->light_count; i++) {
        glViewport(i * BLOCKER_RES, 0, BLOCKER_RES, BLOCKER_RES);
        glUniformMatrix4fv(ul(r->depth.id, "uViewProj"), 1, GL_FALSE, r->shadow_mat[i].m);
        draw_scene(r, s, 0, MAT_CASTS_SHADOW, 0);
    }

    /* Blocker map: for every texel, the distance to the closest occluder in its neighbourhood, blurred.
       The shader turns it into a penumbra width. Texels with no occluder nearby stay "infinitely" far,
       which is also how the shader skips the shadow filter for most of the screen. */
    float *depth = r->shadow_depth;
    float *atlas = r->blocker_px;
    float *img = atlas + (size_t)BLOCKER_RES * n * BLOCKER_RES, *tmp = img + (size_t)BLOCKER_RES * BLOCKER_RES;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, BLOCKER_RES * n, BLOCKER_RES, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    /* widest penumbra (occluder close to the light, receiver on the floor) plus the reach of the blur */
    float uv_reach = look->shadow_softness * 0.4f / (2.0f * tanf(SHADOW_FOV * 0.5f));
    int dilate = (int)(uv_reach * BLOCKER_RES) + 2 * BLOCKER_BLUR + 3;
    for (int i = 0; i < n; i++) {
        for (int y = 0; y < BLOCKER_RES; y++)
            for (int x = 0; x < BLOCKER_RES; x++) {
                float dmin = depth[(size_t)y * BLOCKER_RES * n + i * BLOCKER_RES + x];
                img[y * BLOCKER_RES + x] = dmin >= 1.0f ? 1000.0f
                    : SHADOW_NEAR * SHADOW_FAR / (SHADOW_FAR - dmin * (SHADOW_FAR - SHADOW_NEAR));
            }
        filter_rows(img, tmp, BLOCKER_RES, dilate, 1);
        filter_rows(img, tmp, BLOCKER_RES, BLOCKER_BLUR, 0);
        filter_rows(img, tmp, BLOCKER_RES, BLOCKER_BLUR, 0);
        for (int y = 0; y < BLOCKER_RES; y++)
            memcpy(atlas + (size_t)y * BLOCKER_RES * n + i * BLOCKER_RES, img + y * BLOCKER_RES, BLOCKER_RES * sizeof(float));
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, r->blocker_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, BLOCKER_RES * n, BLOCKER_RES, 0, GL_RED, GL_FLOAT, atlas);
}

/* ---- bake: irradiance probes -------------------------------------------------------------------- */

static void upload_sh(IntRenderer *r) {
    for (int c = 0; c < 3; c++) {
        if (!r->sh_tex[c]) r->sh_tex[c] = make_tex(GL_TEXTURE_3D, GL_LINEAR, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_3D, r->sh_tex[c]);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, r->probe_n[0], r->probe_n[1], r->probe_n[2], 0, GL_RGBA, GL_FLOAT, r->sh_cpu[c]);
    }
}

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

/* Sizes the probe grid to the room and allocates what a capture needs. Does nothing if the grid is unchanged. */
static void probes_setup(IntRenderer *r, const IntScene *s) {
    V3 size = v3_add(s->room_max, v3_scale(s->room_min, -1.0f));
    float dims[3] = {size.x, size.y, size.z};
    int n[3];
    for (int a = 0; a < 3; a++) {
        n[a] = (int)(dims[a] / PROBE_SPACING + 0.5f);
        if (n[a] < 2) n[a] = 2;
    }
    r->probe_origin = s->room_min;
    r->probe_extent = size;
    if (r->probe_dir && n[0] == r->probe_n[0] && n[1] == r->probe_n[1] && n[2] == r->probe_n[2]) return;
    memcpy(r->probe_n, n, sizeof n);
    int count = n[0] * n[1] * n[2];

    int F = PROBE_FACE;
    int aw = PROBE_ROW * 6 * F, ah = ((count + PROBE_ROW - 1) / PROBE_ROW) * F;
    if (r->capture_tex) { glDeleteTextures(1, &r->capture_tex); glDeleteTextures(1, &r->capture_depth); }
    r->capture_tex = make_tex2d(GL_RGBA16F, aw, ah, GL_RGBA, GL_FLOAT, NULL, GL_NEAREST);
    r->capture_depth = make_tex2d(GL_DEPTH_COMPONENT24, aw, ah, GL_DEPTH_COMPONENT, GL_FLOAT, NULL, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, r->capture_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->capture_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, r->capture_depth, 0);
    check_fbo("probe atlas");

    /* per face texel: world direction and solid angle (normalised so a full sphere sums to 4 pi) */
    float (*dir)[4] = realloc(r->probe_dir, (size_t)6 * F * F * sizeof *dir);
    float wsum = 0.0f;
    for (int f = 0; f < 6; f++) {
        V3 fw = v3(CUBE_FWD[f][0], CUBE_FWD[f][1], CUBE_FWD[f][2]);
        V3 rt = v3_norm(v3_cross(fw, v3(CUBE_UP[f][0], CUBE_UP[f][1], CUBE_UP[f][2])));
        V3 up = v3_cross(rt, fw);
        for (int j = 0; j < F; j++)
            for (int i = 0; i < F; i++) {
                float x = ((float)i + 0.5f) / (float)F * 2.0f - 1.0f, y = ((float)j + 0.5f) / (float)F * 2.0f - 1.0f;
                V3 d = v3_norm(v3_add(fw, v3_add(v3_scale(rt, x), v3_scale(up, y))));
                float wgt = 1.0f / powf(1.0f + x * x + y * y, 1.5f);
                float *o = dir[(f * F + j) * F + i];
                o[0] = d.x; o[1] = d.y; o[2] = d.z; o[3] = wgt;
                wsum += wgt;
            }
    }
    for (int i = 0; i < 6 * F * F; i++) dir[i][3] *= 4.0f * PI / wsum;
    r->probe_dir = dir;
    r->probe_pixels = (float *)realloc(r->probe_pixels, (size_t)aw * ah * 3 * sizeof(float));
    r->probe_ids = (int *)realloc(r->probe_ids, (size_t)count * sizeof(int));
    for (int c = 0; c < 3; c++) r->sh_cpu[c] = (float *)realloc(r->sh_cpu[c], (size_t)count * 4 * sizeof(float));

    /* live updates visit the probes in a scattered order, so a refresh spreads over the room evenly */
    r->probe_stride = (int)((float)count * 0.618f) | 1;
    while (gcd(r->probe_stride, count) != 1) r->probe_stride += 2;
    r->probe_cursor = 0;
}

/* Captures the probes listed in ids with the lighting as it currently stands (direct light plus the
   bounce stored in the SH textures), and stores their new SH. The caller uploads. */
static void probes_capture(IntRenderer *r, const IntScene *s, const IntLook *look, const int *ids, int n) {
    int nx = r->probe_n[0], ny = r->probe_n[1], nz = r->probe_n[2];
    int F = PROBE_FACE, aw = PROBE_ROW * 6 * F, rows = ((n + PROBE_ROW - 1) / PROBE_ROW) * F;
    V3 size = r->probe_extent;
    float (*dir)[4] = r->probe_dir;
    float *pixels = r->probe_pixels;

    glBindFramebuffer(GL_FRAMEBUFFER, r->capture_fbo);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    fwd_begin(r, s, look, PASS_PROBE, 0);
    for (int k = 0; k < n; k++) { /* probe ids[k] is drawn into atlas slot k */
        int p = ids[k];
        int ix = p % nx, iy = (p / nx) % ny, iz = p / (nx * ny);
        V3 pos = v3(s->room_min.x + ((float)ix + 0.5f) * size.x / (float)nx,
                    s->room_min.y + ((float)iy + 0.5f) * size.y / (float)ny,
                    s->room_min.z + ((float)iz + 0.5f) * size.z / (float)nz);
        for (int f = 0; f < 6; f++) {
            glViewport((k % PROBE_ROW) * 6 * F + f * F, (k / PROBE_ROW) * F, F, F);
            fwd_view(r, cube_view_proj(pos, f), pos);
            draw_scene(r, s, 1, 0, 1);
        }
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, aw, rows, GL_RGB, GL_FLOAT, pixels);

    /* project each probe onto L1 SH and convolve with the cosine lobe, so the shader evaluates
       irradiance / pi as L0 + dot(L1, n):  L0 = mean radiance,  L1 = (1 / 2pi) * sum(radiance * dir) */
    for (int k = 0; k < n; k++) {
        float acc[3][4] = {{0}};
        for (int f = 0; f < 6; f++)
            for (int j = 0; j < F; j++) {
                const float *row = pixels + ((size_t)((k / PROBE_ROW) * F + j) * aw + (k % PROBE_ROW) * 6 * F + f * F) * 3;
                for (int i = 0; i < F; i++) {
                    const float *d = dir[(f * F + j) * F + i];
                    for (int c = 0; c < 3; c++) {
                        float v = row[i * 3 + c] * d[3];
                        acc[c][0] += v; acc[c][1] += v * d[0]; acc[c][2] += v * d[1]; acc[c][3] += v * d[2];
                    }
                }
            }
        for (int q = 0; q < 4; q++) { /* pull the bounce toward grey: see IntLook.bounce_saturation */
            float grey = 0.2126f * acc[0][q] + 0.7152f * acc[1][q] + 0.0722f * acc[2][q];
            for (int c = 0; c < 3; c++) acc[c][q] = grey + (acc[c][q] - grey) * look->bounce_saturation;
        }
        for (int c = 0; c < 3; c++) {
            float *o = r->sh_cpu[c] + (size_t)ids[k] * 4;
            o[0] = acc[c][0] / (4.0f * PI);
            for (int q = 1; q < 4; q++) o[q] = acc[c][q] / (2.0f * PI);
        }
    }
}

/* From scratch: every pass over all probes adds one bounce. */
static void bake_probes(IntRenderer *r, const IntScene *s, const IntLook *look) {
    probes_setup(r, s);
    int count = r->probe_n[0] * r->probe_n[1] * r->probe_n[2];
    for (int c = 0; c < 3; c++) memset(r->sh_cpu[c], 0, (size_t)count * 4 * sizeof(float));
    upload_sh(r); /* bounce 0 sees no indirect light */
    for (int i = 0; i < count; i++) r->probe_ids[i] = i;
    for (int bounce = 0; bounce < PROBE_BOUNCES; bounce++) {
        probes_capture(r, s, look, r->probe_ids, count);
        upload_sh(r);
    }
}

/* ---- bake: reflection probe --------------------------------------------------------------------- */

static void bake_env(IntRenderer *r, const IntScene *s, const IntLook *look) {
    const int levels = (int)ENV_MAX_LOD + 1;
    if (!r->env_tex) {
        r->env_raw = make_tex(GL_TEXTURE_CUBE_MAP, GL_LINEAR_MIPMAP_LINEAR, GL_CLAMP_TO_EDGE);
        for (int f = 0; f < 6; f++)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F, ENV_RES, ENV_RES, 0, GL_RGBA, GL_FLOAT, NULL);
        r->env_tex = make_tex(GL_TEXTURE_CUBE_MAP, GL_LINEAR_MIPMAP_LINEAR, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, levels - 1);
        for (int m = 0; m < levels; m++)
            for (int f = 0; f < 6; f++)
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, m, GL_RGBA16F, ENV_RES >> m, ENV_RES >> m, 0, GL_RGBA, GL_FLOAT, NULL);
        r->env_depth = make_tex2d(GL_DEPTH_COMPONENT24, ENV_RES, ENV_RES, GL_DEPTH_COMPONENT, GL_FLOAT, NULL, GL_NEAREST);
    }

    /* capture the lit room */
    glBindFramebuffer(GL_FRAMEBUFFER, r->env_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, r->env_depth, 0);
    glViewport(0, 0, ENV_RES, ENV_RES);
    fwd_begin(r, s, look, PASS_ENV, 0);
    for (int f = 0; f < 6; f++) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, r->env_raw, 0);
        if (f == 0) check_fbo("reflection probe");
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        fwd_view(r, cube_view_proj(s->env_pos, f), s->env_pos);
        draw_scene(r, s, 1, MAT_SHELL, 1); /* only the room itself: box projection is exact for it */
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, r->env_raw);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

    /* convolve it per roughness: mip m of env_tex holds roughness m / ENV_MAX_LOD */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glDisable(GL_DEPTH_TEST);
    GLuint p = r->prefilter.id;
    glUseProgram(p);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, r->env_raw);
    glUniform1i(ul(p, "uSrc"), 0);
    glUniform1f(ul(p, "uSrcRes"), (float)ENV_RES);
    glBindVertexArray(r->fs_vao);
    for (int m = 0; m < levels; m++) {
        glViewport(0, 0, ENV_RES >> m, ENV_RES >> m);
        glUniform1f(ul(p, "uRoughness"), (float)m / ENV_MAX_LOD);
        for (int f = 0; f < 6; f++) {
            V3 fw = v3(CUBE_FWD[f][0], CUBE_FWD[f][1], CUBE_FWD[f][2]);
            V3 rt = v3_norm(v3_cross(fw, v3(CUBE_UP[f][0], CUBE_UP[f][1], CUBE_UP[f][2])));
            set_v3(p, "uFaceFwd", fw);
            set_v3(p, "uFaceRight", rt);
            set_v3(p, "uFaceUp", v3_cross(rt, fw));
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, r->env_tex, m);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
    glEnable(GL_DEPTH_TEST);
}

void interior_bake(IntRenderer *r, const IntScene *s, const IntLook *look) {
    double t0 = platform_time();
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    bake_shadows(r, s, look);
    bake_probes(r, s, look);
    bake_env(r, s, look);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    r->dirty = 0;
    r->probes_pending = 0;
    r->bake_ms = (platform_time() - t0) * 1000.0;
}

void interior_invalidate(IntRenderer *r, unsigned what) {
    r->dirty |= what;
    if (what & INT_DIRTY_LIGHTING) r->probes_pending = r->probe_n[0] * r->probe_n[1] * r->probe_n[2] * LIVE_SWEEPS;
}

float interior_bake_progress(const IntRenderer *r) {
    int total = r->probe_n[0] * r->probe_n[1] * r->probe_n[2] * LIVE_SWEEPS;
    if (r->probes_pending <= 0) return 1.0f;
    return total > 0 ? 1.0f - (float)r->probes_pending / (float)total : 0.0f;
}

int interior_bake_step(IntRenderer *r, const IntScene *s, const IntLook *look, double budget_ms) {
    if (!r->dirty && r->probes_pending <= 0) return 0;
    double t0 = platform_time();
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    /* shadows first and in full: they are what makes a moved object sit in the room */
    if (r->dirty & INT_DIRTY_SHADOWS) bake_shadows(r, s, look);

    /* then as many probes as the budget allows. Each one gathers the bounce its neighbours hold
       right now, so repeated sweeps converge on the same multi-bounce result as a full bake. */
    probes_setup(r, s);
    int count = r->probe_n[0] * r->probe_n[1] * r->probe_n[2], captured = 0;
    while (r->probes_pending > 0) {
        int n = r->probes_pending < PROBE_ROW ? r->probes_pending : PROBE_ROW;
        for (int i = 0; i < n; i++)
            r->probe_ids[i] = (int)(((long long)(r->probe_cursor + i) * r->probe_stride) % count);
        probes_capture(r, s, look, r->probe_ids, n);
        int before = r->probe_cursor / count;
        r->probe_cursor += n;
        r->probes_pending -= n;
        captured = 1;
        /* the reflection probe follows once per sweep, and once more when the probes have settled */
        if (r->probe_cursor / count != before) { r->probe_cursor %= count; r->dirty |= INT_DIRTY_ENV; }
        if (r->probes_pending <= 0) r->dirty |= INT_DIRTY_ENV;
        if ((platform_time() - t0) * 1000.0 >= budget_ms) break;
    }
    if (captured) upload_sh(r);
    if (r->dirty & INT_DIRTY_ENV) bake_env(r, s, look);
    r->dirty = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    r->step_ms = (platform_time() - t0) * 1000.0;
    return r->probes_pending > 0;
}

/* ---- frame -------------------------------------------------------------------------------------- */

static void ensure_targets(IntRenderer *r, int w, int h, int samples) {
    if (samples != 2 && samples != 8) samples = 4;
    if (r->w == w && r->h == h && r->samples == samples) return;
    if (r->scene_color) {
        GLuint old[10] = {r->scene_color, r->scene_depth, r->pre_normal, r->pre_depth, r->ao_tex[0], r->ao_tex[1], r->ldr_tex, r->dof_tex,
                          r->glow_tex[0], r->glow_tex[1]};
        glDeleteTextures(10, old);
    }
    r->w = w; r->h = h; r->samples = samples;

    glGenTextures(1, &r->scene_color);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, r->scene_color);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA16F, w, h, GL_TRUE);
    glGenTextures(1, &r->scene_depth);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, r->scene_depth);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_DEPTH_COMPONENT24, w, h, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, r->scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, r->scene_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, r->scene_depth, 0);
    check_fbo("scene");

    r->pre_normal = make_tex2d(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL, GL_NEAREST);
    r->pre_depth = make_tex2d(GL_DEPTH_COMPONENT24, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, NULL, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, r->pre_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->pre_normal, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, r->pre_depth, 0);
    check_fbo("prepass");

    for (int i = 0; i < 2; i++) {
        r->ao_tex[i] = make_tex2d(GL_R8, w, h, GL_RED, GL_UNSIGNED_BYTE, NULL, i ? GL_LINEAR : GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, r->ao_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->ao_tex[i], 0);
        check_fbo("ambient obscurance");
    }

    r->ldr_tex = make_tex2d(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, r->ldr_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->ldr_tex, 0);
    check_fbo("depth of field source");
    r->dof_tex = make_tex2d(GL_RGBA8, (w + 1) / 2, (h + 1) / 2, GL_RGBA, GL_UNSIGNED_BYTE, NULL, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, r->dof_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->dof_tex, 0);
    check_fbo("depth of field blur");
    for (int i = 0; i < 2; i++) {
        r->glow_tex[i] = make_tex2d(GL_RGBA16F, (w + 3) / 4, (h + 3) / 4, GL_RGBA, GL_FLOAT, NULL, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, r->glow_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->glow_tex[i], 0);
        check_fbo("glow");
    }
}

float interior_exposure_gain(IntRenderer *r, float *metered) {
    float px[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    glBindFramebuffer(GL_FRAMEBUFFER, r->expo_fbo[r->expo_cur]);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (metered) *metered = px[1];
    return px[0];
}

void interior_render(IntRenderer *r, const IntScene *s, const IntLook *look, V3 cam, V3 fwd, int w, int h, int view) {
    if (w < 1 || h < 1) return;
    ensure_targets(r, w, h, look->msaa);
    float aspect = (float)w / (float)h, tan_half = tanf(look->fov * 0.5f);
    M4 view_m = m4_view(cam, fwd);
    if (r->cam_roll != 0.0f) {
        M4 roll = m4_identity();
        float c = cosf(r->cam_roll), sn = sinf(r->cam_roll);
        roll.m[0] = c; roll.m[1] = sn; roll.m[4] = -sn; roll.m[5] = c;
        view_m = m4_mul(roll, view_m);
    }
    M4 vp = m4_mul(m4_perspective(look->fov, aspect, CAM_NEAR, CAM_FAR), view_m);
    int use_ao = look->ao_strength > 0.0f;

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    /* the torch's shadow map: furniture as the torch sees it */
    M4 flash_mat = m4_identity();
    if (r->flash > 0.0f) {
        V3 up = fabsf(r->flash_dir.y) > 0.999f ? v3(0, 0, -1) : v3(0, 1, 0);
        flash_mat = m4_mul(m4_perspective(FLASH_FOV, 1.0f, 0.05f, 30.0f), look_matrix(r->flash_pos, r->flash_dir, up));
        glBindFramebuffer(GL_FRAMEBUFFER, r->flash_fbo);
        glViewport(0, 0, FLASH_RES, FLASH_RES);
        glEnable(GL_DEPTH_TEST);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.5f, 2.0f);
        glUseProgram(r->depth.id);
        glUniformMatrix4fv(ul(r->depth.id, "uViewProj"), 1, GL_FALSE, flash_mat.m);
        draw_scene(r, s, 0, MAT_CASTS_SHADOW, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);

        /* what the beam lands on, for the light it throws back */
        glBindFramebuffer(GL_FRAMEBUFFER, r->bcap_fbo);
        glViewport(0, 0, BOUNCE_RES, BOUNCE_RES);
        glClearColor(0.0f, 0.0f, 0.0f, 3.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        fwd_begin(r, s, look, PASS_PROBE, VIEW_BOUNCE_CAPTURE);
        fwd_view(r, m4_mul(m4_perspective(BOUNCE_FOV, 1.0f, 0.05f, 30.0f), look_matrix(r->flash_pos, r->flash_dir, up)), r->flash_pos);
        draw_scene(r, s, 1, 0, 1);
        glDisable(GL_DEPTH_TEST);
        int next = 1 - r->bounce_cur;
        glBindFramebuffer(GL_FRAMEBUFFER, r->bounce_fbo[next]);
        glViewport(0, 0, 1, 1);
        glUseProgram(r->bounce.id);
        glBindVertexArray(r->fs_vao);
        glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, r->bounce_tex[r->bounce_cur]);
        glActiveTexture(GL_TEXTURE0);     glBindTexture(GL_TEXTURE_2D, r->bcap_tex);
        glUniform1i(ul(r->bounce.id, "uCapture"), 0);
        glUniform1i(ul(r->bounce.id, "uPrev"), 1);
        glUniform1f(ul(r->bounce.id, "uBlend"), r->frame_dt > 0.0f ? 1.0f - expf(-r->frame_dt * 10.0f) : 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        r->bounce_cur = next;
        glEnable(GL_DEPTH_TEST);
    }
    glViewport(0, 0, w, h);

    if (use_ao) {
        /* depth + normal prepass */
        glBindFramebuffer(GL_FRAMEBUFFER, r->pre_fbo);
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.5f, 0.5f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(r->gbuf.id);
        glUniformMatrix4fv(ul(r->gbuf.id, "uViewProj"), 1, GL_FALSE, vp.m);
        draw_scene(r, s, 0, 0, 1);

        /* obscurance, then the 4x4 blur that removes its sampling pattern */
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(r->fs_vao);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, r->pre_depth);
        glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, r->pre_normal);
        glBindFramebuffer(GL_FRAMEBUFFER, r->ao_fbo[0]);
        GLuint p = r->ao.id;
        glUseProgram(p);
        glUniform1i(ul(p, "uDepthTex"), 0);
        glUniform1i(ul(p, "uNormalTex"), 1);
        glUniformMatrix4fv(ul(p, "uView"), 1, GL_FALSE, view_m.m);
        glUniform4f(ul(p, "uProjInfo"), tan_half * aspect, tan_half, CAM_NEAR, CAM_FAR);
        glUniform4f(ul(p, "uAOParams"), look->ao_radius, 1.0f, 0.012f, (float)h / (2.0f * tan_half));
        glUniform2f(ul(p, "uInvScreen"), 1.0f / (float)w, 1.0f / (float)h);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glBindFramebuffer(GL_FRAMEBUFFER, r->ao_fbo[1]);
        p = r->blur.id;
        glUseProgram(p);
        glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, r->ao_tex[0]);
        glUniform1i(ul(p, "uDepthTex"), 0);
        glUniform1i(ul(p, "uAOTex"), 1);
        glUniform4f(ul(p, "uProjInfo"), tan_half * aspect, tan_half, CAM_NEAR, CAM_FAR);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE0);
    }

    /* forward pass into the multisampled HDR target */
    glBindFramebuffer(GL_FRAMEBUFFER, r->scene_fbo);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    fwd_begin(r, s, look, PASS_FINAL, view);
    glUniform1f(ul(r->fwd.id, "uSceneLight"), r->scene_light);
    glUniform2f(ul(r->fwd.id, "uGloss"), look->rough_min, look->rough_scale);
    if (r->flash > 0.0f) {
        V3 o = r->flash_pos;
        glUniform4f(ul(r->fwd.id, "uFlashPos"), o.x, o.y, o.z, r->flash);
        set_v3(r->fwd.id, "uFlashDir", r->flash_dir);
        glUniform1f(ul(r->fwd.id, "uFlashBounce"), r->flash * 0.013f * r->flash_bounce);
        set_v3(r->fwd.id, "uFlashColor", r->flash_color);
        glUniform4f(ul(r->fwd.id, "uFlashShape"), r->flash_cone, r->flash_falloff, r->flash_harsh, r->flash_spec);
        glActiveTexture(GL_TEXTURE0 + TU_FLASH_BOUNCE);
        glBindTexture(GL_TEXTURE_2D, r->bounce_tex[r->bounce_cur]);
        glActiveTexture(GL_TEXTURE0);
        glUniformMatrix4fv(ul(r->fwd.id, "uFlashMat"), 1, GL_FALSE, flash_mat.m);
    }
    fwd_view(r, vp, cam);
    glUniform2f(ul(r->fwd.id, "uInvScreen"), 1.0f / (float)w, 1.0f / (float)h);
    if (use_ao) {
        glUniform2f(ul(r->fwd.id, "uAO"), look->ao_strength, look->ao_direct);
        glActiveTexture(GL_TEXTURE0 + TU_AO);
        glBindTexture(GL_TEXTURE_2D, r->ao_tex[1]);
        glActiveTexture(GL_TEXTURE0);
    }
    draw_scene(r, s, 1, 0, 1);

    /* eye adaptation: meter the frame, move the exposure gain towards what the meter asks for */
    int adapt = look->adapt_max > 1.0f && view == INT_VIEW_FINAL;
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(r->fs_vao);
    glActiveTexture(GL_TEXTURE0);
    if (adapt) {
        glBindFramebuffer(GL_FRAMEBUFFER, r->meter_fbo);
        glViewport(0, 0, 8, 8);
        glUseProgram(r->meter.id);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, r->scene_color);
        glUniform1i(ul(r->meter.id, "uScene"), 0);
        glUniform2f(ul(r->meter.id, "uSize"), (float)w, (float)h);
        glUniform1f(ul(r->meter.id, "uFloor"), ADAPT_FLOOR);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        int next = 1 - r->expo_cur;
        glBindFramebuffer(GL_FRAMEBUFFER, r->expo_fbo[next]);
        glViewport(0, 0, 1, 1);
        glUseProgram(r->adapt.id);
        glBindTexture(GL_TEXTURE_2D, r->meter_tex);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, r->expo_tex[r->expo_cur]);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(ul(r->adapt.id, "uMeter"), 0);
        glUniform1i(ul(r->adapt.id, "uPrev"), 1);
        glUniform4f(ul(r->adapt.id, "uAdapt"), ADAPT_KEY, look->adapt_max, r->frame_dt, 0.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        r->expo_cur = next;
        glViewport(0, 0, w, h);
    }

    /* glow: bloom and the torch beam in the air, at quarter resolution, blurred twice */
    int glow = view == INT_VIEW_FINAL && (look->bloom > 0.0f || look->lens_dirt > 0.0f || (look->air > 0.0f && r->flash > 0.0f));
    if (glow) {
        int gw = (w + 3) / 4, gh = (h + 3) / 4;
        V3 right = v3(view_m.m[0], view_m.m[4], view_m.m[8]), upv = v3(view_m.m[1], view_m.m[5], view_m.m[9]);
        V3 ahead = v3(-view_m.m[2], -view_m.m[6], -view_m.m[10]);
        GLuint g = r->glow.id;
        glBindFramebuffer(GL_FRAMEBUFFER, r->glow_fbo[0]);
        glViewport(0, 0, gw, gh);
        glUseProgram(g);
        glActiveTexture(GL_TEXTURE0);     glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, r->scene_color);
        glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, r->scene_depth);
        glActiveTexture(GL_TEXTURE0 + 2); glBindTexture(GL_TEXTURE_2D, adapt ? r->expo_tex[r->expo_cur] : r->white_tex);
        glActiveTexture(GL_TEXTURE0 + 3); glBindTexture(GL_TEXTURE_2D, r->flash_shadow);
        glActiveTexture(GL_TEXTURE0 + 4); glBindTexture(GL_TEXTURE_2D, r->flash_cookie);
        glUniform1i(ul(g, "uScene"), 0);
        glUniform1i(ul(g, "uSceneDepth"), 1);
        glUniform1i(ul(g, "uExposureTex"), 2);
        glUniform1i(ul(g, "uFlashShadow"), 3);
        glUniform1i(ul(g, "uFlashCookie"), 4);
        glUniform2f(ul(g, "uSize"), (float)w, (float)h);
        glUniform4f(ul(g, "uGlow"), look->exposure, look->bloom, look->air, r->grain_seed);
        glUniform4f(ul(g, "uProj"), tan_half * aspect, tan_half, CAM_NEAR, CAM_FAR);
        set_v3(g, "uCam", cam);
        set_v3(g, "uRight", right);
        set_v3(g, "uUp", upv);
        set_v3(g, "uFwd", ahead);
        glUniform4f(ul(g, "uFlashPos"), r->flash_pos.x, r->flash_pos.y, r->flash_pos.z, r->flash);
        glUniformMatrix4fv(ul(g, "uFlashMat"), 1, GL_FALSE, flash_mat.m);
        set_v3(g, "uFlashColor", r->flash_color);
        glUniform4f(ul(g, "uFlashShape"), r->flash_cone, r->flash_falloff, r->flash_harsh, r->flash_spec);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
        glActiveTexture(GL_TEXTURE0);     glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

        g = r->glow_blur.id;
        glUseProgram(g);
        glUniform1i(ul(g, "uTex"), 0);
        static const float spread[2] = {1.0f, 2.6f}; /* tap spacing in texels: a tight pass, then a wide one */
        for (int i = 0; i < 4; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, r->glow_fbo[1 - (i & 1)]);
            glBindTexture(GL_TEXTURE_2D, r->glow_tex[i & 1]);
            if (i & 1) glUniform2f(ul(g, "uStep"), 0.0f, spread[i / 2] / (float)gh);
            else glUniform2f(ul(g, "uStep"), spread[i / 2] / (float)gw, 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glViewport(0, 0, w, h);
    }

    /* resolve, tonemap and grade: to the window, or to a texture when the near field gets blurred after */
    int dof = look->dof_strength > 0.0f && view == INT_VIEW_FINAL && look->dof_start > look->dof_full && look->dof_full > 0.0f;
    int fx = view == INT_VIEW_FINAL && (look->chroma > 0.0f || (look->lens_dirt > 0.0f && glow));
    int lens = (look->lens_curve > 0.0f && view == INT_VIEW_FINAL) || fx; /* the last pass also does fringing and lens dirt */
    glBindFramebuffer(GL_FRAMEBUFFER, (dof || lens) ? r->ldr_fbo : r->out_fbo);
    if (!(dof || lens)) glViewport(r->out_x, r->out_y, w, h);
    glDisable(GL_DEPTH_TEST);
    GLuint p = r->post.id;
    glUseProgram(p);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, r->scene_color);
    glUniform1i(ul(p, "uScene"), 0);
    glUniform1i(ul(p, "uSamples"), r->samples);
    glUniform2f(ul(p, "uOrigin"), (dof || lens) ? 0.0f : (float)r->out_x, (dof || lens) ? 0.0f : (float)r->out_y);
    int raw = view == INT_VIEW_ALBEDO || view == INT_VIEW_NORMAL || view == INT_VIEW_AO ||
              view == INT_VIEW_SHADOW || view == INT_VIEW_ROUGHNESS;
    glUniform1i(ul(p, "uRaw"), raw);
    glUniform4f(ul(p, "uGrade"), look->exposure, look->saturation, look->contrast, look->vignette);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, r->scene_depth);
    glUniform1i(ul(p, "uSceneDepth"), 1);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, adapt ? r->expo_tex[r->expo_cur] : r->white_tex);
    glUniform1i(ul(p, "uExposureTex"), 2);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, glow ? r->glow_tex[0] : r->white_tex);
    glUniform1i(ul(p, "uGlowTex"), 3);
    glUniform3f(ul(p, "uGlowInfo"), 1.0f / (float)w, 1.0f / (float)h, glow ? 1.0f : 0.0f);
    glUniform4f(ul(p, "uFilm"), look->grain, look->grain_size, look->grade_cast, look->black_crush);
    glUniform2f(ul(p, "uNight"), look->adapt_max, r->grain_seed);
    glActiveTexture(GL_TEXTURE0 + 1);
    /* blur grows with 1/distance, like a lens focused far away: 0 at dof_start, 1 at dof_full */
    if (dof) glUniform4f(ul(p, "uDof"), CAM_NEAR, CAM_FAR, 1.0f / look->dof_start, 1.0f / (1.0f / look->dof_full - 1.0f / look->dof_start));
    else glUniform4f(ul(p, "uDof"), CAM_NEAR, CAM_FAR, 0.0f, 0.0f);
    glUniform2f(ul(p, "uDofEdge"), look->dof_edge, look->dof_defocus);
    glBindVertexArray(r->fs_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

    glViewport(0, 0, w, h);
    if (dof || lens) glBindTexture(GL_TEXTURE_2D, r->ldr_tex);
    if (dof) {
        /* the foreground, blurred at half resolution and spread past its own outline */
        int hw = (w + 1) / 2, hh = (h + 1) / 2;
        glBindFramebuffer(GL_FRAMEBUFFER, r->dof_fbo);
        glViewport(0, 0, hw, hh);
        p = r->dof.id;
        glUseProgram(p);
        glUniform1i(ul(p, "uTex"), 0);
        glUniform2f(ul(p, "uInvScreen"), 1.0f / (float)w, 1.0f / (float)h);
        glUniform1f(ul(p, "uRadius"), 0.022f * (float)h * look->dof_strength);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (dof || lens) {
        /* laid over the sharp frame, through the lens */
        glBindFramebuffer(GL_FRAMEBUFFER, r->out_fbo);
        glViewport(r->out_x, r->out_y, w, h);
        p = r->dof_mix.id;
        glUseProgram(p);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, r->dof_tex);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(ul(p, "uSharp"), 0);
        glUniform1i(ul(p, "uNear"), 1);
        glUniform1i(ul(p, "uUseNear"), dof);
        glUniform2f(ul(p, "uLens"), look->lens_curve > 0.0f ? look->lens_curve : 0.0f, aspect);
        glActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, glow ? r->glow_tex[0] : r->white_tex);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(ul(p, "uGlowTex"), 2);
        glUniform2f(ul(p, "uFx"), fx ? look->chroma : 0.0f, fx && glow ? look->lens_dirt : 0.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
}
