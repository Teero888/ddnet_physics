/* The part of CCollision that Unique Race adds to DDNet's. */
#include "collision.h"

#include <math.h>

/* CCollision::GetMapIndicesWithTime */
map_indices_with_time_t collision_map_indices_with_time(const collision_t *col, vec2 prev_pos, vec2 pos) {
  map_indices_with_time_t it;
  it.col = col;
  it.prev_pos = prev_pos;
  it.pos = pos;
  it.d = vdistance(prev_pos, pos);
  it.end = (int)ceilf(it.d); /* std::ceil of a float */
  it.i = 0;
  it.last_index = 0;
  return it;
}

bool collision_map_indices_with_time_next(map_indices_with_time_t *it, int *index, float *fraction) {
  const collision_t *col = it->col;
  if (!it->d) {
    /* Not moving: only the tile the tee is on, once, at the start of the tick. */
    if (it->i != 0)
      return false;
    it->i = 1;
    int nx = clampi((int)it->pos.x / 32, 0, col->width - 1);
    int ny = clampi((int)it->pos.y / 32, 0, col->height - 1);
    *index = ny * col->width + nx;
    *fraction = 0.0f;
    return collision_tile_exists(col, *index);
  }

  while (it->i < it->end) {
    float a = (float)(it->i) / it->d;
    it->i++;
    vec2 tmp = vmix(it->prev_pos, it->pos, a);
    int nx = clampi((int)tmp.x / 32, 0, col->width - 1);
    int ny = clampi((int)tmp.y / 32, 0, col->height - 1);
    int tile = ny * col->width + nx;
    if (collision_tile_exists(col, tile) && it->last_index != tile) {
      it->last_index = tile;
      *index = tile;
      *fraction = a;
      return true;
    }
  }
  return false;
}
