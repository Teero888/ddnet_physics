/* The structures of the reference implementation: DDNet's classes as plain C
 * structs, member for member. Include <ddnet_physics/world.h>, not this. */
#ifndef DDNET_PHYSICS_REFERENCE_WORLD_H
#define DDNET_PHYSICS_REFERENCE_WORLD_H

#include "../collision.h"
#include "../config.h"
#include "../events.h"
#include "../tuning.h"
#include "../types.h"
#include "../vmath.h"

typedef struct ddnet_weapon_stat_t {
  int ammo_regen_start;
  int ammo;
  int ammo_cost;
  bool got;
} ddnet_weapon_stat_t;

typedef struct ddnet_ninja_t {
  ddnet_vec2_t activation_dir;
  int activation_tick;
  int current_move_time;
  int old_vel_amount;
} ddnet_ninja_t;

/* CCharacterCore: movement, jumping and the hook. */
typedef struct ddnet_character_core_t {
  ddnet_vec2_t pos;
  ddnet_vec2_t vel;

  ddnet_vec2_t hook_pos;
  ddnet_vec2_t hook_dir;
  ddnet_vec2_t hook_tele_base;
  int hook_tick;
  int hook_state;
  int hooked_player;

  int active_weapon;
  ddnet_weapon_stat_t weapons[DDNET_NUM_WEAPONS];
  ddnet_ninja_t ninja;

  bool new_hook;

  int jumped;       /* bit 0: jump is held, bit 1: all air jumps are used up */
  int jumped_total; /* jumps performed in the air */
  int jumps;        /* number of jumps the tee has */
  int direction;
  int angle;
  ddnet_input_t input;

  int triggered_events;

  int id; /* client id */
  int colliding;
  bool left_wall;

  bool solo;
  bool jetpack;
  bool collision_disabled;
  bool endless_hook;
  bool endless_jump;
  bool hammer_hit_disabled;
  bool grenade_hit_disabled;
  bool laser_hit_disabled;
  bool shotgun_hit_disabled;
  bool hook_hit_disabled;

  bool has_telegun_gun;
  bool has_telegun_grenade;
  bool has_telegun_laser;
  int freeze_start;
  int freeze_end;
  bool is_in_freeze;
  bool deep_frozen;
  bool live_frozen;

  /* The tuning of the tune zone the tee is in, refreshed every tick. */
  ddnet_tuning_t tuning;

  int move_restrictions;
} ddnet_character_core_t;

/* Entities of one kind are chained in the order DDNet ticks them. */
typedef struct ddnet_entity_link_t {
  int prev;
  int next;
} ddnet_entity_link_t;

/* CCharacter: a tee that is in the world. */
typedef struct ddnet_character_t {
  ddnet_entity_link_t link; /* other characters, by client id */
  bool alive;
  ddnet_vec2_t pos; /* position after the last tick, core.pos is the working copy */

  ddnet_character_core_t core;

  int hit_objects[DDNET_MAX_CLIENTS]; /* players already hit by the current ninja slash */
  int num_objects_hit;

  int last_weapon;
  int queued_weapon;
  int reload_timer;
  int attack_tick;
  int move_restrictions;

  ddnet_input_t latest_prev_input;
  ddnet_input_t latest_input;
  ddnet_input_t prev_input;
  ddnet_input_t input;
  ddnet_input_t saved_input;
  int num_inputs;

  int health;
  int armor;

  float time; /* race time in seconds */
  ddnet_race_state_t race_state;
  int start_time; /* tick the race started */
  int last_time_cp;
  float current_time_cp[DDNET_MAX_CHECKPOINTS];

  int freeze_time; /* ticks left frozen */
  bool frozen_last_tick;
  int tune_zone;
  int tune_zone_old;
  ddnet_vec2_t prev_pos;
  int tele_checkpoint;
  int tile_index;
  int tile_f_index;
  bool last_refill_jumps;
  bool last_penalty;
  bool last_bonus;
  ddnet_vec2_t tele_gun_pos;
  bool tele_gun_teleport;
  bool is_blue_tele_gun_teleport;
  int strong_weak_id; /* position in the tick order, 0 has the strongest hook */
  int spawn_tick;

  bool paused; /* taken out of the world by /spec, see DDNET_PAUSE_SPEC */
  int paused_tick;
  /* CCharacter::m_PainSoundTimer: ticks until a frozen tee that fires makes its pain sound again (only for
   * the sound, see events.h; the optimized backend keeps the tick it ends on instead) */
  int pain_sound_timer;

  /* The two members below are controls for the caller. They are not reset
   * when the tee respawns and can be set at any time, also while the player
   * has no tee. */

  /* Nonzero while the player wants to be in /spec. The /spec chat command is
   * issued before a tick whenever this differs from the current state
   * (players[i].paused). What /spec does depends on sv_pauseable, see
   * DDNET_PAUSE_*. */
  int spec;

  /* Picks the exit when this tee, its hook or one of its bullets and lasers
   * goes through a teleporter that has several exits with the same number:
   * exit (unsigned)tele_out % number of exits, in map order. A server draws a
   * random number here; there is no randomness in this library, so change
   * this value whenever you want another exit. */
  int tele_out;
} ddnet_character_t;

