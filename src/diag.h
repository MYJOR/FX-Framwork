#ifndef DIAG_H
#define DIAG_H
#include <stddef.h>

/* Performance diagnostics: CPU zones (flame graph), GPU timer queries, frame
 * history and per-frame counters. UI-agnostic; debug_ui.c presents it. */

#define DIAG_HIST 240
#define DIAG_MAX_SPANS 512
#define DIAG_MAX_ZONES 32

typedef struct { const char *name; double t0, t1; int depth; } DiagSpan;

typedef struct {
    const char *name;
    float last_ms, avg_ms, max_ms;
} DiagZoneStat;

typedef enum { GPU_SKY, GPU_TERRAIN, GPU_PLAYER, GPU_PARTICLES, GPU_UI, GPU_ZONE_COUNT } DiagGpuZone;

typedef struct {
    float fps, avg_ms, min_ms, max_ms, p99_ms; /* over the history window */
    float cpu_ms, gpu_ms;                      /* latest frame */
} DiagSummary;

typedef struct {
    int draw_calls;
    long long triangles;
    int particles_ambient, particles_kick, particles_dust;
    int trail_upload_bytes;
    int ui_vertices, ui_commands;
} DiagCounters;

typedef struct { size_t working_set, private_bytes; } DiagMemory;

void diag_init(void);
void diag_frame_begin(void);  /* finalizes the previous frame, starts the next */
void diag_cpu_done(void);     /* call after all CPU work, before the swap/vsync wait */
void diag_set_paused(int paused); /* freezes the displayed flame graph */
int diag_paused(void);

void diag_zone_begin(const char *name);
void diag_zone_end(void);
void diag_gpu_begin(DiagGpuZone z);
void diag_gpu_end(DiagGpuZone z);
const char *diag_gpu_name(DiagGpuZone z);
float diag_gpu_ms(DiagGpuZone z);

/* Counters accumulate during a frame; the previous frame's totals are shown. */
void diag_count_draw(long long triangles);
void diag_counter_set(DiagCounters *c_in); /* overwrite non-accumulated fields (particles, uploads, ui) */
void diag_counter_ui(int vertices, int commands);
const DiagCounters *diag_counters(void);

/* History in chronological order (oldest first); returns count written (<= n). */
int diag_history(float *frame_ms, float *cpu_ms, float *gpu_ms, int n);
DiagSummary diag_summary(void);
const DiagSpan *diag_spans(int *count, double *frame_t0, double *frame_t1);
int diag_zone_stats(const DiagZoneStat **out);
DiagMemory diag_memory(void);
unsigned long long diag_frame_index(void);

#endif
