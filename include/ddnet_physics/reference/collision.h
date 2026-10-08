/* The collision of the reference implementation: the tile layers as DDNet
 * keeps them. Include <ddnet_physics/collision.h>, not this. */
#ifndef DDNET_PHYSICS_REFERENCE_COLLISION_H
#define DDNET_PHYSICS_REFERENCE_COLLISION_H

#include "../types.h"
#include "../vmath.h"

#include <stdbool.h>

typedef struct ddnet_tile_t {
  unsigned char index;
  unsigned char flags;
} ddnet_tile_t;

typedef struct ddnet_tele_tile_t {
  unsigned char number;
  unsigned char type;
} ddnet_tele_tile_t;

typedef struct ddnet_speedup_tile_t {
  unsigned char force;
  unsigned char max_speed;
  unsigned char type;
  short angle;
} ddnet_speedup_tile_t;

typedef struct ddnet_switch_tile_t {
  unsigned char number;
  unsigned char type;
  unsigned char flags;
  unsigned char delay;
} ddnet_switch_tile_t;

typedef struct ddnet_door_tile_t {
  unsigned char index;
  unsigned char flags;
  int number;
} ddnet_door_tile_t;

typedef struct ddnet_tune_tile_t {
  unsigned char number;
  unsigned char type;
} ddnet_tune_tile_t;

/* The positions of all teleporter tiles with one number. */
typedef struct ddnet_tele_list_t {
  ddnet_vec2_t *positions;
  int count;
} ddnet_tele_list_t;

/* The static part of a map: tiles, teleporter targets, doors and map settings.
 *
 * A collision never changes after ddnet_collision_init(), so one collision can
 * be shared by any number of worlds, also across threads.
 *
 * Every layer has width * height tiles, indexed with y * width + x. Layers
 * other than the game layer are NULL if the map does not have them. */
typedef struct ddnet_collision_t {
  int width;
  int height;
  ddnet_tile_t *game;
  ddnet_tile_t *front;
  ddnet_tele_tile_t *tele;
  ddnet_speedup_tile_t *speedup;
  ddnet_switch_tile_t *switch_tiles;
  ddnet_tune_tile_t *tune;
  /* Stoppers of closed doors, filled in from the door entities of the map.
   * Only present if the map has a switch layer. */
  ddnet_door_tile_t *door;

  int highest_switch_number;
  int highest_tune_zone;

  /* Teleporter positions by number - 1. */
  ddnet_tele_list_t tele_ins[DDNET_MAX_TELE_NUMBER];
  ddnet_tele_list_t tele_outs[DDNET_MAX_TELE_NUMBER];
  ddnet_tele_list_t tele_check_outs[DDNET_MAX_TELE_NUMBER];
  ddnet_tele_list_t tele_others[DDNET_MAX_TELE_NUMBER];

  /* Spawn points in map order: [0] default, [1] red, [2] blue. */
  ddnet_vec2_t *spawn_points[3];
  int num_spawn_points[3];

  /* The doors of the map in map order, to draw them (types.h). */
  ddnet_map_door_t *doors;
  int num_doors;

  /* Server settings embedded in the map ("tune_zone 1 gravity 0.25", ...). */
  char **settings;
  int num_settings;
} ddnet_collision_t;

#endif
