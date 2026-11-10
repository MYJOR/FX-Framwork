#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "gl.h"
#include "mathx.h"
#include "shader.h"
#include "png.h"
#include "gamepad.h"
#include "interior.h"
#include "office_scene.h"
#include "office_editor.h"

/* Office: first-person interior scene on the same platform / GL / shader layer as the desert.
 *
 *   WASD move      mouse look       Shift run      Ctrl crouch
 *   G / R  in the editor: move gizmo / rotate gizmo on the selected object
 *   Y    scene editor: frees the cursor and opens the panel (src/office_editor.c). Objects are
 *        moved in the view and the lighting is rebaked as they move. Hold the right mouse button
 *        to look around while editing.
 *   F1 debug view (Shift+F1 back)   F2 ambient obscurance   F3 reflections   F4 depth of field   F torch   L room lights   H horror look   F5 vsync
 *   - / = exposure                  Esc quit (in the editor: deselect, then back to playing)
 *   gamepad: left stick move, right stick look, RT run, B crouch
 */

#define PLAY_FPS 30.0f /* play mode is locked to this */
#define EDIT_FPS 60.0f /* the editor keeps the faster rate for dragging */
#define PI 3.14159265f

#define EYE_STAND 1.64f
#define EYE_CROUCH 1.05f
#define PLAYER_RADIUS 0.27f
#define WALK_SPEED 1.5f
#define RUN_SPEED 3.9f
#define CROUCH_SPEED 1.1f
#define TILT_RATE 3.5f /* 1/s: how quickly the camera tilt follows the movement; lower = lazier */
#define IDLE_SWAY 1.0f   /* idle camera sway: 1 = about a third of a degree, 0 = off */
#define FLASH_POWER 16.0f /* torch brightness */
#define ZOOM_FACTOR 2.2f /* magnification while the right mouse button is held */
#define ZOOM_RATE 10.0f  /* 1/s: how quickly the zoom eases in and out */
#define FOCUS_RATE 4.0f  /* 1/s: how quickly the lens finds focus again after the zoom changed */
#define FOCUS_BLUR 2.4f  /* how far out of focus it gets on the way */
#define MOUSE_SENS 0.0021f /* radians per mouse count */
#define LIVE_BAKE_BUDGET_MS 6.0 /* per frame, for the bounce light while the scene is edited */

typedef struct {
    V3 pos;        /* feet */
    float vx, vz;
    float yaw, pitch;
    float eye;     /* current eye height, eased between standing and crouching */
    float bob_phase, bob_amount;
    float tilt;    /* camera roll, eased */
} FpsPlayer;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Keeps a circle of PLAYER_RADIUS inside the room and outside the furniture footprints. */
static void collide(FpsPlayer *p, const Office *o) {
    const IntScene *s = &o->scene;
    float margin = PLAYER_RADIUS + 0.02f;
    for (int iter = 0; iter < 3; iter++) {
        for (int i = 0; i < o->collider_count; i++) {
            const OfficeCollider *c = &o->colliders[i];
            float qx = clampf(p->pos.x, c->x0, c->x1), qz = clampf(p->pos.z, c->z0, c->z1);
            float dx = p->pos.x - qx, dz = p->pos.z - qz;
            float d2 = dx * dx + dz * dz;
            if (d2 >= PLAYER_RADIUS * PLAYER_RADIUS) continue;
            if (d2 > 1e-8f) { /* outside the box: push straight away from the nearest point */
                float d = sqrtf(d2), push = PLAYER_RADIUS - d;
                p->pos.x += dx / d * push; p->pos.z += dz / d * push;
            } else {          /* centre inside the box: leave through the nearest side */
                float l = p->pos.x - c->x0, r = c->x1 - p->pos.x, n = p->pos.z - c->z0, f = c->z1 - p->pos.z;
                float m = l < r ? l : r; if (n < m) m = n; if (f < m) m = f;
                if (m == l) p->pos.x = c->x0 - PLAYER_RADIUS;
                else if (m == r) p->pos.x = c->x1 + PLAYER_RADIUS;
                else if (m == n) p->pos.z = c->z0 - PLAYER_RADIUS;
                else p->pos.z = c->z1 + PLAYER_RADIUS;
            }
        }
        p->pos.x = clampf(p->pos.x, s->room_min.x + margin, s->room_max.x - margin);
        p->pos.z = clampf(p->pos.z, s->room_min.z + margin, s->room_max.z - margin);
    }
}

