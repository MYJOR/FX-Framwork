#include "knock.h"
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define TAIL 1.1f                                  /* seconds kept after the last knock for the hallway to die away */
#define MAX_FRAMES (KNOCK_RATE * 33 / 10)          /* 3.3 s: four knocks half a second apart, the tail, and a little over */
#define WAV_HEADER 44

/* PlaySound reads the memory while it plays: the buffer has to outlive the call. */
static unsigned char wav[WAV_HEADER + MAX_FRAMES * 4];
static float mono[MAX_FRAMES], wet[MAX_FRAMES];

static float rnd(unsigned *s) { /* 0..1 */
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / 16777216.0f;
}

/* One rap of the knuckles on the door, added to out from frame `start`. strength 0..1. */
static void rap(float *out, int frames, int start, float strength, unsigned *seed) {
    /* The door's panel modes: what makes it a thump on heavy wood and not a tick. Frequency (Hz), how
       long each rings (seconds to fall to 1/e), how loud. The low ones carry the weight and last the
       longest; a harder knock brings out more of the high ones. */
    static const float modes[][3] = {{108.0f, 0.034f, 0.42f}, {176.0f, 0.030f, 0.80f}, {262.0f, 0.025f, 1.00f}, {388.0f, 0.019f, 0.92f},
                                     {565.0f, 0.014f, 0.90f}, {830.0f, 0.010f, 0.75f}, {1190.0f, 0.007f, 0.55f}, {1760.0f, 0.005f, 0.36f}};
    const int n_modes = (int)(sizeof modes / sizeof modes[0]);
    float tune = 1.0f + (rnd(seed) - 0.5f) * 0.07f; /* no two knocks land on quite the same spot */
    float f[8], tau[8], amp[8], phase[8];
    for (int m = 0; m < n_modes; m++) {
        f[m] = modes[m][0] * tune * (1.0f + (rnd(seed) - 0.5f) * 0.03f);
        tau[m] = modes[m][1] * (0.9f + 0.2f * rnd(seed));
        float bright = (float)m / (float)(n_modes - 1); /* 0 low .. 1 high */
        amp[m] = modes[m][2] * (1.0f - bright * (1.0f - strength) * 0.8f) * (0.85f + 0.3f * rnd(seed));
        phase[m] = rnd(seed) * 0.6f;
    }
    int len = (int)(0.30f * KNOCK_RATE);
    float lp = 0.0f, hp = 0.0f, prev = 0.0f;
    for (int i = 0; i < len && start + i < frames; i++) {
        float t = (float)i / KNOCK_RATE;
        float attack = t < 0.0012f ? t / 0.0012f : 1.0f; /* skin first, then bone: not an instant edge */
        float s = 0.0f;
        for (int m = 0; m < n_modes; m++) s += amp[m] * expf(-t / tau[m]) * sinf(2.0f * PI * f[m] * t + phase[m]);
        /* the contact itself: a few milliseconds of noise, kept to the band where a knuckle clicks */
        float noise = (rnd(seed) * 2.0f - 1.0f) * expf(-t / 0.0022f);
        lp += (noise - lp) * 0.48f;               /* low-pass near 4.5 kHz */
        hp = 0.90f * (hp + lp - prev); prev = lp; /* high-pass near 750 Hz */
        s = s * attack + hp * (1.3f + 1.2f * strength);
        out[start + i] += s * strength * 0.30f;
    }
}

/* The hallway answering: a short, dark reverberation (four combs into two all-passes). */
static void room(const float *in, float *out, int frames) {
    static const int comb_len[4] = {1557, 1617, 1491, 1422};
    static const int ap_len[2] = {225, 556};
    static float comb[4][1700], ap[2][600];
    float damp[4] = {0};
    int ci[4] = {0}, ai[2] = {0};
    memset(comb, 0, sizeof comb);
    memset(ap, 0, sizeof ap);
    const float feedback = 0.68f, damping = 0.30f; /* about 0.6 s to die away; hard walls, soft ceiling tiles */
    for (int i = 0; i < frames; i++) {
        float x = in[i], s = 0.0f;
        for (int c = 0; c < 4; c++) {
            float y = comb[c][ci[c]];
            damp[c] = y * (1.0f - damping) + damp[c] * damping;
            comb[c][ci[c]] = x + damp[c] * feedback;
            if (++ci[c] >= comb_len[c]) ci[c] = 0;
            s += y;
        }
        s *= 0.25f;
        for (int a = 0; a < 2; a++) {
            float b = ap[a][ai[a]], y = b - 0.5f * s;
            ap[a][ai[a]] = s + 0.5f * y;
            if (++ai[a] >= ap_len[a]) ai[a] = 0;
            s = y;
        }
        out[i] = s;
    }
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v & 255); p[1] = (unsigned char)(v >> 8 & 255); }
static void put32(unsigned char *p, unsigned v) { put16(p, v & 65535); put16(p + 2, v >> 16); }

/* Renders the sequence into `wav` as it is heard at the given distance (metres), pan (-1 left .. 1
   right) and through that many walls. Returns the size of the file in bytes. */
