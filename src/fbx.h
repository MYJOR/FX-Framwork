#ifndef FBX_H
#define FBX_H

/* Minimal binary FBX reader: pulls the first static mesh out of a file as a plain triangle list.
   No skinning, animation, materials or node transforms; positions are returned exactly as stored in
   the Geometry node, so the caller places and scales the mesh. */
typedef struct {
    float *v;         /* vertex_count * 8 floats: position xyz, normal xyz, uv */
    int vertex_count; /* always a multiple of 3 */
    float min[3], max[3];
} FbxMesh;

int fbx_load_mesh(const char *path, FbxMesh *out);
void fbx_free(FbxMesh *m);

#endif
