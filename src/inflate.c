#include "inflate.h"
#include <string.h>

/* Table-driven deflate decoder. Each Huffman table is a direct lookup on the next FAST_BITS bits;
   longer codes (rare) fall back to a canonical bit-by-bit walk. */

#define FAST_BITS 10
#define MAX_BITS 15

typedef struct {
    unsigned short fast[1 << FAST_BITS]; /* (symbol << 4) | length, 0 = not in the fast table */
    unsigned short count[MAX_BITS + 1];  /* codes per length */
    unsigned short symbol[288];          /* symbols ordered by code */
} Huff;

typedef struct {
    const unsigned char *src;
    size_t len, pos;
    unsigned long long bits; /* bit buffer, LSB first */
    int nbits;
    unsigned char *dst;
    size_t cap, out;
    int err;
} Inf;

static void refill(Inf *s) {
    while (s->nbits <= 56) {
        unsigned long long b = s->pos < s->len ? s->src[s->pos] : 0; /* zero padding past the end; overruns are caught below */
        s->pos++;
        s->bits |= b << s->nbits;
        s->nbits += 8;
    }
}

static unsigned take(Inf *s, int n) {
    if (s->nbits < n) refill(s);
    unsigned v = (unsigned)(s->bits & ((1ull << n) - 1));
    s->bits >>= n;
    s->nbits -= n;
    return v;
}

static unsigned reverse_bits(unsigned v, int n) {
    unsigned r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

static int huff_build(Huff *h, const unsigned char *lengths, int n) {
    unsigned short offs[MAX_BITS + 2], next_code[MAX_BITS + 2];
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l <= MAX_BITS; l++) {
        left = (left << 1) - h->count[l];
        if (left < 0) return 0; /* over-subscribed */
    }
    offs[1] = 0;
    unsigned code = 0;
    for (int l = 1; l <= MAX_BITS; l++) {
        offs[l + 1] = (unsigned short)(offs[l] + h->count[l]);
        next_code[l] = (unsigned short)code;
        code = (code + h->count[l]) << 1;
    }
    for (int i = 0; i < n; i++) {
        int l = lengths[i];
        if (!l) continue;
        h->symbol[offs[l]++] = (unsigned short)i;
        unsigned c = next_code[l]++;
        if (l <= FAST_BITS) {
            unsigned r = reverse_bits(c, l);
            for (unsigned k = r; k < (1u << FAST_BITS); k += 1u << l)
                h->fast[k] = (unsigned short)((i << 4) | l);
        }
    }
    return 1;
}

static int huff_decode(Inf *s, const Huff *h) {
    if (s->nbits < MAX_BITS) refill(s);
    unsigned e = h->fast[s->bits & ((1u << FAST_BITS) - 1)];
    if (e) {
        s->bits >>= (e & 15);
        s->nbits -= (int)(e & 15);
        return (int)(e >> 4);
    }
    /* slow path: canonical decode one bit at a time */
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= MAX_BITS; l++) {
        code |= (int)(s->bits & 1);
        s->bits >>= 1;
        s->nbits--;
        int count = h->count[l];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return 0;
}

static const unsigned short LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned char LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                             1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned char DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void inflate_block(Inf *s, const Huff *lit, const Huff *dist) {
    for (;;) {
        if (s->err || s->pos > s->len + 8) { s->err = 1; return; }
        int sym = huff_decode(s, lit);
        if (sym < 256) {
            if (s->out >= s->cap) { s->err = 1; return; }
            s->dst[s->out++] = (unsigned char)sym;
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) { s->err = 1; return; }
            size_t len = LEN_BASE[sym] + take(s, LEN_EXTRA[sym]);
            int ds = huff_decode(s, dist);
            if (ds >= 30) { s->err = 1; return; }
            size_t d = DIST_BASE[ds] + take(s, DIST_EXTRA[ds]);
            if (d > s->out || s->out + len > s->cap) { s->err = 1; return; }
            unsigned char *o = s->dst + s->out;
            const unsigned char *f = o - d;
            for (size_t i = 0; i < len; i++) o[i] = f[i]; /* overlapping copy is intended */
            s->out += len;
        }
    }
}

long zlib_inflate(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_cap) {
    static Huff lit, dist, clen; /* not re-entrant; loading is single threaded */
    if (src_len < 6 || (src[0] & 15) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 32)) return -1;
    Inf s;
    memset(&s, 0, sizeof s);
    s.src = src + 2; s.len = src_len - 2;
    s.dst = dst; s.cap = dst_cap;

    int last;
    do {
        last = (int)take(&s, 1);
        unsigned type = take(&s, 2);
        if (type == 0) {
            /* stored: drop to a byte boundary, then LEN / ~LEN */
            take(&s, s.nbits & 7);
            unsigned n = take(&s, 16), nn = take(&s, 16);
            if ((n ^ 0xffff) != nn || s.out + n > s.cap) return -1;
            for (unsigned i = 0; i < n; i++) s.dst[s.out++] = (unsigned char)take(&s, 8);
        } else if (type == 1) {
            unsigned char l[288 + 30];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            for (i = 0; i < 30; i++) l[288 + i] = 5;
            huff_build(&lit, l, 288);
            huff_build(&dist, l + 288, 30);
            inflate_block(&s, &lit, &dist);
        } else if (type == 2) {
            static const unsigned char order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            unsigned char l[288 + 32];
            int nlit = (int)take(&s, 5) + 257, ndist = (int)take(&s, 5) + 1, nclen = (int)take(&s, 4) + 4;
            if (nlit > 286 || ndist > 30) return -1;
            memset(l, 0, 19);
            for (int i = 0; i < nclen; i++) l[order[i]] = (unsigned char)take(&s, 3);
            if (!huff_build(&clen, l, 19)) return -1;
            int n = 0;
            while (n < nlit + ndist) {
                int sym = huff_decode(&s, &clen);
                if (s.err) return -1;
                if (sym < 16) { l[n++] = (unsigned char)sym; continue; }
                int rep, val = 0;
                if (sym == 16) { if (!n) return -1; val = l[n - 1]; rep = 3 + (int)take(&s, 2); }
                else if (sym == 17) rep = 3 + (int)take(&s, 3);
                else rep = 11 + (int)take(&s, 7);
                if (n + rep > nlit + ndist) return -1;
                while (rep--) l[n++] = (unsigned char)val;
            }
            if (!huff_build(&lit, l, nlit) || !huff_build(&dist, l + nlit, ndist)) return -1;
            inflate_block(&s, &lit, &dist);
        } else {
            return -1;
        }
        if (s.err || s.pos > s.len + 8) return -1;
    } while (!last);
    return (long)s.out;
}
