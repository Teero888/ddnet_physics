/* The structures of the optimized implementation of Unique Race. Include
 * <ddnet_physics/world.h>, not this.
 *
 * Like those of the reference implementation (unique_reference/world.h), with
 * what is the same for every copy of a world shared between them, and with
 * the grenades and pickups in arrays in the order the server ticks them: a
 * copy of a world is two small copies. */
#ifndef DDNET_PHYSICS_UNIQUE_OPTIMIZED_WORLD_H
#define DDNET_PHYSICS_UNIQUE_OPTIMIZED_WORLD_H

#include "../collision.h"
#include "../config.h"
#include "../tuning.h"
#include "../types.h"
#include "../vmath.h"

#include <stdint.h>

/* Unique Race's maxammo of the grenade, the ammo of sv_health_and_ammo. */
enum { DDNET_GRENADE_MAX_AMMO = 10 };

/* The tuning as the floats the physics compute with (the server converts on
 * every read), derived from ddnet_world_t::tuning by
 * ddnet_world_tuning_changed(). */
typedef struct ddnet_tuning_values_t {
#define DDNET_TUNING_PARAM(name, default_value) float name;
#include "../tuning_params.h"
#undef DDNET_TUNING_PARAM
  /* The smallest squares of a speed that are not below velramp_start (/ 50),
   * above 6000, and of a distance above hook_length: comparisons without the
   * root. */
  float velramp_start_squared;
  float speed_clamp_squared;
  float hook_length_squared;
  /* For VelocityRamp(), see velocity_ramp_prepare(). */
  int velramp_powf;
  double velramp_log2_curvature;
} ddnet_tuning_values_t;

typedef struct ddnet_weapon_stat_t {
  int ammo; /* -1 is unlimited */
  bool got;
} ddnet_weapon_stat_t;

/* CCharacterCore: movement, jumping and the hook. */
typedef struct ddnet_character_core_t {
  ddnet_vec2_t pos;
  ddnet_vec2_t vel;
  ddnet_vec2_t hook_pos;
  ddnet_vec2_t hook_dir;
  /* The acceleration the hook gave the tee in the last tick (not Unique Race's: for searches) */
  ddnet_vec2_t hook_accel;
  int hook_tick;
  int hook_state;

  /* see unique_reference/world.h */
  int active_weapon;
  ddnet_weapon_stat_t weapons[DDNET_NUM_WEAPONS];

  int jumped;       /* bit 0: jump is held, bit 1: all air jumps are used up */
  int jumped_total; /* jumps performed in the air */
  int jumps;        /* number of jumps the tee has */
  int direction;
  /* The input the core took on its last tick (a frozen tee leaves out its
   * moves). ddnet_character_angle() works the angle out of its aim. */
  ddnet_input_t input;
  /* the aim of the last tick of the core (0, 0 before the first one) */
  int aim_x, aim_y;

  int triggered_events;

  int colliding;
  bool left_wall;

  int freeze_start;

  int move_restrictions;
} ddnet_character_core_t;

/* CCharacter: the tee, while it is in the world. */
typedef struct ddnet_character_t {
  bool alive;
  ddnet_vec2_t pos; /* position after the last tick, core.pos is the working copy */

  ddnet_character_core_t core;

  int last_weapon;
  int queued_weapon;
  int reload_timer;
  int attack_tick;
  /* see unique_reference/world.h */
  int hit_num;
  int move_restrictions;

  /* The input of the tee, the last one it was given: the server keeps it as
   * CCharacter::m_LatestInput, m_LatestPrevInput, m_SavedInput and m_Input,
   * which hold the same between two ticks (the tee takes the direct and the
   * predicted input of a tick together, or neither). */
  ddnet_input_t input;
  int num_inputs; /* (counts up to 2) */

  int health;
  int armor;

  /* see unique_reference/world.h */
  float time;
  ddnet_race_state_t race_state;
  int start_time;
  float start_time_offset;
  bool got_flag[2];

  int freeze_time; /* ticks left frozen */
  int tune_zone;
  ddnet_vec2_t prev_pos;
  int tele_checkpoint;
  int spawn_tick;
} ddnet_character_t;

