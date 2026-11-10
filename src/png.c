#include "png.h"
#include "inflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned crc_table[256];

static void crc_init(void) {
    for (unsigned n = 0; n < 256; n++) {
        unsigned c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static unsigned crc_update(unsigned crc, const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ b[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static void put32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, unsigned len) {
    unsigned char hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    unsigned crc = crc_update(0xFFFFFFFFu, (const unsigned char *)type, 4);
    if (len) crc = crc_update(crc, data, len);
    crc ^= 0xFFFFFFFFu;
    unsigned char c[4];
    put32(c, crc);
    fwrite(c, 1, 4, f);
}

int png_write_rgb(const char *path, int w, int h, const unsigned char *rgb) {
    crc_init();
    size_t stride = (size_t)w * 3;
    size_t raw_len = (stride + 1) * (size_t)h;
    unsigned char *raw = (unsigned char *)malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (stride + 1)] = 0; /* filter: none */
        memcpy(raw + (size_t)y * (stride + 1) + 1, rgb + (size_t)y * stride, stride);
    }

    /* zlib stream with stored (uncompressed) deflate blocks */
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    unsigned char *z = (unsigned char *)malloc(z_len);
    if (!z) { free(raw); return 0; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    unsigned a = 1, b = 0;
    for (size_t pos = 0; pos < raw_len;) {
        size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        z[o++] = (pos + n == raw_len) ? 1 : 0;
        z[o++] = (unsigned char)(n & 0xff); z[o++] = (unsigned char)(n >> 8);
        z[o++] = (unsigned char)(~n & 0xff); z[o++] = (unsigned char)((~n >> 8) & 0xff);
        memcpy(z + o, raw + pos, n);
        for (size_t i = 0; i < n; i++) { a = (a + raw[pos + i]) % 65521; b = (b + a) % 65521; }
        o += n; pos += n;
    }
    put32(z + o, (b << 16) | a); o += 4;

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    put32(ihdr, (unsigned)w); put32(ihdr + 4, (unsigned)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (unsigned)o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return 1;
}

/* ---- reader ------------------------------------------------------------------------------------ */

static unsigned get32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

unsigned char *png_load(const char *path, int *w_out, int *h_out, int *channels_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *file = size > 0 ? (unsigned char *)malloc((size_t)size) : NULL;
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(file); return NULL; }
    fclose(f);

    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char *idat = NULL, *raw = NULL, *out = NULL;
    size_t idat_len = 0, n = (size_t)size;
    int w = 0, h = 0, ch = 0, ok = 0;
    if (n < 8 || memcmp(file, sig, 8)) goto done;
    idat = (unsigned char *)malloc(n); /* all IDAT payloads together are smaller than the file */
    if (!idat) goto done;

    for (size_t pos = 8; pos + 12 <= n;) {
        size_t len = get32(file + pos);
        const unsigned char *type = file + pos + 4, *data = file + pos + 8;
        if (len > n - pos - 12) goto done;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)get32(data); h = (int)get32(data + 4);
            int depth = data[8], ctype = data[9], interlace = data[12];
            ch = ctype == 0 ? 1 : ctype == 4 ? 2 : ctype == 2 ? 3 : ctype == 6 ? 4 : 0;
            if (depth != 8 || !ch || interlace || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
                fprintf(stderr, "[png] %s: only 8-bit non-interlaced grey/RGB(A) is supported\n", path);
                goto done;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(idat + idat_len, data, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += len + 12;
    }
    if (!ch || !idat_len) goto done;

    size_t stride = (size_t)w * ch;
    raw = (unsigned char *)malloc((stride + 1) * (size_t)h);
    out = (unsigned char *)malloc(stride * (size_t)h);
    if (!raw || !out) goto done;
    if (zlib_inflate(idat, idat_len, raw, (stride + 1) * (size_t)h) != (long)((stride + 1) * (size_t)h)) {
        fprintf(stderr, "[png] %s: corrupt image data\n", path);
        goto done;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char *src = raw + (size_t)y * (stride + 1);
        unsigned char *dst = out + (size_t)y * stride;
        const unsigned char *up = y ? dst - stride : NULL;
        int filter = *src++;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)ch ? dst[x - ch] : 0;
            int b = up ? up[x] : 0;
            int c = (up && x >= (size_t)ch) ? up[x - ch] : 0;
            int v = src[x];
            switch (filter) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: v += paeth(a, b, c); break;
            default: break;
            }
            dst[x] = (unsigned char)v;
        }
    }
    ok = 1;
done:
    free(file); free(idat); free(raw);
    if (!ok) { free(out); return NULL; }
    *w_out = w; *h_out = h; *channels_out = ch;
    return out;
}