static void player_step(FpsPlayer *p, const Office *o, float dt, float move_x, float move_z, int run, int crouch) {
    float len = sqrtf(move_x * move_x + move_z * move_z);
    if (len > 1.0f) { move_x /= len; move_z /= len; }
    float speed = crouch ? CROUCH_SPEED : (run ? RUN_SPEED : WALK_SPEED);
    float k = 1.0f - expf(-(len > 0.01f ? 11.0f : 14.0f) * dt); /* stops a touch quicker than it starts */
    p->vx += (move_x * speed - p->vx) * k;
    p->vz += (move_z * speed - p->vz) * k;
    float px = p->pos.x, pz = p->pos.z;
    p->pos.x += p->vx * dt;
    p->pos.z += p->vz * dt;
    collide(p, o);
    if (dt > 0.0f) { /* velocity follows what collision allowed, so sliding along a wall feels right */
        p->vx = (p->pos.x - px) / dt;
        p->vz = (p->pos.z - pz) / dt;
    }
    p->eye += ((crouch ? EYE_CROUCH : EYE_STAND) - p->eye) * (1.0f - expf(-10.0f * dt));

    /* a small head bob that follows the stride and fades out when standing still */
    float v = sqrtf(p->vx * p->vx + p->vz * p->vz);
    p->bob_phase += v * dt * 2.0f * PI / 1.5f; /* one full cycle per 1.5 m: two steps */
    p->bob_amount += (clampf(v / WALK_SPEED, 0.0f, 1.2f) - p->bob_amount) * (1.0f - expf(-8.0f * dt));
}

/* bob: 1 in play mode, 0 in the editor where the camera should hold still */
static V3 player_eye(const FpsPlayer *p, float bob) {
    float dip = sinf(p->bob_phase);
    float side = cosf(p->bob_phase) * 0.006f * p->bob_amount * bob;
    float up = -dip * dip * 0.014f * p->bob_amount * bob; /* smooth dip at each footfall */
    return v3(p->pos.x + cosf(p->yaw) * side, p->pos.y + p->eye + up, p->pos.z + sinf(p->yaw) * side);
}

/* How bright the ceiling tubes are, t seconds after the switch: a few false starts, then they catch
   and finish warming up. */
