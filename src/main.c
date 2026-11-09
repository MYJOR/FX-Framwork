#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "gl.h"
#include "mathx.h"
#include "shader.h"
#include "terrain.h"
#include "png.h"
#include "particles.h"
#include "wind.h"
#include "player.h"

#define PI 3.14159265f

/* Everything the look depends on lives here so the final game can reuse it. */
typedef struct {
    float sun_elevation; /* radians above horizon */
    float sun_azimuth;
    float fov;
} Scene;

/* Third-person chase camera. Mouse (right button) orbits; the target is smoothed. */
typedef struct {
    float yaw, pitch;
    float distance;
    V3 target;   /* smoothed look-at point */
    V3 eye;
} ChaseCam;

static V3 sun_dir(const Scene *s) {
    return v3(cosf(s->sun_elevation) * sinf(s->sun_azimuth), sinf(s->sun_elevation),
              -cosf(s->sun_elevation) * cosf(s->sun_azimuth));
}

static void set1f(GLuint p, const char *n, float v) { glUniform1f(glGetUniformLocation(p, n), v); }
static void set3f(GLuint p, const char *n, V3 v) { glUniform3f(glGetUniformLocation(p, n), v.x, v.y, v.z); }

static void common_uniforms(GLuint p, const Scene *s, V3 cam, float time) {
    set3f(p, "uSunDir", sun_dir(s));
    set3f(p, "uCam", cam);
    set1f(p, "uTime", time);
}

