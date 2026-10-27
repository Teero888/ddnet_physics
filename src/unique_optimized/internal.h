/* Internal interface of the optimized implementation of Unique Race.
 *
 * It computes what the reference implementation (../unique_reference)
 * computes, bit for bit, for a world of one tee, with the collision of the
 * optimized DDNet implementation (../ddnet_optimized/collision.c: flags per
 * tile that say what is around, fast tests of boxes and lines). The tick is
 * compiled once per game mode (race, race with sv_health_and_ammo, fastcap,
 * fastcap without weapons), each without what its mode does not have.
 *
 * It has no events (events.h): the reference backend has them. */
#ifndef DDNET_PHYSICS_UNIQUE_OPTIMIZED_INTERNAL_H
#define DDNET_PHYSICS_UNIQUE_OPTIMIZED_INTERNAL_H

#include "../ddnet_optimized/collision.h"

#include <ddnet_physics/ddnet_physics.h>

typedef ddnet_world_t world_t;
typedef ddnet_player_t player_t;
typedef ddnet_character_t character_t;
typedef ddnet_character_core_t core_t;
typedef ddnet_input_t input_t;
typedef ddnet_tuning_values_t tuning_t;
typedef ddnet_entity_t projectile_t;

#define ALWAYS_INLINE inline __attribute__((always_inline))

enum {
  SERVER_TICK_SPEED = DDNET_TICK_SPEED,

  WEAPON_HAMMER = DDNET_WEAPON_HAMMER,
  WEAPON_GUN = DDNET_WEAPON_GUN,
  WEAPON_SHOTGUN = DDNET_WEAPON_SHOTGUN,
  WEAPON_GRENADE = DDNET_WEAPON_GRENADE,
  WEAPON_LASER = DDNET_WEAPON_LASER,
  WEAPON_NINJA = DDNET_WEAPON_NINJA,
  NUM_WEAPONS = DDNET_NUM_WEAPONS,

  HOOK_RETRACTED = DDNET_HOOK_RETRACTED,
  HOOK_IDLE = DDNET_HOOK_IDLE,
  HOOK_RETRACT_START = DDNET_HOOK_RETRACT_START,
  HOOK_RETRACT_END = DDNET_HOOK_RETRACT_END,
  HOOK_FLYING = DDNET_HOOK_FLYING,
  HOOK_GRABBED = DDNET_HOOK_GRABBED,

  COREEVENT_GROUND_JUMP = DDNET_COREEVENT_GROUND_JUMP,
  COREEVENT_AIR_JUMP = DDNET_COREEVENT_AIR_JUMP,
  COREEVENT_HOOK_LAUNCH = DDNET_COREEVENT_HOOK_LAUNCH,
  COREEVENT_HOOK_ATTACH_GROUND = DDNET_COREEVENT_HOOK_ATTACH_GROUND,
  COREEVENT_HOOK_HIT_NOHOOK = DDNET_COREEVENT_HOOK_HIT_NOHOOK,
  COREEVENT_HOOK_RETRACT = DDNET_COREEVENT_HOOK_RETRACT,

  INPUT_STATE_MASK = 0x3f,

  TEAM_RED = 0,
  TEAM_BLUE = 1,

  PICKUP_RESPAWN_TIME = 15, /* s */
  GRENADE_MAX_AMMO = DDNET_GRENADE_MAX_AMMO,
  SV_FREEZE_DELAY = 3,
  SV_SPAWN_RELOAD_DELAY = 8,

  /* what a frozen tee leaves out of its input */
  HELD_MOVE = 1,
  HELD_HOOK = 2,
};

#define PHYSICAL_SIZE 28.0f
#define PHYSICAL_SIZE_VEC2 ((vec2){28.0f, 28.0f})
#define PICKUP_PROXIMITY_RADIUS 14.0f

/* The game mode the tick is compiled for. */
typedef struct game_mode_t {
  bool health;  /* sv_health_and_ammo */
  bool fastcap; /* the flags, the red and blue spawns */
  bool weapons; /* grenades can exist */
} game_mode_t;

static inline int server_tick(const world_t *w) { return w->tick; }
static inline const collision_t *collision(const world_t *w) { return w->collision; }

/* The teleporter exit, out of count, see world.h. */
static inline int world_pick_tele_out(const world_t *w, int count) {
  if (count <= 1)
    return 0;
  return (int)((unsigned)w->tele_out % (unsigned)count);
}

/* CGameContext::GetPlayerChar: the living tee, NULL otherwise. */
static inline character_t *get_player_char(world_t *w) {
  return w->player.has_character && w->character.alive ? &w->character : NULL;
}

/* CEntity::GameLayerClipped */
static inline bool game_layer_clipped(const world_t *w, vec2 check_pos) {
  return round_to_int(check_pos.x) / 32 < -200 || round_to_int(check_pos.x) / 32 > collision(w)->width + 200 ||
         round_to_int(check_pos.y) / 32 < -200 || round_to_int(check_pos.y) / 32 > collision(w)->height + 200;
}

/* CountInput */
typedef struct input_count_t {
  int presses;
  int releases;
} input_count_t;

static inline input_count_t count_input(int prev, int cur) {
  /* the server counts from prev up to cur one by one: every odd value on the
   * way is a press, every even one a release */
  prev &= INPUT_STATE_MASK;
  cur &= INPUT_STATE_MASK;
  const int steps = (cur - prev) & INPUT_STATE_MASK;
  input_count_t c;
  c.presses = (steps + 1 - (prev & 1)) >> 1;
  c.releases = steps - c.presses;
  return c;
}

/* IGameController::CanSpawn of Unique Race (world.c) */
bool controller_can_spawn(world_t *w, vec2 *out_pos);
/* CPlayer::Tick (world.c) */
void player_tick(world_t *w);

/* tuning_t::velramp_powf */
enum { VELRAMP_POWF_LIBRARY, VELRAMP_POWF_SSE2, VELRAMP_POWF_FMA };
void velocity_ramp_prepare(tuning_t *tuning);
float velocity_ramp(const tuning_t *tuning, float value);

#endif