/* CPlayer: the client, with or without a tee in the world. */
typedef struct ddnet_player_t {
  /* The input that is applied on the next ddnet_world_tick(). */
  ddnet_input_t input;

  /* The tee is world->character. */
  bool has_character;
  bool spawning;

  int die_tick;
  int previous_die_tick;
  int last_kill;
  int player_flags;
  ddnet_vec2_t view_pos;
  int tune_zone;
  int fastcap_side; /* see unique_reference/world.h */

  int finish_tick; /* -1 until finished */
  float finish_time;
} ddnet_player_t;

/* A grenade. */
typedef struct ddnet_projectile_t {
  ddnet_vec2_t direction;
  int life_span;
  int start_tick;
  int tune_zone;
} ddnet_projectile_t;

/* What ddnet_projectile_get_pos() takes: a grenade of world->projectiles. */
typedef struct ddnet_entity_t {
  ddnet_vec2_t pos;
  union {
    ddnet_projectile_t projectile;
  } u;
} ddnet_entity_t;

/* The pickups of Unique Race, numbered like its POWERUP_*. */
enum {
  DDNET_UNIQUE_POWERUP_HEALTH = 0,
  DDNET_UNIQUE_POWERUP_ARMOR,
  DDNET_UNIQUE_POWERUP_WEAPON,
};

/* A pickup of the map (which never moves in the maps of Unique Race). */
typedef struct ddnet_pickup_t {
  ddnet_vec2_t pos;
  int type;    /* DDNET_UNIQUE_POWERUP_* */
  int subtype; /* weapon */
} ddnet_pickup_t;

/* What the copies of a world share: the tuning and the pickups of the map.
 * See ddnet_world_tuning_edit(). */
typedef struct ddnet_world_shared_t {
  int refs; /* the worlds that hold it (the last one frees it) */
  ddnet_tuning_t *tuning;              /* [num_tune_zones] */
  ddnet_tuning_values_t *tuning_values; /* [num_tune_zones] */
  ddnet_pickup_t *pickups;             /* [num_pickups], in the order the server ticks them */
  /* for every pickup of the collision (ddnet_collision_t::pickups) its index
   * in pickups, -1 if Unique Race does not have it */
  int *pickup_of_map;
} ddnet_world_shared_t;

/* The complete dynamic state of a game of one player. */
typedef struct ddnet_world_t {
  const ddnet_collision_t *collision;
  /* the config passed to init with the map settings and the rules of the game
   * mode applied */
  ddnet_config_t config;

  int tick;

  /* see unique_reference/world.h */
  int tele_out;

  /* Tune zones, [0] is the global tuning (shared with the copies of the world). */
  const ddnet_tuning_t *tuning;
  const ddnet_tuning_values_t *tuning_values;
  int num_tune_zones;

  ddnet_player_t player;
  ddnet_character_t character; /* valid while player.has_character */

  /* The grenades, in the order the server ticks them (the newest first). */
  ddnet_entity_t *projectiles;
  int num_projectiles;
  int projectile_capacity;

  /* The pickups (shared), and with sv_health_and_ammo the tick each is gone
   * for the tee until (CPickup::m_aSpawnTick of the player, -1 if it is there). */
  const ddnet_pickup_t *pickups;
  int num_pickups;
  int *pickup_spawn_ticks; /* [num_pickups], NULL without sv_health_and_ammo */
  int num_respawning;       /* how many of them are not -1 */
  int next_respawn;         /* the smallest of those (INT_MAX if none) */

  /* fastcap: where the flags stand, [0] red, [1] blue (0, 0 if the map has none) */
  ddnet_vec2_t flag_pos[2];

  ddnet_world_shared_t *shared;
} ddnet_world_t;

#endif