/* CPlayer: a connected client, with or without a tee in the world. */
typedef struct ddnet_player_t {
  bool active; /* slot is in use, see ddnet_player_join() */

  /* The input that is applied on the next ddnet_world_tick(). */
  ddnet_input_t input;

  /* The tee of this player is world->characters[client id]. */
  bool has_character;

  bool spawning;
  bool weak_hook_spawn; /* respawn after the others of a killed team, to get weak hook on them */

  int die_tick;
  int previous_die_tick;
  int player_flags;
  ddnet_vec2_t view_pos;
  int tune_zone;
  bool ninja_jetpack;

  int paused;     /* DDNET_PAUSE_* */
  int last_pause; /* tick of the last change of paused */

  uint32_t unique_id; /* distinguishes players that reused a slot */

  /* Set when the player finishes the race, for the caller to read. */
  int finish_tick; /* -1 until finished */
  int finish_time_ticks;
} ddnet_player_t;

/* CGameTeams: ddrace teams and their race progress.
 *
 * Tees of different teams do not interact and every team has its own switch
 * states. In team 0 every tee races for itself; a team with a number starts
 * and finishes together. Put a player into a team with ddnet_player_set_team()
 * and lock a team by setting team_locked. */
typedef struct ddnet_teams_t {
  int team[DDNET_MAX_CLIENTS];
  bool is_solo[DDNET_MAX_CLIENTS];
  bool tee_started[DDNET_MAX_CLIENTS]; /* went through the start line */
  bool tee_finished[DDNET_MAX_CLIENTS];
  ddnet_team_state_t team_state[DDNET_NUM_TEAMS];
  /* The tees of a locked team stay in it when they die, and all die together. */
  bool team_locked[DDNET_NUM_TEAMS];
  int team_unfinishable_kill_tick[DDNET_NUM_TEAMS];
} ddnet_teams_t;

/* The state of one switch number, separate for every team. */
typedef struct ddnet_switcher_t {
  bool status[DDNET_NUM_TEAMS];
  bool initial;
  int end_tick[DDNET_NUM_TEAMS];
  int type[DDNET_NUM_TEAMS];
  int last_update_tick[DDNET_NUM_TEAMS];
} ddnet_switcher_t;

/* The entity lists of DDNet's CGameWorld. Draggers, turrets, plasma bullets
 * and laser walls all live in the laser list. */
enum {
  DDNET_ENTTYPE_PROJECTILE = 0,
  DDNET_ENTTYPE_LASER,
  DDNET_ENTTYPE_PICKUP,
  DDNET_ENTTYPE_FLAG,
  DDNET_ENTTYPE_CHARACTER,
  DDNET_NUM_ENTTYPES
};

typedef enum ddnet_entity_kind_t {
  DDNET_ENTITY_FREE = 0, /* unused slot */
  DDNET_ENTITY_PROJECTILE,
  DDNET_ENTITY_LASER,
  DDNET_ENTITY_PICKUP,
  DDNET_ENTITY_DRAGGER,
  DDNET_ENTITY_DRAGGER_BEAM,
  DDNET_ENTITY_GUN,
  DDNET_ENTITY_PLASMA,
  DDNET_ENTITY_LIGHT,
} ddnet_entity_kind_t;

typedef struct ddnet_projectile_t {
  ddnet_vec2_t direction;
  int life_span;
  int owner;
  int type; /* weapon */
  int start_tick;
  bool explosive;
  int bouncing;
  bool freeze;
  int tune_zone;

} ddnet_projectile_t;

/* Who a laser may hit, remembered from when its owner was last connected. */
typedef struct ddnet_interactions_t {
  int owner_id;
  uint32_t unique_owner_id;
  bool owner_alive;
  int ddrace_team;
  bool solo;
  bool no_hit_others;
  bool no_hit_self;
} ddnet_interactions_t;

typedef struct ddnet_laser_t {
  ddnet_vec2_t from;
  ddnet_vec2_t dir;
  ddnet_vec2_t tele_pos;
  bool was_tele;
  float energy;
  int bounces;
  int eval_tick;
  int owner;
  bool zero_energy_bounce_in_last_tick;
  ddnet_interactions_t interact_state;
  ddnet_vec2_t prev_pos;
  int type; /* DDNET_WEAPON_LASER or DDNET_WEAPON_SHOTGUN */
  int tune_zone;
  bool teleport_cancelled;
  bool is_blue_teleport;

} ddnet_laser_t;

typedef struct ddnet_pickup_t {
  ddnet_vec2_t core; /* movement per mover step */
  int type;          /* DDNET_POWERUP_* */
  int subtype;       /* weapon */
} ddnet_pickup_t;

