/* The structures of the reference implementation: DDNet's classes as plain C
 * structs, member for member. Include <ddnet_physics/world.h>, not this. */
#ifndef DDNET_PHYSICS_OPTIMIZED_WORLD_H
#define DDNET_PHYSICS_OPTIMIZED_WORLD_H

#include "../collision.h"
#include "../config.h"
#include "../events.h"
#include "../tuning.h"
#include "../types.h"
#include "../vmath.h"

/* The tuning as the floats the physics compute with (DDNet converts on every
 * read). Derived from ddnet_world_t::tuning by ddnet_world_tuning_changed(). */
typedef struct ddnet_tuning_values_t {
#define DDNET_TUNING_PARAM(name, default_value) float name;
#include "../tuning_params.h"
#undef DDNET_TUNING_PARAM
  /* Worked out from the values above. A tee is slower than velramp_start,
   * which leaves its velocity as it is, exactly if the square of its speed is
   * below this: the root does not have to be taken to know. */
  float velramp_start_squared;
  /* The same for the clamp of the speed to 6000 in CCharacterCore::TickDeferred: the smallest square of a
   * speed that is above it. */
  float speed_clamp_squared;
  /* ... and for the length of the hook in CCharacterCore::Tick: the smallest square of a distance that
   * is above hook_length (not a number if there is none) */
  float hook_length_squared;
  /* For VelocityRamp(), see velocity_ramp_prepare(): which powf() it can
   * compute itself, and the logarithm of velramp_curvature that it begins
   * with. */
  int velramp_powf;
  double velramp_log2_curvature;
} ddnet_tuning_values_t;

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
  /* The input of the tee: the last one it took (DDNet keeps it four times, in
   * CCharacter's m_SavedInput, m_LatestInput, m_LatestPrevInput and m_Input,
   * and in the core's m_Input, which hold the same between two inputs, except
   * for the moves a frozen tee cannot make: the core leaves those out). */
  ddnet_input_t input;
  /* The aim of the input of the last tick of the core (0, 0 before the
   * first), which the angle follows from (it is not stored, see
   * ddnet_character_angle()). It is not always the one of the input: DDNet
   * takes the input of the tees that come after one that dies during its
   * tick, but skips their tick. */
  int aim_x, aim_y;

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

  /* The players already hit by the current ninja slash, one bit per client id
   * (DDNet keeps a list; only whether a player is in it matters). */
  uint64_t hit_objects[(DDNET_MAX_CLIENTS + 63) / 64];

  int last_weapon;
  int queued_weapon;
  int reload_timer;
  int attack_tick;
  /* How hard the tee was hit lately (not DDNet's: for searches). A tick takes
   * one off first, down to 0, and every hit of the tick then adds to it: an
   * explosion its damage, a shotgun or laser 2, a hammer 3, a ninja 9. Read
   * after a tick, it has all of that tick's hits. */
  int hit_num;
  int move_restrictions;

  int num_inputs; /* (the input is core.input) */

  int health;
  int armor;

  ddnet_race_state_t race_state;
  int start_time; /* tick the race started */
  int last_time_cp;
  float current_time_cp[DDNET_MAX_CHECKPOINTS];

  int freeze_time; /* ticks left frozen */
  bool frozen_last_tick;
  /* Looked up once per position of the tee (quiet_pos): the tile it is on (positions outside of the map are
   * on the tiles at its border), the flags of that tile, and whether nothing of the map is around, see
   * DDNET_TILEFLAG_QUIET. */
  bool quiet, quiet_known;
  ddnet_vec2_t quiet_pos;
  int here_tile, here_flags;
  bool here_on_map;
  bool paused; /* taken out of the world by /spec, see DDNET_PAUSE_SPEC */
  /* where the tee was when it was last compared with all projectiles, see ddnet_world_t::projectile_slack */
  ddnet_vec2_t projectile_check_pos;
  ddnet_vec2_t projectile_parked_check_pos;
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

  int paused_tick;
  /* CCharacter::m_PainSoundTimer, as the tick from which a frozen tee that fires makes its pain sound again
   * (only for the sound, see events.h; a tick instead of a countdown, which the build without events would
   * have to count down on every tick, so that both builds keep it and a world can go from one to the other) */
  int pain_sound_tick;

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
  /* CGameTeams of this client slot (it keeps them while nobody is connected
   * on it): its ddrace team (put a player into one with
   * ddnet_player_set_team()), whether it is in a solo part, and whether its
   * tee went through the start line and the finish line of its team. */
  uint8_t team; /* (its row in world->teams is team_row while the player is connected) */
  bool is_solo;
  bool tee_started;

  /* The input that is applied on the next ddnet_world_tick(). */
  ddnet_input_t input;

  /* The tee of this player is world->characters[client id]. */
  bool has_character;

  bool spawning;
  bool weak_hook_spawn; /* respawn after the others of a killed team, to get weak hook on them */
  bool tee_finished;    /* (with team above) */

  int die_tick;
  int previous_die_tick;
  int player_flags;
  ddnet_vec2_t view_pos;
  int tune_zone;
  bool ninja_jetpack;
  uint8_t team_row;

  int paused;     /* DDNET_PAUSE_* */
  int last_pause; /* tick of the last change of paused */

  uint32_t unique_id; /* distinguishes players that reused a slot */

  /* Set when the player finishes the race, for the caller to read. */
  int finish_tick; /* -1 until finished */
  int finish_time_ticks;
} ddnet_player_t;

