/* The structures of the reference implementation of Unique Race: the classes
 * of its server as plain C structs, member for member, for a world that holds
 * a single player and only what the maps of Unique Race use (see
 * ddnet_map_unsupported() in collision.h). Include <ddnet_physics/world.h>,
 * not this. */
#ifndef DDNET_PHYSICS_UNIQUE_REFERENCE_WORLD_H
#define DDNET_PHYSICS_UNIQUE_REFERENCE_WORLD_H

#include "../collision.h"
#include "../config.h"
#include "../events.h"
#include "../tuning.h"
#include "../types.h"
#include "../vmath.h"

/* Unique Race's maxammo of the grenade, the ammo of sv_health_and_ammo. */
enum { DDNET_GRENADE_MAX_AMMO = 10 };

typedef struct ddnet_weapon_stat_t {
  int ammo; /* -1 is unlimited */
  bool got;
} ddnet_weapon_stat_t;

/* CCharacterCore: movement, jumping and the hook. */
typedef struct ddnet_character_core_t {
  ddnet_vec2_t pos;
  ddnet_vec2_t vel;
  /* The acceleration the hook gave the tee in the last tick (not Unique Race's: for searches) */
  ddnet_vec2_t hook_accel;

  ddnet_vec2_t hook_pos;
  ddnet_vec2_t hook_dir;
  int hook_tick;
  int hook_state;

  /* The tee has the hammer and the gun, and in Unique Race nothing else than
   * the grenade (the shotgun, laser and ninja slots stay empty). The hammer
   * and the gun hit nothing a solo tee could feel, but switching to them and
   * firing them takes the time it takes. */
  int active_weapon;
  ddnet_weapon_stat_t weapons[DDNET_NUM_WEAPONS];

  int jumped;       /* bit 0: jump is held, bit 1: all air jumps are used up */
  int jumped_total; /* jumps performed in the air */
  int jumps;        /* number of jumps the tee has */
  int direction;
  int angle;
  ddnet_input_t input;

  int triggered_events;

  int colliding;
  bool left_wall;

  int freeze_start;

  /* The tuning of the tune zone the tee is in, refreshed every tick. */
  ddnet_tuning_t tuning;

  int move_restrictions;
} ddnet_character_core_t;

/* Entities of one kind are chained in the order the server ticks them. */
typedef struct ddnet_entity_link_t {
  int prev;
  int next;
} ddnet_entity_link_t;

/* CCharacter: the tee, while it is in the world. */
typedef struct ddnet_character_t {
  bool alive;
  ddnet_vec2_t pos; /* position after the last tick, core.pos is the working copy */

  ddnet_character_core_t core;

  int last_weapon;
  int queued_weapon;
  int reload_timer;
  int attack_tick;
  /* How hard the tee was hit lately (not Unique Race's: for searches). A tick
   * takes one off first, down to 0, and every explosion of the tick then adds
   * its damage. Read after a tick, it has all of that tick's hits. */
  int hit_num;
  int move_restrictions;

  ddnet_input_t latest_prev_input;
  ddnet_input_t latest_input;
  ddnet_input_t prev_input;
  ddnet_input_t input;
  ddnet_input_t saved_input;
  int num_inputs;

  int health;
  int armor;
  /* sv_health_and_ammo: for the stars of the damage indicators only */
  int damage_taken;
  int damage_taken_tick;

  /* The race time in seconds, as of the last tile the tee went through
   * (CCharacter::m_Time). Unique Race times the race in fractions of a tick:
   * start_time + start_time_offset is when the tee crossed the start line,
   * in ticks. */
  float time;
  ddnet_race_state_t race_state;
  int start_time;
  float start_time_offset;
  /* fastcap: the flags the tee touched in this life, [0] red, [1] blue */
  bool got_flag[2];

  /* Hearts freeze in race without sv_health_and_ammo, and freezing in Unique
   * Race never ends early: the tee is frozen for the full freeze delay. */
  int freeze_time; /* ticks left frozen */
  bool frozen_last_tick;
  int tune_zone;
  ddnet_vec2_t prev_pos;
  int tele_checkpoint;
  int spawn_tick;

  /* CCharacter::m_PainSoundTimer: only for the sound, see events.h */
  int pain_sound_timer;
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
  int last_kill; /* tick of the last kill, see ddnet_player_kill() */
  int player_flags;
  ddnet_vec2_t view_pos;
  int tune_zone;

  /* fastcap: where the tee spawns, 1 at the red spawns, 2 at the blue ones
   * (the /red and /blue chat commands, see ddnet_player_set_fastcap_side()). */
  int fastcap_side;

  /* Set when the tee finishes the race, for the caller to read: the tick, and
   * the time in seconds as the server takes it (CGameTeams::OnFinish). */
  int finish_tick; /* -1 until finished */
  float finish_time;
} ddnet_player_t;

