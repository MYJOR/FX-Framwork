#include "diag.h"
#include "gl.h"
#include "platform.h"
#include <windows.h>
#include <psapi.h>
#include <string.h>
#include <stdlib.h>

#define GPU_LAT 4 /* query ring depth, hides readback latency */

static double g_frame_start, g_cpu_end;
static int g_started;
static unsigned long long g_index;
static int g_paused;

static float h_frame[DIAG_HIST], h_cpu[DIAG_HIST], h_gpu[DIAG_HIST];
static int h_head, h_count;

static DiagSpan g_build[DIAG_MAX_SPANS], g_show[DIAG_MAX_SPANS];
static int g_build_n, g_show_n;
static int g_stack[32], g_sp;
static double g_show_t0, g_show_t1;

static DiagZoneStat g_zone[DIAG_MAX_ZONES];
static int g_zone_n;

static GLuint g_q[GPU_ZONE_COUNT][GPU_LAT];
static int g_q_pending[GPU_ZONE_COUNT][GPU_LAT];
static int g_q_slot[GPU_ZONE_COUNT];
static int g_q_active[GPU_ZONE_COUNT];
static float g_gpu_zone_ms[GPU_ZONE_COUNT];
static int g_gpu_supported;
static const char *g_gpu_names[GPU_ZONE_COUNT] = {"Sky", "Terrain", "Player", "Particles", "UI"};

static DiagCounters g_cnt_cur, g_cnt_prev;

void diag_init(void) {
    g_gpu_supported = (glGenQueries != NULL && glGetQueryObjectui64v != NULL);
    if (g_gpu_supported)
        for (int z = 0; z < GPU_ZONE_COUNT; ++z) glGenQueries(GPU_LAT, g_q[z]);
}

static float percentile99(const float *v, int n) {
    if (n <= 0) return 0;
    float tmp[DIAG_HIST];
    memcpy(tmp, v, (size_t)n * sizeof(float));
    for (int i = 1; i < n; ++i) { /* insertion sort, n <= 240 */
        float x = tmp[i]; int j = i - 1;
        while (j >= 0 && tmp[j] > x) { tmp[j + 1] = tmp[j]; --j; }
        tmp[j + 1] = x;
    }
    int idx = (int)(0.99f * (float)(n - 1) + 0.5f);
    return tmp[idx];
}

static void fold_zones(void) {
    /* Aggregate depth-1 and deeper spans by name into rolling stats. */
    float sum[DIAG_MAX_ZONES] = {0};
    const char *names[DIAG_MAX_ZONES]; int n = 0;
    for (int i = 0; i < g_build_n; ++i) {
        int k = -1;
        for (int j = 0; j < n; ++j) if (names[j] == g_build[i].name || strcmp(names[j], g_build[i].name) == 0) { k = j; break; }
        if (k < 0) { if (n >= DIAG_MAX_ZONES) continue; k = n++; names[k] = g_build[i].name; sum[k] = 0; }
        sum[k] += (float)((g_build[i].t1 - g_build[i].t0) * 1000.0);
    }
    for (int j = 0; j < n; ++j) {
        int k = -1;
        for (int z = 0; z < g_zone_n; ++z) if (strcmp(g_zone[z].name, names[j]) == 0) { k = z; break; }
        if (k < 0) {
            if (g_zone_n >= DIAG_MAX_ZONES) continue;
            k = g_zone_n++;
            g_zone[k].name = names[j]; g_zone[k].avg_ms = sum[j]; g_zone[k].max_ms = sum[j];
        }
        g_zone[k].last_ms = sum[j];
        g_zone[k].avg_ms += (sum[j] - g_zone[k].avg_ms) * 0.05f;
        g_zone[k].max_ms = sum[j] > g_zone[k].max_ms * 0.995f ? sum[j] : g_zone[k].max_ms * 0.995f;
    }
}