/* CGameTeams of one ddrace team: tees of different teams do not interact and
 * every team has its own switch states. In team 0 every tee races for itself;
 * a team with a number starts and finishes together. Only the teams tees are
 * in have one of these (see ddnet_world_t::teams); read one with
 * ddnet_world_team(), lock one with ddnet_world_lock_team(). */
typedef struct ddnet_team_t {
  int kill_tick;  /* a team that cannot finish anymore is killed on this tick, -1 = none */
  uint8_t number; /* which team this is (255: a row that no team has now, see ddnet_world_t::teams) */
  uint8_t state;  /* ddnet_team_state_t */
  bool locked;    /* its tees stay in it when they die, and all die together */
  /* some switch of it may be in another state than a reset leaves it in (a
   * reset of its switches has nothing to do otherwise) */
  bool switches_touched;
} ddnet_team_t;

/* The state of one switch number for one team (DDNet's CSwitcher keeps one of
 * these per team in every switch number). Read it with ddnet_world_switch(). */
typedef struct ddnet_switch_state_t {
  int end_tick;
  int last_update_tick;
  bool status;
  uint8_t type; /* the switch tile that set it last */
} ddnet_switch_state_t;

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

/* (The small fields are bytes and at the end, without holes: with the header of
 * an entity this is exactly two cache lines.) */
typedef struct ddnet_projectile_t {
  ddnet_vec2_t direction;
  /* The tick on which the lifetime ends (DDNet counts m_LifeSpan down every tick). */
  int expire_tick;
  /* Nothing on the map can happen to the projectile before this tick. */
  int next_tick;
  /* Until then it can only reach tees in this rectangle. */
  ddnet_vec2_t reach_min, reach_max;
  /* Projectiles are kept by the tick they have to be looked at on, see
   * ddnet_world_t::projectile_due: the tick this one is filed under (-1 =
   * none) and its neighbors there. seq orders them like the entity list. */
  int due_tick, due_prev, due_next;
  unsigned seq;
  /* A projectile of the map flies the same way again and again as long as no
   * tee is near (its orbit). orbit_period: after how many ticks it is back
   * where it is, 0 = not known. parked: it is not looked at at all; it is
   * where it was on parked_tick, reach_min/reach_max are the rectangle of the
   * whole orbit, and it is brought up to date when a tee gets in there or
   * with ddnet_world_sync(). orbit_min/orbit_max keep that rectangle. */
  int orbit_period;
  int orbit_anchor_tick; /* from this tick on it is on the orbit */
  int orbit_retry_tick;
  /* A gun bullet whose owner is the only tee in the world does nothing to
   * anybody. As long as that is so it is not simulated at all (lazy), and
   * what happened to it since lazy_tick is worked out when it matters: when
   * another tee comes, its owner goes, or with ddnet_world_sync(). */
  int lazy_tick;
  int set_slot; /* where it is in ddnet_world_t::projectile_awake or projectile_parked */
  int parked_tick;
  ddnet_vec2_t orbit_min, orbit_max;
  int owner;
  int start_tick;
  int8_t type; /* weapon */
  int8_t bouncing;
  uint8_t tune_zone;
  /* false if next_tick is only as far as was looked, with nothing happening on it */
  bool next_is_event;
  unsigned char orbit_tries;
  bool orbit_uses_choice; /* it goes through a teleporter with several exits: the orbit is one for
                             ddnet_world_t::tele_out as it is */
  bool lazy;
  bool parked;
  bool explosive;
  bool freeze;
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
  int eval_tick; /* not kept (only the creation tick): DDNet only sends it to old clients */
  /* index into world->dragger_targets while it has a target or beam, -1 while
   * it is idle (read them with ddnet_dragger_target() and ddnet_dragger_beam()) */
  int targets;
  /* no targets and no beams: nothing to do while no tee is in range */
  bool idle;
  ddnet_sight_t sight; /* what is known about its line of sight */
} ddnet_dragger_t;

