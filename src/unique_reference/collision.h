/* CCollision: Unique Race has DDNet's (../ddnet_reference/collision.c), and
 * one function of its own. */
#ifndef DDNET_PHYSICS_SRC_UNIQUE_REFERENCE_COLLISION_H
#define DDNET_PHYSICS_SRC_UNIQUE_REFERENCE_COLLISION_H

#include "../ddnet_reference/collision.h"

/* CCollision::GetMapIndicesWithTime(): like GetMapIndices(), the tiles that
 * matter on the way from prev_pos to pos, in order, but each with the
 * fraction of the way at which the tee reached it. Produced one by one, like
 * collision_map_indices_next(). */
typedef struct map_indices_with_time_t {
  const collision_t *col;
  vec2 prev_pos;
  vec2 pos;
  float d;
  int end;
  int i;
  int last_index;
} map_indices_with_time_t;

map_indices_with_time_t collision_map_indices_with_time(const collision_t *col, vec2 prev_pos, vec2 pos);
bool collision_map_indices_with_time_next(map_indices_with_time_t *it, int *index, float *fraction);

#endif
