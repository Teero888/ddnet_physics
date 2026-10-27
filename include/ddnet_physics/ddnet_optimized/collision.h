/* The collision of the reference implementation: the tile layers as DDNet
 * keeps them. Include <ddnet_physics/collision.h>, not this. */
#ifndef DDNET_PHYSICS_OPTIMIZED_COLLISION_H
#define DDNET_PHYSICS_OPTIMIZED_COLLISION_H

#include "../types.h"
#include "../vmath.h"

#include <stdbool.h>
#include <stdint.h>

enum {
  DDNET_TILEFLAG_SOLID = 1 << 0, /* solid or unhookable */
  /* not solid, but a hook reacts to it: hookthrough tiles and hook teleporters */
  DDNET_TILEFLAG_HOOK = 1 << 1,
  DDNET_TILEFLAG_WEAPON = 1 << 2, /* weapon teleporter */
  DDNET_TILEFLAG_TELEIN =
      1 << 3, /* normal teleporter, which hooks and weapons only use with sv_old_teleport_* */
  DDNET_TILEFLAG_EXISTS = 1 << 4,       /* does something to a tee passing through (CCollision::TileExists) */
  DDNET_TILEFLAG_NEAR_STOPPER = 1 << 5, /* a stopper or door on this tile or one of its 8 neighbors */
  DDNET_TILEFLAG_NEAR_DEATH = 1 << 6,   /* a death tile on this tile or one of its 8 neighbors */
  DDNET_TILEFLAG_NEAR_PICKUP = 1 << 7,  /* a pickup at most two tiles away */
  /* a freeze, deep freeze, live freeze or death tile in the game, front or switch layer */
  DDNET_TILEFLAG_FREEZE = 1 << 8,
  /* Nothing on this tile and its 8 neighbors but air and walls: no tile that
   * does something to a tee, no stopper, door, death or freeze, and no tune
   * zone on the tile. No pickup is in reach either. A tee on it has no tile
   * to handle (DDNET_TILEFLAG_NEAR_SOLID says whether it can touch a wall). */
  DDNET_TILEFLAG_QUIET = 1 << 9,
  DDNET_TILEFLAG_NOLASER = 1 << 10,    /* stops lasers, in the game or front layer */
  DDNET_TILEFLAG_NEAR_SOLID = 1 << 11, /* something solid on this tile or one of its 8 neighbors */
  /* The tile itself does nothing to a tee on it: it is not one that does
   * something (EXISTS), no freeze, no tune zone, and no stopper, door or death
   * tile is on it or next to it. Its neighbors may do something. */
  DDNET_TILEFLAG_INERT = 1 << 12,
  /* a start or finish line on this tile or one of its 8 neighbors, in the game or front layer */
  DDNET_TILEFLAG_NEAR_RACE = 1 << 13,
};

/* ddnet_collision_t::tile_handlers */
enum {
  DDNET_TILEHANDLER_CHECKPOINT = 1 << 0, /* a time checkpoint or teleporter checkpoint */
  DDNET_TILEHANDLER_RACE = 1 << 1,       /* start, finish, team unlock, solo on or off */
  DDNET_TILEHANDLER_SWITCH = 1 << 4,     /* something in the switch layer */
  DDNET_TILEHANDLER_TELE = 1 << 5,       /* a teleporter for tees */
  /* two different tiles of the groups of freeze and state in the game and the front layer (layer_tiles) */
  DDNET_TILEHANDLER_LAYERS_TWO = 1 << 6,
};

enum { DDNET_TILE_PAD = 32 };

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

/* A weapon, shield, heart or ninja on the map. */
typedef struct ddnet_map_pickup_t {
  ddnet_vec2_t pos;
  int tile;
  int type;    /* DDNET_POWERUP_* */
  int subtype; /* weapon */
  int layer;
  int number; /* switch number, 0 = not switched */
  bool moves; /* on a conveyor: where it is, is known by its entity */
} ddnet_map_pickup_t;

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
/* What a turret or dragger remembers about its line of sight, see
 * collision_sight_blocked(): the tile that was in the way the last time
 * (+ 1, 0 = none). */
typedef struct ddnet_sight_t {
  int blocker;
} ddnet_sight_t;

/* Tiles that are out of sight from a position altogether, which is worked out
 * once: from where, which tile and for which kind of trace (tile * 4 + kind),
 * and what is known. state 0: unused, 1: the tile is hidden, blocker + 2: it
 * could not be shown with this blocking tile. These are facts about the map. */
