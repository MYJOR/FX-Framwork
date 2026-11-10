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
 *   F1 debug view (Shift+F1 back)   F2 ambient obscurance   F3 reflections   F5 vsync
 *   - / = exposure                  Esc quit (in the editor: deselect, then back to playing)
 *   gamepad: left stick move, right stick look, RT run, B crouch
 */

#define TARGET_FPS 60.0f
#define PI 3.14159265f

#define EYE_STAND 1.64f
#define EYE_CROUCH 1.05f
#define PLAYER_RADIUS 0.27f
#define WALK_SPEED 2.1f
#define RUN_SPEED 3.9f
#define CROUCH_SPEED 1.1f
#define MOUSE_SENS 0.0021f /* radians per mouse count */
#define LIVE_BAKE_BUDGET_MS 6.0 /* per frame, for the bounce light while the scene is edited */

typedef struct {
    V3 pos;        /* feet */
    float vx, vz;
    float yaw, pitch;
    float eye;     /* current eye height, eased between standing and crouching */
    float bob_phase, bob_amount;
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

static V3 player_eye(const FpsPlayer *p) {
    float side = cosf(p->bob_phase) * 0.006f * p->bob_amount;
    float up = -fabsf(sinf(p->bob_phase)) * 0.014f * p->bob_amount;
    return v3(p->pos.x + cosf(p->yaw) * side, p->pos.y + p->eye + up, p->pos.z + sinf(p->yaw) * side);
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
    int arg_edit = 0, arg_select = -1, arg_place = -1, arg_frames = 3, arg_fullbake = 0, headless = 0;
    float place_x = 0, place_z = 0, place_yaw = 0;
    int have_drag = 0; float drag[4] = {0}, arg_wheel = 0;
    int axis_drag = -1; float axis_pixels = 0;
    int ring_drag = 0; float ring_deg = 0, ring_from = 0;
    float arg_x = 0, arg_z = 0, arg_yaw = 0, arg_pitch = -0.06f, arg_eye = 0;
    IntLook look = interior_default_look();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--pos") && i + 2 < argc) { arg_x = (float)atof(argv[++i]); arg_z = (float)atof(argv[++i]); have_pos = 1; }
        else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) arg_yaw = (float)atof(argv[++i]) * PI / 180.0f;
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) arg_pitch = (float)atof(argv[++i]) * PI / 180.0f;
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
    platform_set_target_fps((int)TARGET_FPS);
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
    double load_ms = (platform_time() - t_load) * 1000.0;
    interior_bake(&renderer, &office.scene, &look);
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
    ed.active = arg_edit;
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

    while (!plat.quit) {
        platform_poll(&plat);
        double now = platform_time();
        float dt = (float)(now - last);
        last = now;
        frame_acc += dt; frame_n++;
        if (dt > 0.05f) dt = 0.05f;

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
        if (PRESSED('Y') && !typing && !shot && !bench) { /* play <-> edit, in place */
            ed.active = !ed.active;
            platform_capture_mouse(!ed.active);
        }
        if (ed.active && !typing) { /* gizmo tools */
            if (PRESSED('G')) ed.tool = EDIT_TOOL_MOVE;
            if (PRESSED('R')) ed.tool = EDIT_TOOL_ROTATE;
        }
        if (PRESSED(VK_F1)) ed.view = (ed.view + (plat.keys[VK_SHIFT] ? INT_VIEW_COUNT - 1 : 1)) % INT_VIEW_COUNT;
        if (PRESSED(VK_F2)) ed.ao = !ed.ao;
        if (PRESSED(VK_F3)) ed.reflections = !ed.reflections;
        if (PRESSED(VK_F5)) { ed.vsync = !ed.vsync; platform_set_vsync(ed.vsync); }
        if (!typing) {
            if (plat.keys[VK_OEM_MINUS]) look.exposure *= expf(-0.8f * dt);
            if (plat.keys[VK_OEM_PLUS]) look.exposure *= expf(0.8f * dt);
        }
        /* while playing, a click takes the mouse back after the window lost it */
        if (editor_clicked() && !ed.active && !platform_mouse_captured() && !shot && !bench) platform_capture_mouse(1);
#undef PRESSED
        for (int k = 0; k < 256; k++) key_was[k] = plat.keys[k];
        look.ao_strength = ed.ao ? ed.ao_strength : 0.0f;
        look.reflection = ed.reflections ? 1.0f : 0.0f;

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

        /* look */
        if (platform_mouse_captured() || (ed.active && plat.rmb)) {
            player.yaw += (float)plat.mouse_dx * MOUSE_SENS;
            player.pitch -= (float)plat.mouse_dy * MOUSE_SENS;
        }
        player.yaw += pad.rx * 2.4f * dt;
        player.pitch += pad.ry * 1.7f * dt;
        player.pitch = clampf(player.pitch, -1.45f, 1.45f);

        /* move: relative to where the player faces, on the floor plane */
        V3 f = cam_forward(player.yaw, 0.0f);
        V3 right = v3(-f.z, 0, f.x);
        float mx = 0, mz = 0;
        if (!typing) {
            if (plat.keys['W']) { mx += f.x; mz += f.z; }
            if (plat.keys['S']) { mx -= f.x; mz -= f.z; }
            if (plat.keys['D']) { mx += right.x; mz += right.z; }
            if (plat.keys['A']) { mx -= right.x; mz -= right.z; }
        }
        mx += f.x * pad.ly + right.x * pad.lx;
        mz += f.z * pad.ly + right.z * pad.lx;
        int run = plat.keys[VK_SHIFT] || pad.rt > 0.5f;
        int crouch = plat.keys[VK_CONTROL] || (pad.buttons & GP_B);
        if (!(shot && arg_eye > 0.0f)) player_step(&player, &office, dt, mx, mz, run, crouch);

        reload_timer += dt;
        if (reload_timer > 0.5) {
            reload_timer = 0;
            if (interior_reload_shaders(&renderer)) interior_bake(&renderer, &office.scene, &look);
        }

        V3 eye = player_eye(&player), fwd = cam_forward(player.yaw, player.pitch);
        interior_render(&renderer, &office.scene, &look, eye, fwd, editor_view_width(&ed, plat.width), plat.height, ed.view);
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
            if (have_drag || arg_wheel != 0.0f) /* what the scripted input did */
                for (int i = 0; i < office.object_count; i++) {
                    const OfficeObject *obj = &office.objects[i];
                    printf("%s%-10s x %+.2f  z %+.2f  yaw %+.0f\n", i == ed.selected ? "* " : "  ", obj->name, obj->pos.x, obj->pos.z,
                           obj->yaw * 180.0f / PI);
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
            platform_pace(TARGET_FPS);
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
