#ifndef OFFICE_SCENE_H
#define OFFICE_SCENE_H
#include "interior.h"

/* The office: a small closed room with a suspended ceiling, two lit ceiling panels, a desk and the
   chair model. Built from the files in "office assets". */

#define OFFICE_MAX_COLLIDERS 8
#define OFFICE_MAX_OBJECTS 8
#define OFFICE_OBJECT_BOXES 4

typedef struct { float x0, z0, x1, z1; } OfficeCollider; /* solid footprint on the floor */

typedef enum { OBJ_FURNITURE, OBJ_LIGHT, OBJ_FIXTURE } OfficeObjectKind;

/* Something that can be moved: a piece of furniture standing on the floor, or a fixture in the
   ceiling grid. Its geometry is kept in its own space and placed by pos (x, z) and yaw. */
typedef struct {
    const char *name;
    OfficeObjectKind kind;
    V3 pos; float yaw;            /* current placement; pos.y is fixed */
    V3 home; float home_yaw;      /* where office_build put it */
    V3 bmin, bmax;                /* bounds in object space */
    int first, count;             /* its vertices in the scene's vertex buffer */
    float snap;                   /* natural grid: 0.6 m for ceiling fixtures, finer for furniture */
    /* lights */
    int light;                    /* index into scene.lights, or -1 */
    int emitter_mat;              /* the material of its diffuser */
    V3 light_half;                /* half size of the lit area */
    float intensity;              /* multiplies the panel radiance; 1 = as designed */
    float warmth;                 /* -1 cool .. +1 warm */
    /* furniture: boxes in object space that stand in for it in reflections; the first is also its
       footprint for the player's collision */
    IntProxy boxes[OFFICE_OBJECT_BOXES]; int box_count;
    int solid;
} OfficeObject;

typedef struct {
    IntScene scene;
    OfficeCollider colliders[OFFICE_MAX_COLLIDERS];
    int collider_count;
    OfficeObject objects[OFFICE_MAX_OBJECTS];
    int object_count;
    float *local, *world;         /* all vertices: as built (objects in object space), and as placed */
    V3 spawn;        /* feet position */
    float spawn_yaw;
} Office;

/* asset_dir is the "office assets" folder. Returns 0 if a required file is missing or unreadable. */
int office_build(Office *o, const char *asset_dir);

/* Applies the objects' placement: rewrites their vertices, lights, reflection proxies and
   colliders. Call after changing any OfficeObject, then tell the renderer to rebake. */
void office_update(Office *o);
/* Keeps an object inside the room. */
void office_clamp_object(const Office *o, OfficeObject *obj);
/* Object space <-> world for a point of an object. */
V3 office_object_to_world(const OfficeObject *obj, V3 p);

#endif