static void save_screenshot(const char *path, int w, int h) {
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 3);
    unsigned char *flipped = (unsigned char *)malloc((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++)
        memcpy(flipped + (size_t)y * w * 3, px + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);
    if (png_write_rgb(path, w, h, flipped)) printf("saved %s\n", path);
    else fprintf(stderr, "could not write %s\n", path);
    free(px); free(flipped);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int main(int argc, char **argv) {
    const char *shot = NULL;
    Scene scene = {13.0f * PI / 180.0f, 1.0f, 60.0f * PI / 180.0f};
    V3 start = v3(0, 0, 0);
    float yaw = 0.0f, pitch = 0.12f;
    float time_offset = 0.0f;
    float sim_seconds = 0.0f;   /* --walk: simulate this long with forward input before the shot */
    int run_shot = 0, free_cam_shot = 0, win_w = 1280, win_h = 720;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--sun") && i + 1 < argc) scene.sun_elevation = (float)atof(argv[++i]) * PI / 180.0f;
        else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) yaw = (float)atof(argv[++i]) * PI / 180.0f;
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) pitch = (float)atof(argv[++i]) * PI / 180.0f;
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) time_offset = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--walk") && i + 1 < argc) sim_seconds = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--run")) run_shot = 1;
        else if (!strcmp(argv[i], "--free")) free_cam_shot = 1;
        else if (!strcmp(argv[i], "--pos") && i + 2 < argc) {
            start.x = (float)atof(argv[++i]); start.z = (float)atof(argv[++i]);
        }
    }

    Platform plat;
    if (!platform_init(&plat, "Desert - shader test scene", win_w, win_h)) {
        fprintf(stderr, "platform init failed\n");
        return 1;
    }

    /* find the shader directory regardless of where the exe is launched from */
    const char *dirs[] = {"shaders", "../shaders", "../../shaders"};
    FILE *probe = NULL;
    for (int i = 0; i < 3 && !probe; i++) {
        char path[300];
        snprintf(path, sizeof path, "%s/common.glsl", dirs[i]);
        probe = fopen(path, "rb");
        if (probe) { shader_set_dir(dirs[i]); fclose(probe); }
    }

    Program sky = {0}, sand = {0}, part = {0}, body = {0};
    if (!shader_program_load(&sky, "sky.vert", "sky.frag") ||
        !shader_program_load(&sand, "sand.vert", "sand.frag") ||
        !shader_program_load(&part, "particle.vert", "particle.frag") ||
        !shader_program_load(&body, "capsule.vert", "capsule.frag")) {
        fprintf(stderr, "shader load failed\n");
        return 1;
    }

    Terrain terrain;
    terrain_build(&terrain, 640, 360.0f);
    GLuint empty_vao;
    glGenVertexArrays(1, &empty_vao);

    Player player;
    player_init(&player, start.x, start.z);

    ChaseCam cc = {yaw, pitch, 6.5f, v3(0, 0, 0), v3(0, 0, 0)};
    cc.target = v3_add(player.pos, v3(0, 1.3f, 0));
    V3 free_pos = v3_add(player.pos, v3(0, 3.0f, 0));
    int free_mode = free_cam_shot, f_was_down = 0;

    static Particles particles;
    particles_init(&particles, player.pos);
    for (int i = 0; i < 240; i++)
        particles_update(&particles, 1.0f / 60.0f, player.pos, time_offset - 4.0f + i / 60.0f);

    /* for screenshots: let the player run forward (camera-relative) before the shot */
    if (sim_seconds > 0.0f) {
        V3 f = cam_forward(cc.yaw, 0);
        for (float t = 0; t < sim_seconds; t += 1.0f / 60.0f)
            player_update(&player, 1.0f / 60.0f, f.x, f.z, run_shot);
        cc.target = v3_add(player.pos, v3(0, 1.3f, 0));
    }

    double last = platform_time(), reload_timer = 0;
    int frame = 0;

    while (!plat.quit) {
        platform_poll(&plat);
        double now = platform_time();
        float dt = (float)(now - last);
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        float time = (float)now + time_offset;

        if (plat.keys[VK_ESCAPE]) break;

        /* F toggles free camera (debug) */
        if (plat.keys['F'] && !f_was_down) {
            free_mode = !free_mode;
            if (free_mode) free_pos = cc.eye;
        }
        f_was_down = plat.keys['F'];

        /* mouse orbits the chase camera / turns the free camera */
        cc.yaw += plat.mouse_dx * 0.004f;
        cc.pitch -= plat.mouse_dy * 0.004f;
        cc.pitch = clampf(cc.pitch, -0.35f, 1.35f);
        if (plat.keys['Q']) scene.sun_elevation -= 0.4f * dt;
        if (plat.keys['E']) scene.sun_elevation += 0.4f * dt;
        scene.sun_elevation = clampf(scene.sun_elevation, -0.15f, 1.5f);

        V3 cam_fwd = cam_forward(cc.yaw, cc.pitch);
        V3 cam_right = v3_norm(v3_cross(cam_fwd, v3(0, 1, 0)));
        V3 cam;      /* final eye position */
        V3 view_fwd; /* final view direction */

        if (free_mode) {
            float speed = plat.keys[VK_SHIFT] ? 40.0f : 8.0f;
            if (plat.keys['W']) free_pos = v3_add(free_pos, v3_scale(cam_fwd, speed * dt));
            if (plat.keys['S']) free_pos = v3_add(free_pos, v3_scale(cam_fwd, -speed * dt));
            if (plat.keys['D']) free_pos = v3_add(free_pos, v3_scale(cam_right, speed * dt));
            if (plat.keys['A']) free_pos = v3_add(free_pos, v3_scale(cam_right, -speed * dt));
            if (plat.keys[VK_SPACE]) free_pos.y += speed * dt;
            if (plat.keys[VK_CONTROL]) free_pos.y -= speed * dt;
            float ground = terrain_height(free_pos.x, free_pos.z) + 1.0f;
            if (free_pos.y < ground) free_pos.y = ground;
            cam = free_pos;
            view_fwd = cam_fwd;
        } else {
            /* movement is relative to where the camera looks, flattened onto the ground */
            V3 f = v3_norm(v3(cam_fwd.x, 0, cam_fwd.z));
            float ix = 0, iz = 0;
            if (plat.keys['W']) { ix += f.x; iz += f.z; }
            if (plat.keys['S']) { ix -= f.x; iz -= f.z; }
            if (plat.keys['D']) { ix += cam_right.x; iz += cam_right.z; }
            if (plat.keys['A']) { ix -= cam_right.x; iz -= cam_right.z; }
            player_update(&player, dt, ix, iz, plat.keys[VK_SHIFT]);

            /* smoothed target, then place the eye behind it along the orbit direction */
            V3 want = v3_add(player.pos, v3(0, 1.3f, 0));
            float k = 1.0f - expf(-9.0f * dt);
            cc.target = v3_add(cc.target, v3_scale(v3_add(want, v3_scale(cc.target, -1.0f)), k));
            V3 eye = v3_add(cc.target, v3_scale(cam_fwd, -cc.distance));
            float ground = terrain_height(eye.x, eye.z) + 0.9f;
            if (eye.y < ground) eye.y = ground; /* never dip into a dune */
            cc.eye = eye;
            cam = eye;
            view_fwd = v3_norm(v3_add(cc.target, v3_scale(eye, -1.0f)));
        }

        reload_timer += dt;
        if (reload_timer > 0.5) {
            reload_timer = 0;
            shader_reload_if_changed(&sky);
            shader_reload_if_changed(&sand);
            shader_reload_if_changed(&part);
            shader_reload_if_changed(&body);
        }

        V3 right = v3_norm(v3_cross(view_fwd, v3(0, 1, 0)));
        V3 up = v3_cross(right, view_fwd);
        float aspect = (float)plat.width / (float)(plat.height > 0 ? plat.height : 1);
        glViewport(0, 0, plat.width, plat.height);
        M4 proj = m4_perspective(scene.fov, aspect, 0.3f, 3000.0f);
        M4 view = m4_view(cam, view_fwd);
        M4 vp = m4_mul(proj, view);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* sky: fullscreen triangle, no depth */
        glDisable(GL_DEPTH_TEST);
        glUseProgram(sky.id);
        common_uniforms(sky.id, &scene, cam, time);
        set3f(sky.id, "uRight", right);
        set3f(sky.id, "uUp", up);
        set3f(sky.id, "uFwd", view_fwd);
        set1f(sky.id, "uTanHalfFov", tanf(scene.fov * 0.5f));
        set1f(sky.id, "uAspect", aspect);
        glBindVertexArray(empty_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        /* terrain */
        glEnable(GL_DEPTH_TEST);
        glUseProgram(sand.id);
        common_uniforms(sand.id, &scene, cam, time);
        set3f(sand.id, "uPlayerPos", player.pos);
        glUniformMatrix4fv(glGetUniformLocation(sand.id, "uViewProj"), 1, GL_FALSE, vp.m);
        terrain_draw(&terrain);

        /* player */
        glUseProgram(body.id);
        common_uniforms(body.id, &scene, cam, time);
        glUniformMatrix4fv(glGetUniformLocation(body.id, "uViewProj"), 1, GL_FALSE, vp.m);
        M4 model = player_model(&player);
        glUniformMatrix4fv(glGetUniformLocation(body.id, "uModel"), 1, GL_FALSE, model.m);
        player_draw(&player);

        /* wind-blown sand follows the player, so gusts are always around them */
        particles_update(&particles, dt, player.pos, time);
        glUseProgram(part.id);
        common_uniforms(part.id, &scene, cam, time);
        glUniformMatrix4fv(glGetUniformLocation(part.id, "uViewProj"), 1, GL_FALSE, vp.m);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        particles_draw(&particles, cam, time);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        frame++;
        if (shot && frame == 4) {
            save_screenshot(shot, plat.width, plat.height);
            break;
        }
        platform_swap(&plat);
    }
    return 0;
}
