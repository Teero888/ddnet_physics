/* Collisions: the static part of a map.
 *
 * The functions are the same for both implementations, the structure belongs
 * to the implementation the library was built as, see backend.h. */
#ifndef DDNET_PHYSICS_COLLISION_H
#define DDNET_PHYSICS_COLLISION_H

#include "backend.h"
#include "vmath.h"

#include <stdbool.h>
#include <stddef.h>

struct map_data_t; /* ddnet_map_loader.h */

/* Result bits of ddnet_collision_get_move_restrictions(). */
enum {
  DDNET_CANTMOVE_LEFT = 1 << 0,
  DDNET_CANTMOVE_RIGHT = 1 << 1,
  DDNET_CANTMOVE_UP = 1 << 2,
  DDNET_CANTMOVE_DOWN = 1 << 3,
};

/* Highest teleporter / tune zone number a tile can hold. */
enum { DDNET_MAX_TELE_NUMBER = 255, DDNET_NUM_TUNE_ZONES = 256 };

#if defined(DDNET_PHYSICS_BACKEND_REFERENCE)
#include "reference/collision.h"
#elif defined(DDNET_PHYSICS_BACKEND_OPTIMIZED)
#include "optimized/collision.h"
#endif

/* Copies everything it needs out of the map: the map can be freed afterwards.
 * Returns false if the map has no game layer or memory ran out. */
bool ddnet_collision_init(ddnet_collision_t *collision, const struct map_data_t *map);
void ddnet_collision_free(ddnet_collision_t *collision);

/* The raw collision queries of DDNet's CCollision. Positions are in world
 * units, 32 units per tile. */
int ddnet_collision_get_tile(const ddnet_collision_t *collision, int x, int y);
int ddnet_collision_get_front_tile(const ddnet_collision_t *collision, int x, int y);
bool ddnet_collision_check_point(const ddnet_collision_t *collision, float x, float y);
bool ddnet_collision_test_box(const ddnet_collision_t *collision, ddnet_vec2_t pos, ddnet_vec2_t size);
bool ddnet_collision_is_on_ground(const ddnet_collision_t *collision, ddnet_vec2_t pos, float size);
int ddnet_collision_intersect_line(const ddnet_collision_t *collision, ddnet_vec2_t pos0, ddnet_vec2_t pos1,
                                   ddnet_vec2_t *out_collision, ddnet_vec2_t *out_before_collision);
void ddnet_collision_move_point(const ddnet_collision_t *collision, ddnet_vec2_t *inout_pos,
                                ddnet_vec2_t *inout_vel, float elasticity, int *bounces);
void ddnet_collision_move_box(const ddnet_collision_t *collision, ddnet_vec2_t *inout_pos,
                              ddnet_vec2_t *inout_vel, ddnet_vec2_t size, ddnet_vec2_t elasticity,
                              bool *grounded);
/* Tile index of a position, without any filtering. */
int ddnet_collision_get_pure_map_index(const ddnet_collision_t *collision, float x, float y);

#endif