/* The entity lists of the server's CGameWorld that exist in Unique Race. */
enum {
  DDNET_ENTTYPE_PROJECTILE = 0,
  DDNET_ENTTYPE_PICKUP,
  DDNET_NUM_ENTTYPES
};

typedef enum ddnet_entity_kind_t {
  DDNET_ENTITY_FREE = 0, /* unused slot */
  DDNET_ENTITY_PROJECTILE,
  DDNET_ENTITY_PICKUP,
} ddnet_entity_kind_t;

/* A grenade. */
typedef struct ddnet_projectile_t {
  ddnet_vec2_t direction;
  int life_span;
  int start_tick;
  int tune_zone;
} ddnet_projectile_t;

/* The pickups of Unique Race, numbered like its POWERUP_*. */
enum {
  DDNET_UNIQUE_POWERUP_HEALTH = 0, /* a heart: freezes, or with sv_health_and_ammo gives health */
  DDNET_UNIQUE_POWERUP_ARMOR,      /* a shield: takes the grenade, or with sv_health_and_ammo gives armor */
  DDNET_UNIQUE_POWERUP_WEAPON,     /* the grenade */
};

typedef struct ddnet_pickup_t {
  int type; /* DDNET_UNIQUE_POWERUP_* */
  int subtype; /* weapon */
  /* sv_health_and_ammo: the pickup is gone for the tee until after this
   * tick, -1 if it is there (CPickup::m_aSpawnTick of the player) */
  int spawn_tick;
} ddnet_pickup_t;

typedef struct ddnet_entity_t {
  ddnet_entity_kind_t kind;
  ddnet_entity_link_t link; /* other entities of the same list, by entity index */
  bool marked_for_destroy;
  ddnet_vec2_t pos;
  union {
    ddnet_projectile_t projectile;
    ddnet_pickup_t pickup;
  } u;
} ddnet_entity_t;

/* Position inside an entity list: list and index. */
typedef struct ddnet_entity_ref_t {
  int type;
  int index; /* -1 = none */
} ddnet_entity_ref_t;

/* The complete dynamic state of a game of one player.
 *
 * Everything in here is plain data without pointers into itself, which is
 * what makes ddnet_world_copy() a handful of memcpys. The arrays are owned by
 * the world. */
typedef struct ddnet_world_t {
  const ddnet_collision_t *collision;
  /* the config passed to init with the map settings and the rules of the game
   * mode applied */
  ddnet_config_t config;

  int tick;

  /* Picks the exit when the tee or one of its grenades goes through
   * a teleporter that has several exits with the same number: exit
   * (unsigned)tele_out % number of exits, in map order. A server draws a
   * random number here; there is no randomness in this library, so change
   * this value whenever you want another exit. */
  int tele_out;

  /* Tune zones, [0] is the global tuning. */
  ddnet_tuning_t *tuning;
  int num_tune_zones;

  ddnet_player_t player;
  ddnet_character_t character; /* valid while player.has_character */

  ddnet_entity_t *entities;
  int num_entities; /* slots in use, including free ones in between */
  int entity_capacity;
  int first_free_entity; /* free slots are chained through link.next */

  int first_entity[DDNET_NUM_ENTTYPES];
  ddnet_entity_ref_t next_traverse_entity;

  /* fastcap: where the flags stand, [0] red, [1] blue (0, 0 if the map has none) */
  ddnet_vec2_t flag_pos[2];

  /* Effects and sounds (events.h): called when set, with user_data. ddnet_world_copy copies them too. */
  void *user_data;
  ddnet_particle_fn particle;
  ddnet_damage_indicator_fn damage_indicator;
  ddnet_sound_fn sound;
} ddnet_world_t;

#endif