static int render(const float *at, int count, float distance, float pan, int walls, unsigned seed) {
    int frames = (int)((at[count - 1] + TAIL) * KNOCK_RATE);
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    memset(mono, 0, (size_t)frames * sizeof(float));
    for (int i = 0; i < count; i++) {
        /* the first of a run is the firmest, the last often trails off */
        float strength = 0.72f + 0.28f * rnd(&seed);
        if (i == 0 && count > 1) strength = 0.9f + 0.1f * rnd(&seed);
        if (i == count - 1 && count > 2) strength *= 0.86f;
        rap(mono, frames, (int)(at[i] * KNOCK_RATE), strength, &seed);
    }
    room(mono, wet, frames);

    /* Distance: the knock itself falls away faster than the hallway's answer to it, so from far off
       it is mostly room. It never quite goes: it is meant to be heard from anywhere in the level. */
    float close = 1.0f / (1.0f + distance * distance / 9.0f), away = 1.0f / (1.0f + distance / 5.0f);
    float dry_gain = 0.25f + 0.75f * close, wet_gain = 0.30f * (0.35f + 0.65f * away);
    /* Through a wall the top goes and what is left arrives from everywhere. */
    float cutoff = walls <= 0 ? 9000.0f : (walls == 1 ? 900.0f : 480.0f);
    float k = 1.0f - expf(-2.0f * PI * cutoff / KNOCK_RATE);
    float muffle = walls <= 0 ? 1.0f : (walls == 1 ? 0.75f : 0.6f);
    pan *= walls <= 0 ? 0.75f : 0.3f;
    float angle = (pan * 0.5f + 0.5f) * PI * 0.5f, gl = cosf(angle) * 1.41421f, gr = sinf(angle) * 1.41421f;
    float lp1 = 0.0f, lp2 = 0.0f;
    unsigned char *o = wav + WAV_HEADER;
    for (int i = 0; i < frames; i++) {
        float s = (mono[i] * dry_gain + wet[i] * wet_gain) * muffle;
        lp1 += (s - lp1) * k; lp2 += (lp1 - lp2) * k;
        s = lp2 * 1.5f;
        s = s / (1.0f + fabsf(s) * 0.5f); /* soft knee: a hard knock close by must not clip */
        if (i > frames - 2000) s *= (float)(frames - i) / 2000.0f;
        float l = s * gl, r = s * gr;
        int li = (int)(l * 32767.0f), ri = (int)(r * 32767.0f);
        li = li < -32767 ? -32767 : (li > 32767 ? 32767 : li);
        ri = ri < -32767 ? -32767 : (ri > 32767 ? 32767 : ri);
        put16(o + i * 4, (unsigned)li & 65535);
        put16(o + i * 4 + 2, (unsigned)ri & 65535);
    }
    unsigned bytes = (unsigned)frames * 4;
    memcpy(wav, "RIFF", 4); put32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8);
    put32(wav + 16, 16); put16(wav + 20, 1); put16(wav + 22, 2);
    put32(wav + 24, KNOCK_RATE); put32(wav + 28, KNOCK_RATE * 4); put16(wav + 32, 4); put16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); put32(wav + 40, bytes);
    return WAV_HEADER + (int)bytes;
}

/* How the knocks of one sequence are spaced: an even, unhurried rap, each gap a little off the last. */
static void schedule(Knock *k) {
    k->count = 1 + (int)(rnd(&k->seed) * 4.0f);
    if (k->count > KNOCK_MAX) k->count = KNOCK_MAX;
    float gap = 0.27f + 0.16f * rnd(&k->seed), t = 0.02f;
    for (int i = 0; i < k->count; i++) {
        k->at[i] = t;
        t += gap * (0.92f + 0.16f * rnd(&k->seed));
    }
}

void knock_init(Knock *k, unsigned seed) {
    memset(k, 0, sizeof *k);
    k->seed = seed ? seed : 1u;
    k->clock = -1.0f;
    k->wait = 5.0f + 6.0f * rnd(&k->seed);
}

int knock_update(Knock *k, float dt, V3 door, V3 ear, V3 right, int walls) {
    int started = 0;
    if (k->clock >= 0.0f) { /* a sequence is playing: wait for it to finish */
        k->clock += dt;
        if (k->clock > k->at[k->count - 1] + TAIL) k->clock = -1.0f;
    } else {
        k->wait -= dt;
        if (k->wait <= 0.0f) {
            /* Mostly a good while apart; now and then it comes straight back, as if it had heard something. */
            k->wait = rnd(&k->seed) < 0.2f ? 2.5f + 2.0f * rnd(&k->seed) : 7.0f + 16.0f * rnd(&k->seed);
            schedule(k);
            k->clock = 0.0f;
            started = k->count;
            if (!k->muted) {
                V3 to = v3_add(door, v3_scale(ear, -1.0f));
                float d = sqrtf(v3_dot(to, to));
                float pan = d > 1e-3f ? v3_dot(to, right) / d : 0.0f;
                render(k->at, k->count, d, pan, walls, k->seed);
                PlaySoundA((LPCSTR)wav, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
            }
        }
    }
    return started;
}

int knock_write_wav(const char *path, int count, unsigned seed) {
    Knock k;
    knock_init(&k, seed);
    schedule(&k);
    if (count >= 1 && count <= KNOCK_MAX) { /* the same spacing, cut or carried on to the count asked for */
        float gap = k.count > 1 ? k.at[1] - k.at[0] : 0.34f;
        for (int i = k.count; i < count; i++) k.at[i] = k.at[i - 1] + gap;
        k.count = count;
    }
    int bytes = render(k.at, k.count, 2.0f, 0.0f, 0, k.seed);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(wav, 1, (size_t)bytes, f) == (size_t)bytes;
    return fclose(f) == 0 && ok;
}
