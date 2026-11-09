#include "debug_ui.h"
#include "diag.h"
#include "gl.h"
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PANEL_W 480
#define CHART_N 120
#define PI_F 3.14159265f

static cui_ctx *g_ui;
static cui_gl_renderer *g_gl;
static float g_mouse_x = -1, g_mouse_y = -1;

static const char *k_tabs[] = {"Perf", "Profiler", "Render", "World"};
static const char *k_views[DEBUG_VIEW_COUNT] = {"Final", "Normals", "Trail map", "Slide zones", "Wind gust", "Distance"};

static void *loader(const char *name) { return platform_gl_proc(name); }

int debug_ui_init(DebugState *s) {
    memset(s, 0, sizeof *s);
    s->overlay = 0;
    s->draw_streaks = s->draw_dust = s->draw_player = s->draw_terrain = 1;
    s->vsync = 1;
    g_gl = cui_gl_create(loader);
    if (!g_gl) { fprintf(stderr, "CUI: OpenGL renderer unavailable\n"); return 0; }
    g_ui = cui_create(&(cui_ctx_desc){.renderer = cui_gl_iface(g_gl), .theme = CUI_THEME_DARK});
    if (!g_ui) return 0;
    return 1;
}

void debug_ui_shutdown(void) {
    if (g_ui) cui_destroy(g_ui);
    if (g_gl) cui_gl_destroy(g_gl);
    g_ui = NULL; g_gl = NULL;
}

