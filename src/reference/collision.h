/* CCollision: all queries against the static tiles of the map. */
#ifndef DDNET_PHYSICS_SRC_REFERENCE_COLLISION_H
#define DDNET_PHYSICS_SRC_REFERENCE_COLLISION_H

#include "../common/vmath.h"

#include <ddnet_map_loader.h>
#include <ddnet_physics/collision.h>

typedef ddnet_collision_t collision_t;

/* Asks whether the switch with a number is active for whoever is moving. */
typedef bool (*switch_active_fn)(unsigned char number, void *user);

vec2 clamp_vel(int move_restriction, vec2 vel);

int collision_get_move_restrictions(const collision_t *col, switch_active_fn switch_active, void *user,
                                    vec2 pos, float distance, int override_center_tile_index);

int collision_get_tile(const collision_t *col, int x, int y);
int collision_get_front_tile(const collision_t *col, int x, int y);
int collision_is_solid(const collision_t *col, int x, int y);
bool collision_check_point(const collision_t *col, float x, float y);
int collision_get_collision_at(const collision_t *col, float x, float y);
int collision_get_front_collision_at(const collision_t *col, float x, float y);

int collision_intersect_line(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                             vec2 *out_before_collision);
int collision_intersect_line_tele_hook(const collision_t *col, bool old_teleport_hook, vec2 pos0, vec2 pos1,
                                       vec2 *out_collision, vec2 *out_before_collision, int *tele_nr);
int collision_intersect_line_tele_weapon(const collision_t *col, bool old_teleport_weapons, vec2 pos0,
                                         vec2 pos1, vec2 *out_collision, vec2 *out_before_collision,
                                         int *tele_nr);
int collision_intersect_no_laser(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                                 vec2 *out_before_collision);
int collision_intersect_no_laser_no_walls(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                                          vec2 *out_before_collision);

void collision_move_point(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, float elasticity,
                          int *bounces);
void collision_move_box(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, vec2 size, vec2 elasticity,
                        bool *grounded);
bool collision_test_box(const collision_t *col, vec2 pos, vec2 size);
bool collision_is_on_ground(const collision_t *col, vec2 pos, float size);

/* Tile index of a position (rounded), unconditionally. */
int collision_get_pure_map_index(const collision_t *col, vec2 pos);
/* Tile index of a position (truncated), or -1 if nothing on that tile matters. */
int collision_get_map_index(const collision_t *col, vec2 pos);
bool collision_tile_exists(const collision_t *col, int index);

/* GetMapIndices(): the tiles that matter on the way from prev_pos to pos, in
 * order. DDNet collects them into a vector first, here they are produced one
 * by one, which is equivalent because they only depend on static map data. */
typedef struct map_indices_t {
  const collision_t *col;
  vec2 prev_pos;
  vec2 pos;
  float d;
  int end;
  int i;
  int last_index;
} map_indices_t;

map_indices_t collision_map_indices(const collision_t *col, vec2 prev_pos, vec2 pos);
bool collision_map_indices_next(map_indices_t *it, int *index);

int collision_get_tile_index(const collision_t *col, int index);
int collision_get_front_tile_index(const collision_t *col, int index);
int collision_get_tile_flags(const collision_t *col, int index);
int collision_get_front_tile_flags(const collision_t *col, int index);
/* First tile with a teleporter or speedup on the way from prev_pos to pos, or -1. */
int collision_get_index(const collision_t *col, vec2 prev_pos, vec2 pos);

int collision_is_teleport(const collision_t *col, int index);
int collision_is_evil_teleport(const collision_t *col, int index);
bool collision_is_check_teleport(const collision_t *col, int index);
bool collision_is_check_evil_teleport(const collision_t *col, int index);
int collision_is_teleport_weapon(const collision_t *col, int index);
int collision_is_teleport_hook(const collision_t *col, int index);
int collision_is_tele_checkpoint(const collision_t *col, int index);
bool collision_is_speedup(const collision_t *col, int index);
int collision_is_tune(const collision_t *col, int index);
void collision_get_speedup(const collision_t *col, int index, vec2 *dir, int *force, int *max_speed,
                           int *type);
int collision_get_switch_type(const collision_t *col, int index);
int collision_get_switch_number(const collision_t *col, int index);
int collision_get_switch_delay(const collision_t *col, int index);
int collision_is_time_checkpoint(const collision_t *col, int index);
int collision_is_front_time_checkpoint(const collision_t *col, int index);
int collision_mover_speed(const collision_t *col, int x, int y, vec2 *speed);

/* Entity number (tile index - ENTITY_OFFSET) at a tile position of a layer. */
int collision_entity(const collision_t *col, int x, int y, int layer);

#endif