static float tube_level(float t) {
    static const float keys[][2] = {{0.00f, 0.55f}, {0.05f, 0.04f}, {0.17f, 0.75f}, {0.22f, 0.08f},
                                    {0.36f, 0.90f}, {0.41f, 0.35f}, {0.50f, 0.80f}};
    if (t >= 0.5f) return 1.0f - 0.2f * expf(-(t - 0.5f) * 4.0f);
    float v = keys[0][1];
    for (int i = 0; i < 7; i++) if (t >= keys[i][0]) v = keys[i][1];
    return v;
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

/* Looks for a file relative to the working directory, its parent and grandparent (the exe usually
   runs from a build folder). Writes the directory that worked into out. */
static int find_dir(const char *name, const char *probe_file, char *out, size_t cap) {
    const char *up[] = {"", "../", "../../"};
    for (int i = 0; i < 3; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s%s/%s", up[i], name, probe_file);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); snprintf(out, cap, "%s%s", up[i], name); return 1; }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *shot = NULL;
    int win_w = 1280, win_h = 720, arg_view = INT_VIEW_FINAL, bench = 0, have_pos = 0;
    /* for testing the editor from the command line */
    int arg_edit = 0, arg_select = -1, arg_place = -1, arg_frames = 3, arg_fullbake = 0, headless = 0, pace_test = 0;
    float place_x = 0, place_z = 0, place_yaw = 0;
    int have_drag = 0; float drag[4] = {0}, arg_wheel = 0;
    int axis_drag = -1; float axis_pixels = 0;
    int ring_drag = 0; float ring_deg = 0, ring_from = 0;
    float arg_x = 0, arg_z = 0, arg_yaw = 0, arg_pitch = -0.06f, arg_eye = 0, arg_roll = 0;
    IntLook look = interior_default_look();
    float lens_curve = look.lens_curve;
    float idle_t = 0.0f, idle_amount = 1.0f, arg_sway = -1.0f; /* idle camera sway: its clock, how still the player is */
    int horror = 0; float flick_wait = 3.0f, flick_left = 0.0f, flick_level = 1.0f; /* H: the harsh, dark look */
    int flash_on = 0, lights_on = 1; /* F: hand torch, L: the room's lights */
    float flash_level = 0.0f, light_level = 1.0f, light_timer = 10.0f, sway_t = 0.0f, sway_step = 0.0f; V3 flash_dir = v3(0.0f, 0.0f, -1.0f);
    int arg_zoom = 0, default_scene = 0;
    const char *cmds[16]; int cmd_count = 0; /* editor commands to run, one every other frame */
    float focus = 0.0f; /* the zoom the lens is focused for; trails behind zoom */
    float zoom = 0.0f, base_tan = 0.0f; /* 0..1 eased zoom; tan(half fov) when not zoomed */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--pos") && i + 2 < argc) { arg_x = (float)atof(argv[++i]); arg_z = (float)atof(argv[++i]); have_pos = 1; }
        else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) arg_yaw = (float)atof(argv[++i]) * PI / 180.0f;
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) arg_pitch = (float)atof(argv[++i]) * PI / 180.0f;
        else if (!strcmp(argv[i], "--roll") && i + 1 < argc) arg_roll = (float)atof(argv[++i]) * PI / 180.0f; /* test: fixed tilt */
        else if (!strcmp(argv[i], "--eye") && i + 1 < argc) arg_eye = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--fov") && i + 1 < argc) look.fov = (float)atof(argv[++i]) * PI / 180.0f;
        else if (!strcmp(argv[i], "--view") && i + 1 < argc) arg_view = atoi(argv[++i]) % INT_VIEW_COUNT;
        else if (!strcmp(argv[i], "--edit")) arg_edit = 1;      /* start in the editor */
        else if (!strcmp(argv[i], "--select") && i + 1 < argc) arg_select = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--place") && i + 4 < argc) { /* object index, x, z, yaw in degrees: moved after the first bake */
            arg_place = atoi(argv[++i]); place_x = (float)atof(argv[++i]); place_z = (float)atof(argv[++i]);
            place_yaw = (float)atof(argv[++i]) * PI / 180.0f;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) arg_frames = atoi(argv[++i]); /* frames before --shot */
        else if (!strcmp(argv[i], "--pace-test")) pace_test = 1; /* runs paced frames, prints how even they were */
        else if (!strcmp(argv[i], "--headless")) headless = 1;  /* hidden window, frames go to an offscreen target */
        else if (!strcmp(argv[i], "--drag") && i + 4 < argc) {  /* synthetic mouse drag in window pixels: x0 y0 x1 y1 */
            for (int k = 0; k < 4; k++) drag[k] = (float)atof(argv[++i]);
            have_drag = 1;
        }
        else if (!strcmp(argv[i], "--axis-drag") && i + 2 < argc) { /* x|z pixels: pull that gizmo arrow of the --select object */
            axis_drag = (argv[++i][0] | 32) == 'z'; axis_pixels = (float)atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--ring-drag") && i + 2 < argc) { /* start angle, sweep (degrees): turn the rotate ring of the --select object */
            ring_from = (float)atof(argv[++i]) * PI / 180.0f; ring_deg = (float)atof(argv[++i]) * PI / 180.0f; ring_drag = 1;
        }
        else if (!strcmp(argv[i], "--wheel") && i + 1 < argc) arg_wheel = (float)atof(argv[++i]); /* notches, after the drag */
        else if (!strcmp(argv[i], "--fullbake")) arg_fullbake = 1; /* --place then rebakes from scratch instead of live */
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { win_w = atoi(argv[++i]); win_h = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--msaa") && i + 1 < argc) look.msaa = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--exposure") && i + 1 < argc) look.exposure = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--noao")) look.ao_strength = 0.0f;
        else if (!strcmp(argv[i], "--nodof")) look.dof_strength = 0.0f;
        else if (!strcmp(argv[i], "--nolens")) lens_curve = 0.0f;
        else if (!strcmp(argv[i], "--noadapt")) look.adapt_max = 1.0f;
        else if (!strcmp(argv[i], "--horror")) horror = 1;
        else if (!strcmp(argv[i], "--sway") && i + 1 < argc) arg_sway = (float)atof(argv[++i]); /* test: the idle sway as it is after this many seconds */
        else if (!strcmp(argv[i], "--nobloom")) look.bloom = 0.0f;
        else if (!strcmp(argv[i], "--noair")) look.air = 0.0f;
        else if (!strcmp(argv[i], "--default-scene")) default_scene = 1; /* ignore the saved scene file */
        else if (!strcmp(argv[i], "--cmd") && i + 1 < argc) { if (cmd_count < 16) cmds[cmd_count++] = argv[++i]; else i++; } /* test: an editor command */
        else if (!strcmp(argv[i], "--flash")) flash_on = 1;
        else if (!strcmp(argv[i], "--lights-off")) { lights_on = 0; light_level = 0.0f; }
        else if (!strcmp(argv[i], "--zoom")) arg_zoom = 1; /* test: as if the right mouse button were held */
        else if (!strcmp(argv[i], "--bench")) bench = 1; /* uncapped, prints frame times, exits */
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
    }

    Platform plat;
    platform_set_headless(headless);
    if (!platform_init(&plat, "Office - interior scene", win_w, win_h)) {
        fprintf(stderr, "platform init failed\n");
        return 1;
    }
    int vsync = !(bench || shot);
    float paced_fps = arg_edit ? EDIT_FPS : PLAY_FPS;
    platform_set_target_fps((int)paced_fps);
    platform_pace_init();
    platform_set_vsync(vsync);
    platform_set_event_hook(editor_event, NULL);

    char shader_dir[300], asset_dir[300];
    if (!find_dir("shaders", "interior_common.glsl", shader_dir, sizeof shader_dir)) {
        fprintf(stderr, "cannot find the shaders folder\n");
        return 1;
    }
    if (!find_dir("office assets", "room textures.png", asset_dir, sizeof asset_dir)) {
        fprintf(stderr, "cannot find the \"office assets\" folder\n");
        return 1;
    }
    shader_set_dir(shader_dir);

    static IntRenderer renderer;
    static Office office;
    if (!interior_init(&renderer)) { fprintf(stderr, "shader load failed\n"); return 1; }
    double t_load = platform_time();
    if (!office_build(&office, asset_dir)) return 1;
    /* the scene as it was last saved from the editor, if there is one */
    char scene_path[320];
    snprintf(scene_path, sizeof scene_path, "%s/../office_scene.txt", asset_dir);
    int scene_loaded = !default_scene && office_load(&office, scene_path);
    double load_ms = (platform_time() - t_load) * 1000.0;
    interior_bake(&renderer, &office.scene, &look);
    if (scene_loaded) printf("office: loaded %d objects from %s\n", office.object_count, scene_path);
    printf("office: %d triangles, assets %.0f ms, light bake %.0f ms (%d x %d x %d probes)\n",
           office.scene.vertex_count / 3, load_ms, renderer.bake_ms,
           renderer.probe_n[0], renderer.probe_n[1], renderer.probe_n[2]);

    FpsPlayer player = {0};
    player.pos = office.spawn;
    player.yaw = office.spawn_yaw + arg_yaw;
    player.pitch = arg_pitch;
    player.eye = EYE_STAND;
    if (have_pos) { player.pos.x = arg_x; player.pos.z = arg_z; }
    if (arg_eye > 0.0f) player.eye = arg_eye;

    if (headless) { /* a hidden window's framebuffer is not guaranteed to hold pixels: render offscreen */
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, plat.width, plat.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &renderer.out_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, renderer.out_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    static EditorState ed;
    if (!editor_init(&ed, &look)) { fprintf(stderr, "editor UI init failed\n"); return 1; }
    ed.view = arg_view;
    ed.vsync = vsync;
    IntLook normal_look = look; /* as set up by the command line; the horror look overrides parts of it */
    ed.active = arg_edit;
    snprintf(ed.scene_path, sizeof ed.scene_path, "%s", scene_path);
    if (arg_select >= 0 && arg_select < office.object_count) ed.selected = arg_select;
    if (!shot && !bench && !ed.active) platform_capture_mouse(1);

    int edit_flags = 0;
    if (arg_place >= 0 && arg_place < office.object_count) {
        OfficeObject *obj = &office.objects[arg_place];
        obj->pos.x = place_x; obj->pos.z = place_z; obj->yaw = place_yaw;
        office_clamp_object(&office, obj);
        edit_flags = arg_fullbake ? (EDIT_SCENE | EDIT_FULL_BAKE) : EDIT_SCENE;
    }

    int key_was[256] = {0};
    double last = platform_time(), reload_timer = 0, title_timer = 0, bench_t0 = 0;
    double frame_acc = 0; int frame_n = 0, frame = 0;
    double pace_sum = 0, pace_min = 1e9, pace_max = 0, pace_dev = 0; int pace_n = 0, pace_bad = 0;

    while (!plat.quit) {
        platform_poll(&plat);
        double now = platform_time();
        float dt = (float)(now - last);
        last = now;
        frame_acc += dt; frame_n++;
        if (pace_test && frame > 30) {
            double ms = dt * 1000.0, off = fabs(ms - 1000.0 / paced_fps);
            pace_sum += ms; pace_n++;
            if (ms < pace_min) pace_min = ms;
            if (ms > pace_max) pace_max = ms;
            if (off > pace_dev) pace_dev = off;
            if (off > 2.0) pace_bad++;
        }
        if (dt > 0.05f) dt = 0.05f;
        /* Paced frames are shown exactly one period apart, so step the world by exactly one period: the few
           hundred microseconds the timer wobbles would otherwise show up as uneven motion at 30 fps. */
        if (!bench && fabsf(dt * paced_fps - 1.0f) < 0.3f) dt = 1.0f / paced_fps;

        static Gamepad pad;
        gamepad_poll(&pad);

        if (axis_drag >= 0 && frame == 4) { /* grab the arrow two thirds along and pull it along its own direction */
            float h[4];
            if (editor_gizmo_handle(axis_drag, h)) {
                float dx = h[2] - h[0], dy = h[3] - h[1], len = sqrtf(dx * dx + dy * dy);
                drag[0] = h[0] + dx * 0.66f; drag[1] = h[1] + dy * 0.66f;
                drag[2] = drag[0] + dx / len * axis_pixels; drag[3] = drag[1] + dy / len * axis_pixels;
                have_drag = 1;
                printf("gizmo arrow %c: (%.0f, %.0f) to (%.0f, %.0f)\n", axis_drag ? 'Z' : 'X', h[0], h[1], h[2], h[3]);
            } else printf("gizmo arrow not on screen\n");
        }
        if (ring_drag && frame == 2) ed.tool = EDIT_TOOL_ROTATE;
        if (ring_drag && frame == 4) {
            float a[2], b[2];
            if (editor_gizmo_ring_point(ring_from, a) && editor_gizmo_ring_point(ring_from + ring_deg, b)) {
                drag[0] = a[0]; drag[1] = a[1]; drag[2] = b[0]; drag[3] = b[1];
                have_drag = 1;
                printf("ring: (%.0f, %.0f) to (%.0f, %.0f)\n", a[0], a[1], b[0], b[1]);
            } else printf("rotate ring not on screen\n");
        }
        if (frame >= 3 && (frame - 3) % 2 == 0 && (frame - 3) / 2 < cmd_count && !editor_command(cmds[(frame - 3) / 2]))
            printf("unknown editor command %s\n", cmds[(frame - 3) / 2]);
        if (have_drag || arg_wheel != 0.0f) { /* scripted input, through the same hook as the real mouse */
            const int t0 = 5, steps = 20;
            PlatEvent e;
            memset(&e, 0, sizeof e);
            e.type = PE_MOUSE_MOVE;
            if (frame == t0) { e.x = drag[0]; e.y = drag[1]; editor_event(NULL, &e); }
            if (have_drag && frame == t0 + 1) { e.type = PE_MOUSE_BUTTON; e.a = 0; e.down = 1; editor_event(NULL, &e); }
            if (have_drag && frame > t0 + 1 && frame <= t0 + 1 + steps) {
                float k = (float)(frame - t0 - 1) / (float)steps;
                e.x = drag[0] + (drag[2] - drag[0]) * k; e.y = drag[1] + (drag[3] - drag[1]) * k;
                editor_event(NULL, &e);
            }
            if (have_drag && frame == t0 + 2 + steps) { e.type = PE_MOUSE_BUTTON; e.a = 0; e.down = 0; editor_event(NULL, &e); }
            if (arg_wheel != 0.0f && frame == t0 + 4 + steps) { e.type = PE_WHEEL; e.y = arg_wheel; editor_event(NULL, &e); }
        }

        int typing = editor_wants_keyboard(&ed);
#define PRESSED(vk) (plat.keys[vk] && !key_was[vk])
        if (PRESSED(VK_ESCAPE)) {
            if (!ed.active) break;
            if (ed.selected >= 0) ed.selected = -1;
            else { ed.active = 0; platform_capture_mouse(1); }
        }
        int ctrl = plat.keys[VK_CONTROL] != 0; /* Ctrl + key is an editor shortcut (Ctrl Y = redo) */
        if (((PRESSED('Y') && !typing && !ctrl) || ed.play) && !shot && !bench) { /* play <-> edit, in place */
            ed.active = !ed.active;
            platform_capture_mouse(!ed.active);
        }
        if (ed.active && !typing) { /* gizmo tools */
            if (PRESSED('Q') && !ctrl) ed.tool = EDIT_TOOL_SELECT;
            if (PRESSED('G') && !ctrl) ed.tool = EDIT_TOOL_MOVE;
            if (PRESSED('R') && !ctrl) ed.tool = EDIT_TOOL_ROTATE;
        }
        if (PRESSED(VK_F1)) ed.view = (ed.view + (plat.keys[VK_SHIFT] ? INT_VIEW_COUNT - 1 : 1)) % INT_VIEW_COUNT;
        if (PRESSED(VK_F2)) ed.ao = !ed.ao;
        if (PRESSED(VK_F3)) ed.reflections = !ed.reflections;
        if (PRESSED(VK_F4)) ed.dof = !ed.dof;
        if (PRESSED('F') && !ed.active && !typing) flash_on = !flash_on;          /* in the editor F frames the selection */
        if (PRESSED('H') && !ed.active && !typing) horror = !horror;               /* in the editor H hides the selection */
        if (PRESSED('L') && !typing && !ctrl) { lights_on = !lights_on; light_timer = 0.0f; }
        if (PRESSED(VK_F5)) { ed.vsync = !ed.vsync; platform_set_vsync(ed.vsync); }
        if (!typing) {
            if (plat.keys[VK_OEM_MINUS]) look.exposure *= expf(-0.8f * dt);
            if (plat.keys[VK_OEM_PLUS]) look.exposure *= expf(0.8f * dt);
        }
        ed.play = 0;
        if (ed.focus >= 0 && ed.focus < office.object_count) { /* the editor asked for the camera to frame an object */
            const OfficeObject *obj = &office.objects[ed.focus];
            V3 c = office_object_to_world(obj, v3((obj->bmin.x + obj->bmax.x) * 0.5f, (obj->bmin.y + obj->bmax.y) * 0.5f,
                                                  (obj->bmin.z + obj->bmax.z) * 0.5f));
            float dx = c.x - player.pos.x, dz = c.z - player.pos.z, dist = sqrtf(dx * dx + dz * dz);
            float want = dist > 3.0f ? 2.4f : (dist < 1.0f ? 1.6f : dist); /* step closer or back to a comfortable distance */
            if (dist > 1e-3f && want != dist) {
                player.pos.x = c.x - dx / dist * want; player.pos.z = c.z - dz / dist * want;
                player.vx = player.vz = 0.0f;
                collide(&player, &office);
                dx = c.x - player.pos.x; dz = c.z - player.pos.z; dist = sqrtf(dx * dx + dz * dz);
            }
            if (dist > 1e-3f) {
                player.yaw = atan2f(dx, -dz);
                player.pitch = clampf(atan2f(c.y - (player.pos.y + player.eye), dist), -1.45f, 1.45f);
            }
        }
        ed.focus = -1;
        /* while playing, a click takes the mouse back after the window lost it */
        if (editor_clicked() && !ed.active && !platform_mouse_captured() && !shot && !bench) platform_capture_mouse(1);
#undef PRESSED
        for (int k = 0; k < 256; k++) key_was[k] = plat.keys[k];
        look.ao_strength = ed.ao ? ed.ao_strength : 0.0f;
        look.reflection = ed.reflections ? 1.0f : 0.0f;
        look.dof_strength = (ed.dof && !ed.active) ? 1.0f : 0.0f; /* the editor stays sharp */
        {   /* The horror look: nothing but the torch, a tight hard beam, wet surfaces, a cold grainy picture. */
            int hz = horror && !ed.active;
            look.saturation = hz ? 0.78f : normal_look.saturation;
            look.contrast = hz ? 1.16f : normal_look.contrast;
            look.vignette = hz ? 0.50f : normal_look.vignette;
            look.adapt_max = normal_look.adapt_max <= 1.0f ? 1.0f : hz ? (flash_on ? 1.35f : 2.5f) : normal_look.adapt_max;
            look.grain = hz ? 0.045f : 0.0f;       look.grain_size = hz ? 1.6f : 1.0f;
            look.grade_cast = hz ? 1.0f : 0.0f;    look.black_crush = hz ? 0.035f : 0.0f;
            look.chroma = hz ? 0.006f : 0.0f;      look.lens_dirt = hz ? 0.16f : 0.0f;
            look.rough_min = hz ? 0.10f : normal_look.rough_min;
            look.rough_scale = hz ? 0.60f : normal_look.rough_scale;
            look.air = normal_look.air * (hz ? 0.3f : 1.0f); /* the beam is far stronger and sits mid-screen: keep the haze thin */
            look.bloom = normal_look.bloom * (hz ? 1.5f : 1.0f);
            renderer.flash_color = hz ? v3(0.86f, 1.0f, 0.93f) : v3(1.0f, 0.96f, 0.88f);
            renderer.flash_cone = hz ? 1.3f : 1.0f;
            renderer.flash_falloff = hz ? 0.07f : 0.0f;
            renderer.flash_harsh = hz ? 1.0f : 0.0f;
            renderer.flash_spec = hz ? 2.5f : 1.0f;
            renderer.flash_bounce = hz ? 0.35f : 1.0f;
            /* a tired battery: now and then the beam sags for a moment */
            if (hz && !shot && !bench) {
                if (flick_left > 0.0f) { flick_left -= dt; flick_level = 0.35f + 0.5f * (float)rand() / (float)RAND_MAX; }
                else { flick_level = 1.0f; flick_wait -= dt; }
                if (flick_wait <= 0.0f) { flick_wait = 4.0f + 9.0f * (float)rand() / (float)RAND_MAX; flick_left = 0.08f + 0.22f * (float)rand() / (float)RAND_MAX; }
            } else flick_level = 1.0f;
        }
        look.lens_curve = ed.active ? 0.0f : lens_curve;                /* ... and straight, so picking lines up */

        /* apply what the editor changed last frame, then let the lighting catch up */
        if (edit_flags & EDIT_VSYNC) platform_set_vsync(ed.vsync);
        if (edit_flags & EDIT_SCENE) office_update(&office);
        if (edit_flags & (EDIT_SCENE | EDIT_BAKE_LOOK)) {
            if (ed.live) interior_invalidate(&renderer, INT_DIRTY_SHADOWS | INT_DIRTY_LIGHTING);
            else ed.stale = 1;
        }
        if (edit_flags & EDIT_FULL_BAKE) { interior_bake(&renderer, &office.scene, &look); ed.stale = 0; }
        if (ed.live && ed.stale) { interior_invalidate(&renderer, INT_DIRTY_SHADOWS | INT_DIRTY_LIGHTING); ed.stale = 0; }
        if (ed.live) interior_bake_step(&renderer, &office.scene, &look, LIVE_BAKE_BUDGET_MS);
        edit_flags = 0;

        /* zoom: hold the right mouse button while playing (in the editor that button looks around) */
        if (base_tan == 0.0f) base_tan = tanf(look.fov * 0.5f);
        int zooming = arg_zoom || (!ed.active && plat.rmb && platform_mouse_captured());
        zoom += ((zooming ? 1.0f : 0.0f) - zoom) * (1.0f - expf(-ZOOM_RATE * dt));
        if (zoom < 0.001f && !zooming) zoom = 0.0f;
        float zoom_tan = base_tan / (1.0f + (ZOOM_FACTOR - 1.0f) * zoom);
        look.fov = 2.0f * atanf(zoom_tan);
        look.lens_curve *= zoom_tan / base_tan; /* a longer lens bends less */
        /* the lens refocuses a moment after the zoom: the picture goes soft, then pulls sharp */
        focus += (zoom - focus) * (1.0f - expf(-FOCUS_RATE * dt));
        look.dof_defocus = ed.active ? 0.0f : clampf(fabsf(zoom - focus) * FOCUS_BLUR - 0.02f, 0.0f, 1.0f);
        float sens = MOUSE_SENS * zoom_tan / base_tan; /* the view turns as far on screen per mouse count as before */

        /* look */
        if (platform_mouse_captured() || (ed.active && plat.rmb)) {
            player.yaw += (float)plat.mouse_dx * sens;
            player.pitch -= (float)plat.mouse_dy * sens;
        }
        player.yaw += pad.rx * 2.4f * dt;
        player.pitch += pad.ry * 1.7f * dt;
        player.pitch = clampf(player.pitch, -1.45f, 1.45f);

        /* move: relative to where the player faces, on the floor plane */
        V3 f = cam_forward(player.yaw, 0.0f);
        V3 right = v3(-f.z, 0, f.x);
        float mx = 0, mz = 0;
        if (!typing && !(ed.active && ctrl)) {
            if (plat.keys['W']) { mx += f.x; mz += f.z; }
            if (plat.keys['S']) { mx -= f.x; mz -= f.z; }
            if (plat.keys['D']) { mx += right.x; mz += right.z; }
            if (plat.keys['A']) { mx -= right.x; mz -= right.z; }
        }
        mx += f.x * pad.ly + right.x * pad.lx;
        mz += f.z * pad.ly + right.z * pad.lx;
        int run = plat.keys[VK_SHIFT] || pad.rt > 0.5f;
        int crouch = (plat.keys[VK_CONTROL] && !ed.active) || (pad.buttons & GP_B); /* Ctrl is for shortcuts in the editor */
        if (!(shot && arg_eye > 0.0f)) player_step(&player, &office, dt, mx, mz, run, crouch);

        reload_timer += dt;
        if (reload_timer > 0.5) {
            reload_timer = 0;
            if (interior_reload_shaders(&renderer)) interior_bake(&renderer, &office.scene, &look);
        }

        /* camera tilt: rocks with each step and leans into a strafe; play mode only */
        float lateral = (player.vx * right.x + player.vz * right.z) / WALK_SPEED;
        float tilt = cosf(player.bob_phase) * 0.010f * player.bob_amount + clampf(lateral, -1.5f, 1.5f) * 0.022f;
        player.tilt += (tilt - player.tilt) * (1.0f - expf(-TILT_RATE * dt));
        renderer.cam_roll = ed.active ? 0.0f : player.tilt + arg_roll;
        V3 eye = player_eye(&player, ed.active ? 0.0f : 1.0f), fwd = cam_forward(player.yaw, player.pitch);
        /* Idle sway: nobody holds their head still. A slow wander of the view, a breath that lifts it, and a
           trace of roll; layered sines with unrelated periods so it never visibly repeats. It fades to a
           third while walking (the step tilt takes over) and shrinks with the zoom, like the view itself. */
        if (!ed.active && ((!shot && !bench) || arg_sway >= 0.0f)) {
            float moving = clampf(sqrtf(player.vx * player.vx + player.vz * player.vz) / WALK_SPEED, 0.0f, 1.0f);
            idle_amount += ((1.0f - 0.65f * moving) - idle_amount) * (1.0f - expf(-dt * 2.5f));
            idle_t = arg_sway >= 0.0f ? arg_sway : idle_t + dt;
            float t = idle_t, a = IDLE_SWAY * idle_amount * (base_tan > 0.0f ? zoom_tan / base_tan : 1.0f);
            float breath = sinf(t * 1.45f); /* about fourteen breaths a minute */
            float yaw = a * (0.0042f * sinf(t * 0.53f) + 0.0021f * sinf(t * 1.17f + 1.3f) + 0.0008f * sinf(t * 2.9f + 0.4f));
            float pitch = a * (0.0030f * sinf(t * 0.71f + 2.1f) + 0.0016f * breath + 0.0007f * sinf(t * 3.3f + 1.9f));
            fwd = cam_forward(player.yaw + yaw, player.pitch + pitch);
            renderer.cam_roll += a * (0.0030f * sinf(t * 0.43f + 0.8f) + 0.0012f * sinf(t * 1.03f));
            eye.y += a * 0.0035f * breath;
        }
        /* Room lights fade like a tube dying, the torch snaps on. The torch is held low on the right and
           trails the view a little, so the beam swings after the head instead of being glued to the screen. */
        light_timer += dt;
        if (lights_on) light_level = tube_level(light_timer);       /* tubes stutter on ... */
        else light_level *= expf(-dt * 16.0f);                      /* ... and die with a short afterglow */
        flash_level += ((flash_on && !ed.active ? 1.0f : 0.0f) - flash_level) * (1.0f - expf(-dt * 30.0f));
        {
            int hz = horror && !ed.active;
            float k = shot ? 1.0f : 1.0f - expf(-dt * (hz ? 5.5f : 12.0f)); /* the horror torch trails further behind */
            flash_dir = v3_norm(v3_add(flash_dir, v3_scale(v3_add(fwd, v3_scale(flash_dir, -1.0f)), k)));
            V3 right = v3_norm(v3_cross(fwd, v3(0.0f, 1.0f, 0.0f)));
            renderer.flash = flash_level < 0.01f ? 0.0f : flash_level * FLASH_POWER * (hz ? 1.7f * flick_level : 1.0f);
            renderer.flash_dir = flash_dir;
            if (!shot && !bench) { /* a hand is never still: a slow drift, and a swing with every step */
                float speed = sqrtf(player.vx * player.vx + player.vz * player.vz), walk = clampf(speed / WALK_SPEED, 0.0f, 1.5f);
                sway_t += dt; sway_step += dt * speed * 4.4f;
                float sx = 0.004f * sinf(sway_t * 1.3f) + 0.003f * sinf(sway_t * 2.9f + 1.0f) + walk * 0.016f * sinf(sway_step);
                float sy = 0.004f * sinf(sway_t * 1.7f + 2.0f) + walk * 0.011f * sinf(sway_step * 2.0f + 0.5f);
                V3 upv = v3_cross(right, flash_dir);
                renderer.flash_dir = v3_norm(v3_add(flash_dir, v3_add(v3_scale(right, sx), v3_scale(upv, sy))));
            }
            renderer.flash_pos = hz ? v3_add(eye, v3_add(v3_scale(right, 0.07f), v3(0.0f, -0.36f, 0.0f))) /* chest height, near the middle */
                                    : v3_add(eye, v3_add(v3_scale(right, 0.16f), v3(0.0f, -0.20f, 0.0f)));
            renderer.frame_dt = (shot || bench) ? 0.0f : dt; /* screenshots get the settled eye */
            renderer.grain_seed = shot ? 0.0f : (float)(frame % 61);
            renderer.scene_light = light_level < 0.002f ? 0.0f : light_level;
        }
        int view_rect[4]; /* the whole window while playing, the space between the panels while editing */
        editor_view_rect(&ed, plat.width, plat.height, view_rect);
        renderer.out_x = view_rect[0];
        renderer.out_y = plat.height - (view_rect[1] + view_rect[3]);
        interior_render(&renderer, &office.scene, &look, eye, fwd, view_rect[2], view_rect[3], ed.view);
        edit_flags = editor_frame(&ed, &office, &look, &renderer, eye, fwd, plat.width, plat.height, dt);
        if (bench && arg_place >= 0 && arg_place < office.object_count) { /* benchmark a drag: the object moves every frame */
            OfficeObject *obj = &office.objects[arg_place];
            obj->pos.x = place_x + 0.3f * sinf((float)frame * 0.05f);
            office_clamp_object(&office, obj);
            edit_flags |= EDIT_SCENE;
        }

        frame++;
        if (shot && frame >= arg_frames) {
            glBindFramebuffer(GL_FRAMEBUFFER, renderer.out_fbo);
            save_screenshot(shot, plat.width, plat.height);
            { float metered = 0.0f, gain = interior_exposure_gain(&renderer, &metered); printf("eye: gain %.2f, metered luminance %.4f\n", gain, metered); }
            if (have_drag || arg_wheel != 0.0f || cmd_count) /* what the scripted input did */
                for (int i = 0; i < office.object_count; i++) {
                    const OfficeObject *obj = &office.objects[i];
                    printf("%s%-14s x %+.2f  z %+.2f  yaw %+.0f%s%s\n", i == ed.selected ? "* " : "  ", obj->name, obj->pos.x, obj->pos.z,
                           obj->yaw * 180.0f / PI, obj->visible ? "" : "  hidden", obj->locked ? "  locked" : "");
                }
            break;
        }
        platform_swap(&plat);
        if (bench) {
            if (frame == 20) { glFinish(); bench_t0 = platform_time(); }
            if (frame == 220) {
                glFinish();
                printf("bench: %.2f ms per frame at %d x %d, msaa %d\n", (platform_time() - bench_t0) * 1000.0 / 200.0,
                       plat.width, plat.height, renderer.samples);
                if (arg_place >= 0) printf("bench: live rebake step %.2f ms\n", renderer.step_ms);
                break;
            }
        } else {
            float want = ed.active ? EDIT_FPS : PLAY_FPS;
            if (want != paced_fps) { paced_fps = want; platform_set_target_fps((int)paced_fps); platform_set_vsync(ed.vsync); }
            platform_pace(paced_fps);
            if (pace_test && frame >= 150) {
                printf("pace: target %.0f fps, mean %.2f ms, min %.2f, max %.2f, worst deviation %.2f ms, %d of %d frames off by over 2 ms\n", paced_fps,
                       pace_sum / pace_n, pace_min, pace_max, pace_dev, pace_bad, pace_n);
                break;
            }
        }

        title_timer += dt;
        if (title_timer > 0.5) {
            char title[160];
            ed.fps = (float)(frame_n / frame_acc);
            snprintf(title, sizeof title, "Office - %.0f fps - view: %s - %s", ed.fps, interior_view_name(ed.view),
                     ed.active ? "editing (Y: play)" : (platform_mouse_captured() ? "Y: scene editor" : "click to look around"));
            platform_set_title(title);
            title_timer = 0; frame_acc = 0; frame_n = 0;
        }
    }
    platform_capture_mouse(0);
    editor_shutdown();
    return 0;
}