typedef struct ddnet_sight_memo_entry_t {
  ddnet_vec2_t from;
  int tile;
  int state;
} ddnet_sight_memo_entry_t;

enum { DDNET_SIGHT_MEMO_SIZE = 65536 };

typedef struct ddnet_sight_memo_t {
  uint64_t universe; /* ddnet_world_t::memo_universe the entries are for: they are about one map */
  ddnet_sight_memo_entry_t entries[DDNET_SIGHT_MEMO_SIZE];
} ddnet_sight_memo_t;

enum { DDNET_LINE_FRACTIONS = 128 };

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

  /* Precomputed per tile, so that the tick does not have to look at the layers
   * for the common case that nothing is there.
   *
   * tile_flags: DDNET_TILEFLAG_* of the tile.
   * dist_*: distance in tiles (largest of the x and y difference, at most 255)
   * to the nearest tile that is solid / that a hook or laser can run into /
   * that does something to a tee passing through it. A value of d means that
   * the d - 1 rings of tiles around the tile are free of them. */
  uint16_t *tile_flags;
  /* tile_flags with a border of DDNET_TILE_PAD tiles on every side, which
   * repeats the tiles at the edge of the map (where positions outside of it
   * are looked up), without QUIET and INERT out there. padded_origin is
   * tile 0, 0: tile x, y is padded_origin[y * padded_width + x], for x and y
   * from -DDNET_TILE_PAD to the width or height + DDNET_TILE_PAD - 1. A
   * lookup near the map needs no clamp. */
  uint16_t *padded_flags;
  const uint16_t *padded_origin;
  int padded_width;
  uint8_t *dist_solid;
  uint8_t *dist_hook;       /* solid or DDNET_TILEFLAG_HOOK */
  uint8_t *dist_weapon;     /* solid or DDNET_TILEFLAG_WEAPON */
  uint8_t *dist_projectile; /* solid or any teleporter that could take a projectile */
  uint8_t *dist_laser;      /* solid or DDNET_TILEFLAG_NOLASER */
  uint8_t *dist_nolaser;    /* DDNET_TILEFLAG_NOLASER */
  uint8_t *dist_exists;
  /* For a tile that does something (CCharacter::HandleTiles): which of its
   * checkpoint, race, switch and teleporter checks can find something on this
   * tile, DDNET_TILEHANDLER_*. The others are skipped. */
  uint8_t *tile_handlers;
  /* For every tile: the tile of the game or the front layer that the
   * freeze and state checks of CCharacter::HandleTiles find there, 0 if none
   * (with two different ones, DDNET_TILEHANDLER_LAYERS_TWO). */
  uint8_t *layer_tiles;

  /* Summed-area tables over (width + 1) * (height + 1) entries: how many tiles
   * above and to the left are solid / solid or of interest to a hook / to a
   * projectile. They answer whether a rectangle of tiles is free with four
   * lookups. The counts are kept in 16 bits and wrap around, which a
   * rectangle of up to 255 by 255 tiles does not notice. */
  /* 1.0f / (float)(i + 1), the step of CCollision::MoveBox for a way of i units */
  float move_fraction[256];
  /* i / (float)end, where DDNet samples a line of length end - 1 or a bit more, for end up to
   * DDNET_LINE_FRACTIONS and i from 0 to end: row end begins at end * (end + 1) / 2. A lookup in place of
   * a division on every sample. */
  float *line_fractions;
  /* positions below these (and above 32) have a tee and everything around it on the map */
  float inner_width, inner_height;
  /* the size of the map in units: width * 32, height * 32 */
  float units_width, units_height;

  uint16_t *sat_solid;
  uint16_t *sat_hook;
  uint16_t *sat_projectile;
  uint16_t *sat_tele;             /* a teleporter that could take a projectile */
  int num_projectile_teleporters; /* how many of those tiles there are */
  uint16_t *sat_laser;            /* solid or DDNET_TILEFLAG_NOLASER */
  uint16_t *sat_nolaser;          /* DDNET_TILEFLAG_NOLASER */
  uint16_t *sat_exists;           /* DDNET_TILEFLAG_EXISTS */

  /* The pickups of the map in the order in which they are created: row by
   * row, and on one tile game layer, front layer, switch layer. tile_pickups
   * has the index of the first pickup of every tile, or -1. */
  ddnet_map_pickup_t *pickups;
  int num_pickups;
  int *tile_pickups;
  bool pickups_move; /* some pickup is on a conveyor: the pickups cannot be handled by position */

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