typedef struct ddnet_dragger_beam_t {
  int dragger; /* entity index of the dragger this beam belongs to */
  float strength;
  bool ignore_walls;
  int for_client_id;
  int eval_tick;
  bool active;
  ddnet_sight_t sight; /* what is known about its line of sight */
} ddnet_dragger_beam_t;

typedef struct ddnet_gun_t {
  ddnet_vec2_t core;
  bool freeze;
  bool explosive;
  int eval_tick;       /* not kept (only the creation tick): DDNet only sends it to old clients */
  int timers;          /* which turret it is: its row of world->gun_timers */
  ddnet_sight_t sight; /* what is known about its line of sight */
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
  int eval_tick; /* only kept while it ticks (DDNet only sends it to old clients) */
  int tick;
  int curve_length;
  int length_l;
  float angular_speed;
  int speed;
  int length;
  /* it neither moves nor turns: every step ends where the last one did */
  bool settled;
  /* The last tick it ticked on. It does not tick while no tee is near, and makes up for it afterwards. */
  int last_tick;
} ddnet_light_t;

/* (Bytes where they do: a world has many entities, and copies them all. 128
 * bytes, two cache lines.) */
typedef struct ddnet_entity_t {
  ddnet_entity_link_t link; /* other entities of the same list, by entity index */
  ddnet_vec2_t pos;
  uint8_t kind; /* ddnet_entity_kind_t */
  bool marked_for_destroy;
  uint8_t layer;  /* map layer the entity was placed in */
  uint8_t number; /* switch number (of the switch layer, a byte there), 0 = not switched */
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
enum { DDNET_MAX_TIMED_SWITCHES = 64 };

/* What was found out about the way of a projectile that belongs to no tee:
 * how long nothing on the map happens to it, and where it gets to until then.
 * Bullets of map shotguns bounce between the same walls forever, this keeps
 * their ways from being worked out again every time. An entry only depends on
 * its key, the map and the tuning. */
typedef struct ddnet_projectile_memo_entry_t {
  /* key: where it started and how many ticks ago, what kind it is, and which
   * of the two questions this answers */
  ddnet_vec2_t pos, direction;
  int kind, tune_zone, age;
  /* Ticks until the map does something to it, or until this was not looked
   * at any further (DDNET_PROJECTILE_MEMO_NO_EVENT is added then). For a
   * bounce 1. 0 = unused entry. */
  int ticks;
  /* where it gets to until then. For a bounce: its position and direction afterwards */
  ddnet_vec2_t reach_min, reach_max;
} ddnet_projectile_memo_entry_t;

enum { DDNET_PROJECTILE_MEMO_NO_EVENT = 1 << 16 };

enum { DDNET_PROJECTILE_MEMO_SIZE = 4096 };

typedef struct ddnet_projectile_memo_t {
  uint64_t universe; /* ddnet_world_t::memo_universe the entries are for */
  ddnet_projectile_memo_entry_t entries[DDNET_PROJECTILE_MEMO_SIZE];
} ddnet_projectile_memo_t;

/* A parked projectile: the rectangle of its orbit and its entity. */
typedef struct ddnet_parked_projectile_t {
  ddnet_vec2_t min, max;
  int index;
} ddnet_parked_projectile_t;

/* The tune zones of a world and the floats worked out from them, in one block
 * that copies of the world share: the tuning only changes with
 * ddnet_world_tuning_edit(), which gives a world a block of its own first.
 * num_tune_zones ddnet_tuning_values_t follow it (aligned as malloc aligns),
 * then as many ddnet_tuning_t. */
typedef struct ddnet_tune_block_t {
  int refs; /* the worlds that hold it (the last one frees it) */
  int count;
  int64_t padding;
} ddnet_tune_block_t;

/* The turrets, draggers and laser walls of the map that can have a tee in
 * their range, by where the tee is: the map in cells of
 * DDNET_LASER_GRID_CELL tiles, and for every cell the entities whose range
 * touches it, in the order they tick in. The others have nothing to do on a
 * tick. It only changes with the ranges in the config: a world with other
 * ranges makes a new one. Copies of a world share it. */
typedef struct ddnet_laser_grid_t {
  uint64_t id;                     /* a copy has the same */
  int refs;                        /* the worlds that hold it (the last one frees it) */
  size_t bytes;                    /* of all of it */
  int dragger_range, plasma_range; /* the config it was made for */
  int cells_x, cells_y;
  bool always_all; /* there is something else among them: all of them tick on every tick */
  /* the ones on conveyors, which tick on every tick: where their indices are in data, and how many */
  int movers, num_movers;
  /* [cells_x * cells_y + 1] offsets into the entity indices that follow them */
  int data[];
} ddnet_laser_grid_t;

enum { DDNET_LASER_GRID_CELL = 8 };

/* For a tile a tee is on: the turrets, draggers and laser walls of its cell
 * of the laser grid that a tee on that tile can matter to, in the order they
 * tick in. The others are out of range of every position on the tile or
 * cannot see any of it. Worked out when a tee first gets there. */
enum { DDNET_LASER_TILE_MAX = 124, DDNET_LASER_TILE_CACHE_SIZE = 1024 };
typedef struct ddnet_laser_tile_t {
  uint64_t grid_id; /* the laser grid this is for, 0 = empty */
  int tile;
  int count; /* -1: more than fit, all of the cell */
  int ids[DDNET_LASER_TILE_MAX];
} ddnet_laser_tile_t;
enum { DDNET_PROJECTILE_DUE_SIZE = 128 };
enum { DDNET_MAX_PICKUP_MOVERS = 32 };
/* A pickup on a conveyor (see ddnet_world_t::pickup_movers): its entity, and which pickup of the map it is. */
typedef struct ddnet_pickup_mover_t {
  int index, id;
} ddnet_pickup_mover_t;
enum { DDNET_MAX_BUSY_DRAGGERS = 32 };
enum { DDNET_MAX_MARKED_ENTITIES = 16 };

typedef struct ddnet_world_t {
  const ddnet_collision_t *collision;
  ddnet_config_t config; /* the config passed to init with the map settings applied */

  int tick;

  /* Like ddnet_character_t::tele_out, for map entities that belong to no tee
   * (the bullets of map shotguns). */
  int tele_out;

  /* Tune zones, [0] is the global tuning. Both arrays are in tune_block,
   * which a world shares with its copies: write to the tuning through
   * ddnet_world_tuning_edit() only. */
  ddnet_tuning_t *tuning;
  ddnet_tuning_values_t *tuning_values; /* see ddnet_world_tuning_changed() */
  int num_tune_zones;
  ddnet_tune_block_t *tune_block;

  /* The teams that somebody is in, a row each (which number a team has makes
   * no difference to that: a team takes a free row or a new one). A team
   * without a row is as if nobody had ever been in it: empty, not locked, no
   * kill tick, its switches as below. A row is freed when its team is left by
   * the last tee and nothing of the team can make a difference anymore (see
   * teams.c); num_team_rows counts the free ones too. */
  ddnet_team_t *teams;
  int num_team_rows;
  /* how teams_reset() left the client slots that nobody used yet: a team of
   * their own each (sv_team forced solo then), or team 0 */
  bool slots_forced_solo;

  /* The switches of the teams with rows, by row and then by switch number:
   * switch_states[row * num_switchers + number]. A team without a row has all
   * its switches as the map starts them (status true, the rest 0), or as a
   * reset leaves them (status switch_initial, type open) if
   * rowless_switches_reset; rowless_switches_touched is switches_touched of
   * the teams without a row. */
  ddnet_switch_state_t *switch_states;
  bool rowless_switches_reset, rowless_switches_touched;
  bool *switch_initial; /* by switch number: what a reset of a team sets its status to */
  int num_switchers;    /* 0, or highest switch number + 1 */

  /* By client id, for the ids below num_clients: the arrays grow when a client with a higher id joins (to
   * client_capacity entries, the ones past num_clients empty), so a world for a few players stays small to
   * copy and to make. */
  ddnet_player_t *players;
  ddnet_character_t *characters; /* valid where players[i].has_character */
  int num_clients;               /* highest client id that was ever used + 1 */
  int client_capacity;

  /* Client ids in ascending order, so that nothing has to scan all slots:
   * the connected players, and the tees that take part in the physics (alive
   * and not taken out by /spec). Room for client_capacity each, behind the
   * players in their allocation. */
  int num_players;
  uint8_t *player_ids;
  int num_cores;
  uint8_t *core_ids;

  /* Switches of a team that are waiting for their timer, as number << 8 | team
   * row (a switch number is below 256, see num_switchers). */
  int num_timed_switches;
  uint16_t *timed_switches; /* [DDNET_MAX_TIMED_SWITCHES] from the first timer on, NULL before */
  bool timed_switches_overflow; /* more than fit: all switches are scanned */
  bool unfinishable_teams; /* some team may be waiting for its kill tick */
  bool entities_marked;    /* some entity is marked for destruction */
  /* which ones, as long as they are few: more than fit means all entities are
   * looked through ([DDNET_MAX_MARKED_ENTITIES] from the first one on, NULL
   * before; only used during a tick, not copied) */
  int num_marked_entities;
  int *marked_entities;
  /* 0: all projectiles have to be looked at on the next tick. Set by
   * everything that changes what matters to them: new projectiles, tees
   * entering or leaving the world, tuning changes. */
  int projectile_wakeup;
  /* The projectiles to look at on a tick, by tick modulo the size: the first
   * entity of each list, -1 = none. Nothing is scheduled further ahead.
   * [DDNET_PROJECTILE_DUE_SIZE] from the first projectile on (NULL before).
   * Without projectiles it is all -1, so a copy only copies it while there are
   * projectiles (and empties it where it had some). */
  int *projectile_due;
  unsigned projectile_seq;
  /* tele_out and config.sv_old_teleport_weapons as the orbits of the
   * projectiles were worked out with. Parked projectiles went through
   * teleporters with these. */
  int orbit_tele_out;
  int orbit_old_teleport_weapons;
  /* The projectiles that are looked at (entity indices, in no order), and the
   * parked ones, see ddnet_projectile_t::parked. projectile_parked_slack is
   * projectile_slack for the parked ones, with projectile_parked_check_pos. */
  int *projectile_awake;
  int num_projectile_awake, projectile_awake_capacity;
  ddnet_parked_projectile_t *projectile_parked;
  int num_projectile_parked, projectile_parked_capacity;
  int num_lazy_projectiles;
  bool projectile_sets_failed; /* no memory for them: the entity list is gone through instead */
  float projectile_parked_slack;
  /* No tee that has moved less than this far (along x and y) from its
   * projectile_check_pos is where a projectile can get to before its next
   * tick. */
  float projectile_slack;
  /* Allocated when first needed, not copied with the world: a copy fills its
   * own. memo_universe names the map and tuning the world has; it changes
   * with the tuning. */
  ddnet_projectile_memo_t *projectile_memo;
  ddnet_sight_memo_t *sight_memo; /* the same for lines of sight: allocated when first needed, not copied */
  /* For ddnet_world_copy(): which switchers and per-player tables of
   * draggers and turrets were written to since this world was last the same
   * as another one (kind << 24 | index, see world_touch()). lineage names the
   * state the log starts from and changes when the log is of no use anymore;
   * origin_lineage and origin_pos say which world this one was copied from,
   * and how long the log of that one was then. */
  uint32_t *touch_log;
  int touch_count;
  uint64_t lineage, lineage_base, origin_lineage;
  int origin_pos;
  /* The entities of the map in the laser list: the first of them (they are
   * at the end of the list, everything created later is in front of them),
   * where they matter, and how many draggers have a target or beam. */
  /* The pickups on conveyors, which are the only ones that need their entity:
   * its index and which pickup of the map it is, the last of the map first.
   * pickups_generic: too many, or not as expected: all pickups tick like in DDNet. */
  int num_pickup_movers;
  ddnet_pickup_mover_t *pickup_movers; /* NULL if there are none, which is the usual */
  bool pickups_generic;
  int first_static_laser;
  ddnet_laser_grid_t *laser_grid;
  ddnet_laser_tile_t
      *laser_tiles; /* [DDNET_LASER_TILE_CACHE_SIZE], allocated when first needed, not copied */
  int busy_draggers;
  /* ... and which, as long as they are not more than fit (busy_draggers_overflow):
   * [DDNET_MAX_BUSY_DRAGGERS] from the first one on, NULL before */
  int *busy_dragger_ids;
  bool busy_draggers_overflow;
  /* set by the tick of a projectile: all it did was bounce off the map */
  bool projectile_only_bounced;
  uint64_t memo_universe;
  uint32_t next_unique_id;

  ddnet_entity_t *entities;
  int num_entities; /* slots in use, including free ones in between */
  int entity_capacity;
  int first_free_entity; /* free slots are chained through link.next */

  /* DDNet keeps, in every dragger and turret, something for every team and
   * every client there can be. Here those are tables with room for
   * table_teams team rows (see teams) and table_clients client slots:
   *
   * The targets and beams of the draggers that have some (a dragger takes a
   * table when it gets a target or beam and gives it back when it has none
   * left, all -1 again: most draggers have nobody in range). A table is the
   * entity index of its dragger (-1 = free), the client id of the target of
   * each team row, the entity index of the beam on each client (-1 = none);
   * num_dragger_targets of them. Read them with ddnet_dragger_target() and
   * ddnet_dragger_beam().
   *
   * When the turrets fired last (a tick): for each turret, by team row, then by
   * client for solo players. */
  int *dragger_targets;
  int num_dragger_targets;  /* used and free */
  int dragger_targets_room; /* ints allocated (a world keeps them, copies too) */
  int *gun_timers;
  int num_guns;
  int table_teams, table_clients;

  int first_entity[DDNET_NUM_ENTTYPES];
  ddnet_entity_ref_t next_traverse_entity;

  /* The tees in the world changed since the projectiles were last gone through: the lazy ones are looked at
   * (their owner may not be the one tee in the world anymore). */
  bool lazy_wake;
  /* (during a loop of the tick over the tees, the positions of them; NULL otherwise) */
  struct ddnet_tee_snapshot *tee_snapshot;

  /* Effects and sounds (events.h): called when set, with user_data, by the event build (ddnet_ev_*) only;
   * the same fields in both builds, so a world can go from one to the other. ddnet_world_copy copies them
   * too. */
  void *user_data;
  ddnet_particle_fn particle;
  ddnet_damage_indicator_fn damage_indicator;
  ddnet_sound_fn sound;

  /* (during the loop of the tick over CCharacter::PreTick with sv_no_weak_hook) */
  bool pre_ticking_characters;
} ddnet_world_t;

/* The target of a dragger in a team and its beam on a player, -1 for none. */
int ddnet_dragger_target(const ddnet_world_t *world, const ddnet_dragger_t *dragger, int team);
int ddnet_dragger_beam(const ddnet_world_t *world, const ddnet_dragger_t *dragger, int client_id);

/* A ddrace team (0 to DDNET_NUM_TEAMS - 1), as ddnet_team_t says. */
ddnet_team_t ddnet_world_team(const ddnet_world_t *world, int team);
/* Lock a team (not team 0) or unlock it. False if there is no memory for its row. */
bool ddnet_world_lock_team(ddnet_world_t *world, int team, bool locked);

/* The state of switch number `number` (1 to num_switchers - 1) for team `team`. */
ddnet_switch_state_t ddnet_world_switch(const ddnet_world_t *world, int number, int team);

#endif