static cui_key map_key(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (cui_key)(CUI_KEY_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (cui_key)(CUI_KEY_0 + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return (cui_key)(CUI_KEY_F1 + (vk - VK_F1));
    switch (vk) {
    case VK_TAB: return CUI_KEY_TAB;       case VK_RETURN: return CUI_KEY_ENTER;
    case VK_ESCAPE: return CUI_KEY_ESCAPE; case VK_BACK: return CUI_KEY_BACKSPACE;
    case VK_DELETE: return CUI_KEY_DELETE; case VK_SPACE: return CUI_KEY_SPACE;
    case VK_LEFT: return CUI_KEY_LEFT;     case VK_RIGHT: return CUI_KEY_RIGHT;
    case VK_UP: return CUI_KEY_UP;         case VK_DOWN: return CUI_KEY_DOWN;
    case VK_HOME: return CUI_KEY_HOME;     case VK_END: return CUI_KEY_END;
    case VK_PRIOR: return CUI_KEY_PAGE_UP; case VK_NEXT: return CUI_KEY_PAGE_DOWN;
    case VK_OEM_MINUS: return CUI_KEY_MINUS; case VK_OEM_PLUS: return CUI_KEY_EQUAL;
    case VK_OEM_COMMA: return CUI_KEY_COMMA; case VK_OEM_PERIOD: return CUI_KEY_PERIOD;
    case VK_OEM_2: return CUI_KEY_SLASH;
    }
    return CUI_KEY_NONE;
}

void debug_ui_event(void *user, const PlatEvent *e) {
    (void)user;
    if (!g_ui) return;
    switch (e->type) {
    case PE_MOUSE_MOVE: g_mouse_x = e->x; g_mouse_y = e->y; cui_input_mouse_move(g_ui, e->x, e->y); break;
    case PE_MOUSE_BUTTON: cui_input_mouse_button(g_ui, (cui_mouse_button)e->a, e->down != 0); break;
    case PE_WHEEL: cui_input_scroll(g_ui, e->x, e->y); break;
    case PE_MOUSE_LEAVE: g_mouse_x = g_mouse_y = -1; cui_input_mouse_leave(g_ui); break;
    case PE_FOCUS: cui_input_focus(g_ui, e->a != 0); break;
    case PE_CHAR: cui_input_text(g_ui, e->utf8); break;
    case PE_KEY: {
        cui_key k = map_key(e->a);
        if (k != CUI_KEY_NONE) cui_input_key(g_ui, k, e->down != 0, e->mods);
        break;
    }
    }
}

int debug_ui_wants_mouse(const DebugState *s) {
    return s->overlay == 2 && g_mouse_x >= 0 && g_mouse_x < PANEL_W + 20;
}

/* ---- helpers ---- */

static cui_color ms_color(float ms) {
    if (ms < 16.9f) return CUI_RGB(0x5BD68A);
    if (ms < 33.4f) return CUI_RGB(0xF2C14E);
    return CUI_RGB(0xF0605D);
}

static void kv(const char *k, const char *fmt, ...) {
    char buf[96];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    cui_row(g_ui, .width = CUI_FILL) {
        cui_text(g_ui, k, .color = CUI_COLOR_TEXT_SECONDARY, .size = 13, .width = CUI_PX(150));
        cui_text(g_ui, buf, .size = 13, .tabular = true);
    }
}

static const char *bytes_str(size_t b, char *out, size_t n) {
    if (b >= (size_t)1 << 20) snprintf(out, n, "%.1f MB", (double)b / (1 << 20));
    else snprintf(out, n, "%.1f KB", (double)b / 1024.0);
    return out;
}

/* ---- tabs ---- */

static void tab_perf(DebugState *s) {
    (void)s;
    static float fr[CHART_N], cp[CHART_N], gp[CHART_N];
    DiagSummary d = diag_summary();
    int n = diag_history(fr, cp, gp, CHART_N);

    cui_row(g_ui, .width = CUI_FILL, .gap = 14) {
        cui_column(g_ui, .gap = 0) {
            cui_text(g_ui, "FPS", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_textf(g_ui, "%.0f", d.fps);
        }
        cui_column(g_ui, .gap = 0) {
            cui_text(g_ui, "Frame", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_text(g_ui, "", .size = 1);
            char b[24]; snprintf(b, sizeof b, "%.2f ms", d.avg_ms);
            cui_text(g_ui, b, .color = ms_color(d.avg_ms), .tabular = true);
        }
        cui_column(g_ui, .gap = 0) {
            cui_text(g_ui, "CPU", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_textf(g_ui, "%.2f ms", d.cpu_ms);
        }
        cui_column(g_ui, .gap = 0) {
            cui_text(g_ui, "GPU", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_textf(g_ui, "%.2f ms", d.gpu_ms);
        }
    }
    cui_textf(g_ui, "min %.2f   max %.2f   p99 %.2f ms", d.min_ms, d.max_ms, d.p99_ms);

    if (n > 4) {
        float top = d.p99_ms * 1.25f; if (top < 8.0f) top = 8.0f;
        cui_series ser[3] = {{"Frame", fr, n, {0}}, {"CPU", cp, n, {0}}, {"GPU", gp, n, {0}}};
        cui_line_chart(g_ui, "frametimes", ser, 3, .height = 130, .format = "%.1f", .no_grid = false, .min = 0, .max = top);
    }

    cui_label(g_ui, "GPU time by pass");
    float budget = 16.7f;
    for (int z = 0; z < GPU_ZONE_COUNT; ++z) {
        float ms = diag_gpu_ms((DiagGpuZone)z);
        char lab[64]; snprintf(lab, sizeof lab, "%s  %.2f ms", diag_gpu_name((DiagGpuZone)z), ms);
        cui_progress(g_ui, ms / budget > 1 ? 1 : ms / budget, .label = lab, .height = 6,
                     .tone = ms > 8 ? CUI_TONE_DANGER : (ms > 4 ? CUI_TONE_WARNING : CUI_TONE_SUCCESS));
    }

    const DiagCounters *c = diag_counters();
    DiagMemory m = diag_memory();
    char b1[32], b2[32], b3[32];
    cui_separator(g_ui);
    cui_label(g_ui, "Per frame");
    kv("Draw calls", "%d", c->draw_calls);
    kv("Triangles", "%lld", c->triangles);
    kv("Streak lines", "%d  (kick %d)", c->particles_ambient + c->particles_kick, c->particles_kick);
    kv("Dust sprites", "%d", c->particles_dust);
    kv("Trail upload", "%s", bytes_str((size_t)c->trail_upload_bytes, b1, sizeof b1));
    kv("UI", "%d verts, %d cmds", c->ui_vertices, c->ui_commands);
    kv("Memory (working)", "%s", bytes_str(m.working_set, b2, sizeof b2));
    kv("Memory (private)", "%s", bytes_str(m.private_bytes, b3, sizeof b3));
}

static void tab_profiler(DebugState *s) {
    (void)s;
    static cui_flame_span fs[DIAG_MAX_SPANS];
    static cui_flame_view view;
    static int sel = -1;
    int paused = diag_paused();

    cui_row(g_ui, .gap = 10) {
        bool p = paused != 0;
        if (cui_switch(g_ui, "Pause capture (F2)", &p)) diag_set_paused(p);
        if (paused && cui_button(g_ui, "Fit")) view = (cui_flame_view){0};
    }
    int n; double t0, t1;
    const DiagSpan *sp = diag_spans(&n, &t0, &t1);
    for (int i = 0; i < n; ++i) {
        fs[i] = (cui_flame_span){sp[i].name, sp[i].t0, sp[i].t1 > sp[i].t0 ? sp[i].t1 : sp[i].t0, sp[i].depth, 0};
    }
    if (!paused) { view = (cui_flame_view){0}; sel = -1; }
    cui_flame_sort(fs, n);
    if (n > 0) {
        cui_flame_graph(g_ui, "flame", fs, n, &view, &sel, .height = 170, .row_height = 20);
        if (sel >= 0 && sel < n)
            cui_textf(g_ui, "%s: %.3f ms", fs[sel].name, (fs[sel].end - fs[sel].start) * 1000.0);
        else cui_muted(g_ui, paused ? "Click a span for its duration. Wheel zooms." : "Pause to zoom and inspect spans.");
    } else cui_muted(g_ui, "Waiting for a frame...");

    const DiagZoneStat *z; int zn = diag_zone_stats(&z);
    cui_label(g_ui, "CPU zones (ms)");
    const cui_table_column cols[] = {{"Zone", CUI_FILL, 0, false}, {"Last", CUI_PX(70), 0, false},
                                            {"Avg", CUI_PX(70), 0, false}, {"Max", CUI_PX(70), 0, false}};
    cui_table(g_ui, "zones", cols, 4, .striped = true, .compact = true) {
        for (int i = 0; i < zn; ++i) {
            cui_table_row(g_ui, false);
            cui_table_cell_text(g_ui, z[i].name);
            cui_table_cell_textf(g_ui, "%.3f", z[i].last_ms);
            cui_table_cell_textf(g_ui, "%.3f", z[i].avg_ms);
            cui_table_cell_textf(g_ui, "%.3f", z[i].max_ms);
        }
    }
}

static void tab_render(DebugState *s, DebugWorld *w) {
    cui_label(g_ui, "Terrain debug view (F4)");
    cui_select(g_ui, "view", k_views, DEBUG_VIEW_COUNT, &s->view_mode);

    cui_label(g_ui, "Passes");
    bool b;
    b = s->wireframe != 0;      if (cui_switch(g_ui, "Wireframe terrain (F3)", &b)) s->wireframe = b;
    b = s->draw_terrain != 0;   if (cui_switch(g_ui, "Terrain", &b)) s->draw_terrain = b;
    b = s->draw_player != 0;    if (cui_switch(g_ui, "Player", &b)) s->draw_player = b;
    b = s->draw_streaks != 0;   if (cui_switch(g_ui, "Sand streaks", &b)) s->draw_streaks = b;
    b = s->draw_dust != 0;      if (cui_switch(g_ui, "Dust sprites", &b)) s->draw_dust = b;
    b = s->vsync != 0;          if (cui_switch(g_ui, "VSync (F5)", &b)) { s->vsync = b; platform_set_vsync(b); }

    cui_separator(g_ui);
    cui_label(g_ui, "Scene");
    float el = *w->sun_elevation * 180.0f / PI_F, az = *w->sun_azimuth * 180.0f / PI_F, fv = *w->fov * 180.0f / PI_F;
    if (cui_slider(g_ui, "Sun elevation", &el, -8, 86, .show_value = true, .format = "%.1f deg")) *w->sun_elevation = el * PI_F / 180.0f;
    if (cui_slider(g_ui, "Sun azimuth", &az, -180, 180, .show_value = true, .format = "%.0f deg")) *w->sun_azimuth = az * PI_F / 180.0f;
    if (cui_slider(g_ui, "Field of view", &fv, 35, 100, .show_value = true, .format = "%.0f deg")) *w->fov = fv * PI_F / 180.0f;
}

static void tab_world(DebugState *s, DebugWorld *w) {
    (void)s;
    cui_label(g_ui, "Player");
    kv("Position", "%.1f  %.1f  %.1f", w->pos[0], w->pos[1], w->pos[2]);
    kv("Speed", "%.2f m/s", w->speed);
    kv("Ground height", "%.2f m", w->terrain_h);
    kv("Slope", "%.1f deg", w->slope_deg);
    kv("Sliding", "%s", w->sliding ? "yes" : "no");
    kv("Airborne", "%s", w->airborne ? "yes" : "no");
    if (w->mm_clip) {
        kv("Motion match", "%s", w->mm_clip);
        kv("Clip time", "%.2f s", w->mm_time);
    } else {
        kv("Anim idle/walk", "%.2f / %.2f", w->anim_w[0], w->anim_w[1]);
        kv("Anim jog/sprint", "%.2f / %.2f", w->anim_w[2], w->anim_w[3]);
    }
    cui_separator(g_ui);
    cui_label(g_ui, "Camera");
    kv("Mode", "%s", w->free_cam ? "free (F)" : "chase (F)");
    kv("Position", "%.1f  %.1f  %.1f", w->cam[0], w->cam[1], w->cam[2]);
    cui_separator(g_ui);
    cui_label(g_ui, "Environment");
    kv("Wind gust here", "%.2f", w->gust);
    cui_progress(g_ui, w->gust, .height = 6, .tone = CUI_TONE_WARNING);
    kv("Time", "%.1f s", w->time);
    kv("Frame", "%llu", diag_frame_index());
}

/* ---- frame ---- */

void debug_ui_frame(DebugState *s, DebugWorld *w, int width, int height, float dt) {
    if (!g_ui || s->overlay == 0) return;

    diag_zone_begin("UI");
    cui_set_viewport(g_ui, (float)width, (float)height, 1.0f);
    cui_begin_frame(g_ui, dt);

    cui_row(g_ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 10, .align = CUI_ALIGN_START) {
        if (s->overlay == 1) {
            DiagSummary d = diag_summary();
            cui_card(g_ui, .pad = 10, .gap = 2) {
                char b[48]; snprintf(b, sizeof b, "%.0f FPS   %.2f ms", d.fps, d.avg_ms);
                cui_text(g_ui, b, .color = ms_color(d.avg_ms), .tabular = true, .size = 15);
                cui_textf(g_ui, "CPU %.2f   GPU %.2f ms", d.cpu_ms, d.gpu_ms);
                const DiagCounters *c = diag_counters();
                cui_textf(g_ui, "%d draws  %lld tris", c->draw_calls, c->triangles);
                cui_muted(g_ui, "F1 panel   F2 pause   F3 wire   F4 view");
            }
        } else {
            cui_column(g_ui, .width = CUI_PX(PANEL_W), .height = CUI_FILL, .pad = 12, .gap = 10,
                       .bg = CUI_RGBA(0x0E1014EC), .radius = 12, .border = 1) {
                cui_row(g_ui, .width = CUI_FILL) {
                    cui_heading(g_ui, "Diagnostics", 3);
                    cui_spacer(g_ui);
                    cui_muted(g_ui, "F1 close");
                }
                cui_tabs(g_ui, "tabs", k_tabs, 4, &s->tab);
                cui_scroll(g_ui, .id = "panel_scroll", .height = CUI_FILL, .gap = 8) {
                    switch (s->tab) {
                    case 0: tab_perf(s); break;
                    case 1: tab_profiler(s); break;
                    case 2: tab_render(s, w); break;
                    default: tab_world(s, w); break;
                    }
                }
            }
        }
    }
    cui_end_frame(g_ui);

    const cui_draw_data *dd = cui_get_draw_data(g_ui);
    diag_counter_ui(dd->vertex_count, dd->command_count);
    diag_zone_end();

    diag_gpu_begin(GPU_UI);
    cui_gl_render(g_gl, dd, false);
    diag_gpu_end(GPU_UI);

    /* put back what the scene pass expects */
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glViewport(0, 0, width, height);
}
