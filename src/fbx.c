#include "fbx.h"
#include "inflate.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Binary FBX is a tree of node records:
     end offset, property count, property bytes   (u32 each; u64 from version 7500)
     name length (u8), name, properties, child records, null record
   Arrays are properties of type f/d/i/l with a 12-byte header (count, encoding, byte length) and are
   usually zlib compressed. */

typedef struct {
    const unsigned char *d;
    size_t n;
    int wide; /* 64-bit record headers */
} Fbx;

typedef struct {
    char name[64];
    size_t props, kids, end; /* file offsets */
    unsigned nprops;
} Node;

static unsigned rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned long long rd64(const unsigned char *p) { return rd32(p) | ((unsigned long long)rd32(p + 4) << 32); }

/* Reads the record at off. Returns 0 at a null record or the end of the data. */
static int node_at(const Fbx *f, size_t off, Node *nd) {
    size_t hdr = f->wide ? 24 : 12;
    if (off + hdr + 1 > f->n) return 0;
    unsigned long long end = f->wide ? rd64(f->d + off) : rd32(f->d + off);
    unsigned long long np = f->wide ? rd64(f->d + off + 8) : rd32(f->d + off + 4);
    unsigned long long plen = f->wide ? rd64(f->d + off + 16) : rd32(f->d + off + 8);
    if (end == 0) return 0;
    size_t nl = f->d[off + hdr];
    size_t props = off + hdr + 1 + nl;
    if (end > f->n || props + plen > end) return 0;
    size_t c = nl < sizeof nd->name - 1 ? nl : sizeof nd->name - 1;
    memcpy(nd->name, f->d + off + hdr + 1, c);
    nd->name[c] = 0;
    nd->props = props;
    nd->kids = props + (size_t)plen;
    nd->end = (size_t)end;
    nd->nprops = (unsigned)np;
    return 1;
}

/* Finds a child record by name in the range [from, to). */
static int find_child(const Fbx *f, size_t from, size_t to, const char *name, Node *out) {
    for (size_t off = from; off < to;) {
        Node nd;
        if (!node_at(f, off, &nd)) return 0;
        if (!strcmp(nd.name, name)) { *out = nd; return 1; }
        off = nd.end;
    }
    return 0;
}

/* The node's first property as an array of doubles (any numeric array type). */
static double *array_prop(const Fbx *f, const Node *nd, int *count) {
    *count = 0;
    if (!nd->nprops || nd->props + 13 > nd->kids) return NULL;
    const unsigned char *p = f->d + nd->props;
    char type = (char)p[0];
    size_t elem = (type == 'd' || type == 'l') ? 8 : (type == 'f' || type == 'i') ? 4 : 0;
    if (!elem) return NULL;
    unsigned n = rd32(p + 1), enc = rd32(p + 5), clen = rd32(p + 9);
    if (nd->props + 13 + clen > nd->kids || n > (1u << 28)) return NULL;
    size_t bytes = (size_t)n * elem;
    unsigned char *raw = (unsigned char *)malloc(bytes ? bytes : 1);
    double *out = (double *)malloc((n ? n : 1) * sizeof(double));
    if (!raw || !out) { free(raw); free(out); return NULL; }
    if (enc == 1) {
        if (zlib_inflate(p + 13, clen, raw, bytes) != (long)bytes) { free(raw); free(out); return NULL; }
    } else {
        if (clen != bytes) { free(raw); free(out); return NULL; }
        memcpy(raw, p + 13, bytes);
    }
    for (unsigned i = 0; i < n; i++) {
        const unsigned char *e = raw + (size_t)i * elem;
        if (type == 'd') { double v; memcpy(&v, e, 8); out[i] = v; }
        else if (type == 'f') { float v; memcpy(&v, e, 4); out[i] = v; }
        else if (type == 'i') { int v; memcpy(&v, e, 4); out[i] = v; }
        else { long long v; memcpy(&v, e, 8); out[i] = (double)v; }
    }
    free(raw);
    *count = (int)n;
    return out;
}

/* The node's first property as a string. */
static void string_prop(const Fbx *f, const Node *nd, char *out, size_t cap) {
    out[0] = 0;
    if (!nd->nprops || nd->props + 5 > nd->kids || f->d[nd->props] != 'S') return;
    size_t n = rd32(f->d + nd->props + 1);
    if (nd->props + 5 + n > nd->kids) return;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, f->d + nd->props + 5, n);
    out[n] = 0;
}

/* A per-corner attribute layer (normals, UVs): values plus how they map onto the polygons. */
typedef struct {
    double *val; int nval;
    double *idx; int nidx;
    int by_corner; /* 1 = ByPolygonVertex, 0 = by control point */
    int ok;
} Layer;

static Layer read_layer(const Fbx *f, const Node *geo, const char *layer, const char *values, const char *indices) {
    Layer l = {0};
    Node ln, n;
    char s[64];
    if (!find_child(f, geo->kids, geo->end, layer, &ln)) return l;
    if (find_child(f, ln.kids, ln.end, values, &n)) l.val = array_prop(f, &n, &l.nval);
    if (find_child(f, ln.kids, ln.end, "ReferenceInformationType", &n)) {
        string_prop(f, &n, s, sizeof s);
        if (strcmp(s, "Direct") && find_child(f, ln.kids, ln.end, indices, &n)) l.idx = array_prop(f, &n, &l.nidx);
    }
    if (find_child(f, ln.kids, ln.end, "MappingInformationType", &n)) {
        string_prop(f, &n, s, sizeof s);
        l.by_corner = !strcmp(s, "ByPolygonVertex");
        l.ok = l.val && (l.by_corner || !strcmp(s, "ByVertice") || !strcmp(s, "ByVertex"));
    }
    return l;
}