typedef struct ddnet_dragger_t {
  ddnet_vec2_t core;
  float strength;
  bool ignore_walls;
  int eval_tick;
  int targets; /* index into world->dragger_targets */
} ddnet_dragger_t;

typedef struct ddnet_dragger_beam_t {
  int dragger; /* entity index of the dragger this beam belongs to */
  float strength;
  bool ignore_walls;
  int for_client_id;
  int eval_tick;
  bool active;
} ddnet_dragger_beam_t;

typedef struct ddnet_gun_t {
  ddnet_vec2_t core;
  bool freeze;
  bool explosive;
  int eval_tick;
  int timers; /* index into world->gun_timers */
} ddnet_gun_t;

typedef struct ddnet_plasma_t {
  ddnet_vec2_t core;
  int freeze;
  bool explosive;
  int for_client_id;
  int eval_tick;
  int life_time;
} ddnet_plasma_t;

/* A rotating and/or extending freeze laser. */
typedef struct ddnet_light_t {
  float rotation;
  ddnet_vec2_t to;
  ddnet_vec2_t core;
  int eval_tick;
  int tick;
  int curve_length;
  int length_l;
  float angular_speed;
  int speed;
  int length;
} ddnet_light_t;

typedef struct ddnet_entity_t {
  ddnet_entity_kind_t kind;
  ddnet_entity_link_t link; /* other entities of the same list, by entity index */
  bool marked_for_destroy;
  ddnet_vec2_t pos;
  int layer;  /* map layer the entity was placed in */
  int number; /* switch number, 0 = not switched */
  union {
    ddnet_projectile_t projectile;
    ddnet_laser_t laser;
    ddnet_pickup_t pickup;
    ddnet_dragger_t dragger;
    ddnet_dragger_beam_t dragger_beam;
    ddnet_gun_t gun;
    ddnet_plasma_t plasma;
    ddnet_light_t light;
  } u;
} ddnet_entity_t;

/* Per player state of a dragger. */
typedef struct ddnet_dragger_targets_t {
  int target_id_in_team[DDNET_MAX_CLIENTS];
  int beam[DDNET_MAX_CLIENTS]; /* entity index of the beam on that player, -1 = none */
} ddnet_dragger_targets_t;

/* Per player state of a turret. */
typedef struct ddnet_gun_timers_t {
  int last_fire_team[DDNET_MAX_CLIENTS];
  int last_fire_solo[DDNET_MAX_CLIENTS];
} ddnet_gun_timers_t;

/* Position inside an entity list: list and index (client id for characters). */
typedef struct ddnet_entity_ref_t {
  int type;
  int index; /* -1 = none */
} ddnet_entity_ref_t;

/* The complete dynamic state of a game.
 *
 * Everything in here is plain data without pointers into itself, which is what
 * makes ddnet_world_copy() a handful of memcpys. The arrays are owned by the
 * world. */
typedef struct ddnet_world_t {
  const ddnet_collision_t *collision;
  ddnet_config_t config; /* the config passed to init with the map settings applied */

  int tick;

  /* Like ddnet_character_t::tele_out, for map entities that belong to no tee
   * (the bullets of map shotguns). */
  int tele_out;

  /* Tune zones, [0] is the global tuning. */
  ddnet_tuning_t *tuning;
  int num_tune_zones;

  ddnet_teams_t teams;

  ddnet_switcher_t *switchers;
  int num_switchers; /* 0, or highest switch number + 1 */

  ddnet_player_t *players;       /* [DDNET_MAX_CLIENTS] */
  ddnet_character_t *characters; /* [DDNET_MAX_CLIENTS], valid where players[i].has_character */
  int num_clients;               /* highest client id that was ever used + 1 */
  uint32_t next_unique_id;

  ddnet_entity_t *entities;
  int num_entities; /* slots in use, including free ones in between */
  int entity_capacity;
  int first_free_entity; /* free slots are chained through link.next */

  ddnet_dragger_targets_t *dragger_targets;
  int num_draggers;
  ddnet_gun_timers_t *gun_timers;
  int num_guns;

  int first_entity[DDNET_NUM_ENTTYPES];
  ddnet_entity_ref_t next_traverse_entity;

  /* Effects and sounds (events.h): called when set, with user_data. ddnet_world_copy copies them too. */
  void *user_data;
  ddnet_particle_fn particle;
  ddnet_damage_indicator_fn damage_indicator;
  ddnet_sound_fn sound;
} ddnet_world_t;

/* The state of switch number `number` for team `team` (as the optimized
 * backend has it, which keeps it by team). */
typedef struct ddnet_switch_state_t {
  int end_tick;
  int last_update_tick;
  bool status;
  uint8_t type;
} ddnet_switch_state_t;
static inline ddnet_switch_state_t ddnet_world_switch(const ddnet_world_t *world, int number, int team) {
  const ddnet_switcher_t *switcher = &world->switchers[number];
  const ddnet_switch_state_t state = {switcher->end_tick[team], switcher->last_update_tick[team],
                                      switcher->status[team], (uint8_t)switcher->type[team]};
  return state;
}

#endif