static void poll_gpu(void) {
    if (!g_gpu_supported) return;
    for (int z = 0; z < GPU_ZONE_COUNT; ++z) {
        for (int s = 0; s < GPU_LAT; ++s) {
            if (!g_q_pending[z][s]) continue;
            GLint ready = 0;
            glGetQueryObjectiv(g_q[z][s], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (!ready) continue;
            unsigned long long ns = 0;
            glGetQueryObjectui64v(g_q[z][s], GL_QUERY_RESULT, &ns);
            g_q_pending[z][s] = 0;
            float ms = (float)ns * 1e-6f;
            g_gpu_zone_ms[z] += (ms - g_gpu_zone_ms[z]) * 0.25f;
        }
    }
}

void diag_frame_begin(void) {
    double now = platform_time();
    if (g_started) {
        float wall = (float)((now - g_frame_start) * 1000.0);
        float cpu = (float)((g_cpu_end - g_frame_start) * 1000.0);
        poll_gpu();
        float gpu = 0;
        for (int z = 0; z < GPU_ZONE_COUNT; ++z) gpu += g_gpu_zone_ms[z];
        h_frame[h_head] = wall; h_cpu[h_head] = cpu; h_gpu[h_head] = gpu;
        h_head = (h_head + 1) % DIAG_HIST;
        if (h_count < DIAG_HIST) ++h_count;
        fold_zones();
        if (!g_paused) {
            memcpy(g_show, g_build, (size_t)g_build_n * sizeof(DiagSpan));
            g_show_n = g_build_n;
            g_show_t0 = g_frame_start; g_show_t1 = now;
        }
        g_cnt_prev = g_cnt_cur;
    }
    g_cnt_cur.draw_calls = 0; g_cnt_cur.triangles = 0;
    g_build_n = 0; g_sp = 0;
    g_frame_start = g_cpu_end = now;
    g_started = 1;
    ++g_index;
}

void diag_cpu_done(void) { g_cpu_end = platform_time(); }
void diag_set_paused(int p) { g_paused = p; }
int diag_paused(void) { return g_paused; }

void diag_zone_begin(const char *name) {
    if (g_build_n >= DIAG_MAX_SPANS || g_sp >= 32) { g_stack[g_sp < 32 ? g_sp++ : 31] = -1; return; }
    DiagSpan *s = &g_build[g_build_n];
    s->name = name; s->t0 = platform_time(); s->t1 = s->t0; s->depth = g_sp;
    g_stack[g_sp++] = g_build_n++;
}

void diag_zone_end(void) {
    if (g_sp <= 0) return;
    int i = g_stack[--g_sp];
    if (i >= 0) g_build[i].t1 = platform_time();
}

void diag_gpu_begin(DiagGpuZone z) {
    if (!g_gpu_supported || g_q_active[z]) return;
    int s = g_q_slot[z];
    if (g_q_pending[z][s]) return; /* readback hasn't caught up; skip this sample */
    glBeginQuery(GL_TIME_ELAPSED, g_q[z][s]);
    g_q_active[z] = 1;
}

void diag_gpu_end(DiagGpuZone z) {
    if (!g_gpu_supported || !g_q_active[z]) return;
    glEndQuery(GL_TIME_ELAPSED);
    g_q_active[z] = 0;
    g_q_pending[z][g_q_slot[z]] = 1;
    g_q_slot[z] = (g_q_slot[z] + 1) % GPU_LAT;
}

const char *diag_gpu_name(DiagGpuZone z) { return g_gpu_names[z]; }
float diag_gpu_ms(DiagGpuZone z) { return g_gpu_zone_ms[z]; }

void diag_count_draw(long long tris) { ++g_cnt_cur.draw_calls; g_cnt_cur.triangles += tris; }
void diag_counter_set(DiagCounters *c) {
    g_cnt_cur.particles_ambient = c->particles_ambient;
    g_cnt_cur.particles_kick = c->particles_kick;
    g_cnt_cur.particles_dust = c->particles_dust;
    g_cnt_cur.trail_upload_bytes = c->trail_upload_bytes;
    g_cnt_cur.ui_vertices = g_cnt_cur.ui_commands = 0; /* filled by diag_counter_ui */
}
void diag_counter_ui(int v, int c) { g_cnt_cur.ui_vertices = v; g_cnt_cur.ui_commands = c; }
const DiagCounters *diag_counters(void) { return &g_cnt_prev; }

int diag_history(float *f, float *c, float *g, int n) {
    if (n > h_count) n = h_count;
    for (int i = 0; i < n; ++i) {
        int idx = (h_head - n + i + DIAG_HIST * 2) % DIAG_HIST;
        if (f) f[i] = h_frame[idx];
        if (c) c[i] = h_cpu[idx];
        if (g) g[i] = h_gpu[idx];
    }
    return n;
}

DiagSummary diag_summary(void) {
    DiagSummary s = {0};
    if (!h_count) return s;
    float sum = 0, mn = 1e9f, mx = 0;
    for (int i = 0; i < h_count; ++i) {
        float v = h_frame[i];
        sum += v; if (v < mn) mn = v; if (v > mx) mx = v;
    }
    s.avg_ms = sum / (float)h_count; s.min_ms = mn; s.max_ms = mx;
    s.fps = s.avg_ms > 0 ? 1000.0f / s.avg_ms : 0;
    s.p99_ms = percentile99(h_frame, h_count);
    int last = (h_head - 1 + DIAG_HIST) % DIAG_HIST;
    s.cpu_ms = h_cpu[last]; s.gpu_ms = h_gpu[last];
    return s;
}

const DiagSpan *diag_spans(int *count, double *t0, double *t1) {
    *count = g_show_n; if (t0) *t0 = g_show_t0; if (t1) *t1 = g_show_t1;
    return g_show;
}

int diag_zone_stats(const DiagZoneStat **out) { *out = g_zone; return g_zone_n; }

DiagMemory diag_memory(void) {
    DiagMemory m = {0};
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc)) {
        m.working_set = pmc.WorkingSetSize; m.private_bytes = pmc.PrivateUsage;
    }
    return m;
}

unsigned long long diag_frame_index(void) { return g_index; }