/* Element `comps`-tuple for polygon corner `corner`, which refers to control point `cp`. */
static const double *layer_at(const Layer *l, int corner, int cp, int comps) {
    static const double zero[3] = {0, 0, 0};
    if (!l->ok) return zero;
    int i = l->by_corner ? corner : cp;
    if (l->idx) { if (i < 0 || i >= l->nidx) return zero; i = (int)l->idx[i]; }
    if (i < 0 || (i + 1) * comps > l->nval) return zero;
    return l->val + (size_t)i * comps;
}

int fbx_load_mesh(const char *path, FbxMesh *out) {
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *file = size > 27 ? (unsigned char *)malloc((size_t)size) : NULL;
    if (!file || fread(file, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); free(file); return 0; }
    fclose(fp);

    int ok = 0;
    double *pos = NULL, *poly = NULL;
    Layer nrm = {0}, uv = {0};
    if (memcmp(file, "Kaydara FBX Binary  ", 20)) {
        fprintf(stderr, "[fbx] %s: not a binary FBX file\n", path);
        goto done;
    }
    Fbx f = {file, (size_t)size, rd32(file + 23) >= 7500};
    Node objects, geo, n;
    if (!find_child(&f, 27, f.n, "Objects", &objects) ||
        !find_child(&f, objects.kids, objects.end, "Geometry", &geo)) {
        fprintf(stderr, "[fbx] %s: no mesh geometry\n", path);
        goto done;
    }
    int npos = 0, npoly = 0;
    if (find_child(&f, geo.kids, geo.end, "Vertices", &n)) pos = array_prop(&f, &n, &npos);
    if (find_child(&f, geo.kids, geo.end, "PolygonVertexIndex", &n)) poly = array_prop(&f, &n, &npoly);
    if (!pos || !poly || npos < 9 || npoly < 3) {
        fprintf(stderr, "[fbx] %s: could not read the vertex data\n", path);
        goto done;
    }
    nrm = read_layer(&f, &geo, "LayerElementNormal", "Normals", "NormalsIndex");
    uv = read_layer(&f, &geo, "LayerElementUV", "UV", "UVIndex");

    /* fan-triangulate each polygon; a negative index (~i) closes it */
    int tris = 0;
    for (int i = 0, start = 0; i < npoly; i++)
        if (poly[i] < 0) { if (i - start >= 2) tris += i - start - 1; start = i + 1; }
    out->v = (float *)malloc((size_t)(tris ? tris : 1) * 3 * 8 * sizeof(float));
    if (!out->v) goto done;
    float *w = out->v;
    for (int k = 0; k < 3; k++) { out->min[k] = 1e30f; out->max[k] = -1e30f; }
    for (int i = 0, start = 0; i < npoly; i++) {
        if (poly[i] >= 0) continue;
        for (int j = start + 1; j < i; j++) {
            int corner[3] = {start, j, j + 1};
            float tri[3][8];
            int valid = 1;
            for (int c = 0; c < 3; c++) {
                int cp = (int)poly[corner[c]];
                if (cp < 0) cp = ~cp;
                if ((cp + 1) * 3 > npos) { valid = 0; break; }
                const double *nn = layer_at(&nrm, corner[c], cp, 3), *tt = layer_at(&uv, corner[c], cp, 2);
                for (int k = 0; k < 3; k++) { tri[c][k] = (float)pos[cp * 3 + k]; tri[c][3 + k] = (float)nn[k]; }
                tri[c][6] = (float)tt[0]; tri[c][7] = (float)tt[1];
            }
            if (!valid) continue;
            if (!nrm.ok) { /* no normals in the file: use the face normal */
                float a[3], b[3], fn[3];
                for (int k = 0; k < 3; k++) { a[k] = tri[1][k] - tri[0][k]; b[k] = tri[2][k] - tri[0][k]; }
                fn[0] = a[1] * b[2] - a[2] * b[1]; fn[1] = a[2] * b[0] - a[0] * b[2]; fn[2] = a[0] * b[1] - a[1] * b[0];
                float len = sqrtf(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
                for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) tri[c][3 + k] = len > 0 ? fn[k] / len : 0;
            }
            for (int c = 0; c < 3; c++) {
                for (int k = 0; k < 3; k++) {
                    if (tri[c][k] < out->min[k]) out->min[k] = tri[c][k];
                    if (tri[c][k] > out->max[k]) out->max[k] = tri[c][k];
                }
                memcpy(w, tri[c], sizeof tri[c]);
                w += 8;
            }
        }
        start = i + 1;
    }
    out->vertex_count = (int)((w - out->v) / 8);
    ok = out->vertex_count > 0;
done:
    free(file); free(pos); free(poly);
    free(nrm.val); free(nrm.idx); free(uv.val); free(uv.idx);
    if (!ok) fbx_free(out);
    return ok;
}

void fbx_free(FbxMesh *m) {
    free(m->v);
    memset(m, 0, sizeof *m);
}
