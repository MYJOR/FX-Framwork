#ifndef OFFICE_SCENE_H
#define OFFICE_SCENE_H
#include "interior.h"

/* The office: a small closed room with a suspended ceiling, lit ceiling panels, desks and the chair
   model. Built from the files in "office assets". The room is fixed; everything in it is an object
   that can be added, removed, hidden, moved and saved. */

#define OFFICE_MAX_COLLIDERS 16
#define OFFICE_MAX_OBJECTS 32
#define OFFICE_OBJECT_BOXES 5
#define OFFICE_NAME_LEN 32

typedef struct { float x0, z0, x1, z1; } OfficeCollider; /* solid footprint on the floor */

typedef enum { OBJ_FURNITURE, OBJ_LIGHT, OBJ_FIXTURE, OBJ_KIND_COUNT } OfficeObjectKind;
/* What can be placed. Every object is an instance of one of these. */
typedef enum { OT_CHAIR, OT_DESK, OT_LIGHT_PANEL, OT_VENT, OT_COUNT } OfficeObjectType;

/* A piece of furniture standing on the floor, or a fixture in the ceiling grid, placed by pos (x, z)
   and yaw. */
typedef struct {
    char name[OFFICE_NAME_LEN];
    OfficeObjectType type;
    OfficeObjectKind kind;
    int visible;                  /* hidden objects are not drawn, cast no light and are not solid */
    int locked;                   /* the editor will not move it */
    V3 pos; float yaw;            /* current placement; pos.y is fixed by the type */
    V3 home; float home_yaw;      /* where it was put: "reset" returns here */
    V3 bmin, bmax;                /* bounds in object space */
    float snap;                   /* natural grid: 0.6 m for ceiling fixtures, finer for furniture */
    /* lights */
    int light;                    /* index into scene.lights, or -1: assigned by office_update */
    V3 light_half;                /* half size of the lit area */
    float intensity;              /* multiplies the panel radiance; 1 = as designed */
    float warmth;                 /* -1 cool .. +1 warm */
    /* furniture: boxes in object space that stand in for it in reflections */
    IntProxy boxes[OFFICE_OBJECT_BOXES]; int box_count;
    int solid;                    /* the player cannot walk through it */
} OfficeObject;

/* Geometry of a type, in object space. */
typedef struct { float *v; int n; int material; int emitter; } OfficePart; /* emitter: a light's diffuser */
typedef struct { OfficePart parts[2]; int part_count; OfficeObject tmpl; } OfficeProto;

typedef struct {
    IntScene scene;
    OfficeCollider colliders[OFFICE_MAX_COLLIDERS];
    int collider_count;
    OfficeObject objects[OFFICE_MAX_OBJECTS];
    int object_count;
    OfficeProto protos[OT_COUNT];
    float *room; int room_count;  /* the fixed part: vertices and their draws */
    IntDraw room_draws[8]; int room_draw_count;
    float *world; int world_cap;  /* the vertex buffer as uploaded */
    V3 spawn;        /* feet position */
    float spawn_yaw;
} Office;

/* asset_dir is the "office assets" folder. Returns 0 if a required file is missing or unreadable. */
int office_build(Office *o, const char *asset_dir);

/* Applies the objects: rebuilds the vertex buffer, lights, reflection proxies and colliders. Call
   after changing, adding or removing any OfficeObject, then tell the renderer to rebake. */
void office_update(Office *o);
/* Keeps an object inside the room. */
void office_clamp_object(const Office *o, OfficeObject *obj);
/* Object space -> world for a point of an object. */
V3 office_object_to_world(const OfficeObject *obj, V3 p);

const char *office_type_name(OfficeObjectType type);
int office_light_count(const Office *o, int visible_only);
/* These return the new object's index, or -1 when the scene is full (OFFICE_MAX_OBJECTS objects,
   INT_MAX_LIGHTS lights). pos.y is ignored. Call office_update afterwards. */
int office_add_object(Office *o, OfficeObjectType type, V3 pos, float yaw);
int office_duplicate_object(Office *o, int index);
void office_remove_object(Office *o, int index);
/* Back to the layout the scene was designed with. Updates. */
void office_reset(Office *o);
/* The objects as a small text file. office_load leaves the scene alone and returns 0 if the file is
   missing or malformed; on success it updates. */
int office_save(const Office *o, const char *path);
int office_load(Office *o, const char *path);

#endif
