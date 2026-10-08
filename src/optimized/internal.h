/* Internal interface of the reference implementation.
 *
 * The reference implementation is a line by line port of DDNet's game server:
 * every function names the C++ function it comes from, keeps its statement
 * order, and keeps every arithmetic expression in its original shape and type.
 * It is the readable specification the optimized implementation is checked
 * against, so clarity beats speed everywhere in here.
 *
 *   DDNet                              here
 *   ---------------------------------  ------------------
 *   CCollision                         collision.c
 *   CCharacterCore                     character_core.c
 *   CCharacter                         character.c
 *   CProjectile, CLaser, CPickup, ...  entities.c
 *   CGameTeams, CTeamsCore             teams.c
 *   IGameController, gamemodes         gamecontroller.c
 *   CGameWorld, CGameContext, CPlayer  world.c
 */
#ifndef DDNET_PHYSICS_OPTIMIZED_INTERNAL_H
#define DDNET_PHYSICS_OPTIMIZED_INTERNAL_H

#include "collision.h"

#include <ddnet_physics/ddnet_physics.h>

#include <stddef.h>
#include <string.h>

/* yes ? a : b for yes being 0 or 1, as arithmetic: where the answer follows
 * from the input, a branch would be mispredicted half of the time, and the
 * compiler makes one out of the ?: of C more often than not. */
static inline int select_int(int yes, int a, int b) { return b ^ ((a ^ b) & -yes); }

/* Cycle counts per stage of the tick, for -DDDNET_PHYSICS_PROFILE builds. */
#ifdef DDNET_PHYSICS_PROFILE
#include <stdio.h>
#include <x86intrin.h>
enum {
  PROF_DIRECT_INPUT,
  PROF_PREDICTED_INPUT,
  PROF_PROJECTILES,
  PROF_OTHER_ENTITIES,
  PROF_DDRACE_TICK,
  PROF_CORE_TICK,
  PROF_WEAPONS,
  PROF_POST_CORE,
  PROF_MOVE,
  PROF_QUANTIZE,
  PROF_PLAYERS,
  PROF_NUM
};
extern unsigned long long ddnet_prof_cycles[PROF_NUM], ddnet_prof_calls[PROF_NUM];
#define PROF_BEGIN(name) const unsigned long long prof_start_##name = __rdtsc()
#define PROF_END(name) (ddnet_prof_cycles[name] += __rdtsc() - prof_start_##name, ddnet_prof_calls[name]++)
#else
#define PROF_BEGIN(name) ((void)0)
#define PROF_END(name) ((void)0)
#endif

typedef ddnet_world_t world_t;
typedef ddnet_player_t player_t;
typedef ddnet_character_t character_t;
typedef ddnet_character_core_t core_t;
typedef ddnet_entity_t entity_t;
typedef ddnet_input_t input_t;
/* What the physics read: the float values, not the stored integers. */
typedef ddnet_tuning_values_t tuning_t;
#define ddnet_tune(value) (value)
typedef ddnet_teams_t teams_t;

enum {
  MAX_CLIENTS = DDNET_MAX_CLIENTS,
  TEAM_FLOCK = DDNET_TEAM_FLOCK,
  NUM_DDRACE_TEAMS = DDNET_NUM_TEAMS,

  SERVER_TICK_SPEED = DDNET_TICK_SPEED,

  WEAPON_HAMMER = DDNET_WEAPON_HAMMER,
  WEAPON_GUN = DDNET_WEAPON_GUN,
  WEAPON_SHOTGUN = DDNET_WEAPON_SHOTGUN,
  WEAPON_GRENADE = DDNET_WEAPON_GRENADE,
  WEAPON_LASER = DDNET_WEAPON_LASER,
  WEAPON_NINJA = DDNET_WEAPON_NINJA,
  NUM_WEAPONS = DDNET_NUM_WEAPONS,
  WEAPON_GAME = DDNET_WEAPON_GAME,
  WEAPON_SELF = DDNET_WEAPON_SELF,
  WEAPON_WORLD = DDNET_WEAPON_WORLD,

  HOOK_RETRACTED = DDNET_HOOK_RETRACTED,
  HOOK_IDLE = DDNET_HOOK_IDLE,
  HOOK_RETRACT_START = DDNET_HOOK_RETRACT_START,
  HOOK_RETRACT_END = DDNET_HOOK_RETRACT_END,
  HOOK_FLYING = DDNET_HOOK_FLYING,
  HOOK_GRABBED = DDNET_HOOK_GRABBED,

  COREEVENT_GROUND_JUMP = DDNET_COREEVENT_GROUND_JUMP,
  COREEVENT_AIR_JUMP = DDNET_COREEVENT_AIR_JUMP,
  COREEVENT_HOOK_LAUNCH = DDNET_COREEVENT_HOOK_LAUNCH,
  COREEVENT_HOOK_ATTACH_PLAYER = DDNET_COREEVENT_HOOK_ATTACH_PLAYER,
  COREEVENT_HOOK_ATTACH_GROUND = DDNET_COREEVENT_HOOK_ATTACH_GROUND,
  COREEVENT_HOOK_HIT_NOHOOK = DDNET_COREEVENT_HOOK_HIT_NOHOOK,
  COREEVENT_HOOK_RETRACT = DDNET_COREEVENT_HOOK_RETRACT,

  INPUT_STATE_MASK = 0x3f,
};

/* CCharacterCore::PhysicalSize(), also the proximity radius of a character. */
#define PHYSICAL_SIZE 28.0f
#define PHYSICAL_SIZE_VEC2 ((vec2){28.0f, 28.0f})
/* PICKUP_PHYSICS_RADIUS */
#define PICKUP_PROXIMITY_RADIUS 14.0f

/* Weapon data from DDNet's datasrc/content.py. */
enum {
  HAMMER_DAMAGE = 3,
  NINJA_DAMAGE = 9,
  NINJA_DURATION = 15000, /* ms */
  NINJA_MOVETIME = 200,   /* ms */
  NINJA_VELOCITY = 50,
};

/* ------------------------------------------------------------- world.c */

static inline int server_tick(const world_t *w) { return w->tick; }
static inline const collision_t *collision(const world_t *w) { return w->collision; }
static inline tuning_t *tuning_list(world_t *w) { return w->tuning_values; }
static inline tuning_t *global_tuning(world_t *w) { return &w->tuning_values[0]; }
/* The tuning of the zone a tee is in (CCharacterCore::m_Tuning). */
static inline const tuning_t *core_tuning(const world_t *w, const core_t *core) {
  const character_t *chr = (const character_t *)((const char *)core - offsetof(character_t, core));
  return &w->tuning_values[chr->tune_zone];
}

/* Replaces CWorldCore::RandomOr0: the teleporter exit, out of count, that
 * something owned by a player (or by nobody, -1) takes. */
int world_pick_tele_out(const world_t *w, int client_id, int count);

/* CWorldCore::m_apCharacters[client_id]: the core of a tee that takes part in
 * the physics, NULL otherwise. */
core_t *world_core(world_t *w, int client_id);
void world_core_add(world_t *w, int client_id);
void world_core_remove(world_t *w, int client_id);

/* The positions of the tees during a loop of the tick over them, for the loops over pairs of tees
 * (core_tees_in()), which can then look at four at once. In such a loop only the tee it is at moves (its
 * move, a teleporter or its gun take it somewhere), and nothing else: after it, its position here is
 * updated (snapshot_moved()). The tees are core_ids when the loop began: a tee that enters or leaves the
 * world drops the snapshot (world_core_add(), world_core_remove()). */
typedef struct ddnet_tee_snapshot {
  int num;
  uint8_t ids[DDNET_MAX_CLIENTS];
  uint8_t slot[DDNET_MAX_CLIENTS]; /* client id -> index, 0xff for none */
  float x[DDNET_MAX_CLIENTS] __attribute__((aligned(32)));
  float y[DDNET_MAX_CLIENTS] __attribute__((aligned(32)));
} tee_snapshot_t;
/* The snapshot for a loop, if there are enough tees for it to pay off (with fewer, the scans of the
 * tees themselves take less time than taking it). */
enum { SNAPSHOT_MIN_TEES = 6 };
static inline void snapshot_take(world_t *w, tee_snapshot_t *s) {
  if (w->num_cores < SNAPSHOT_MIN_TEES)
    return;
  s->num = w->num_cores;
  memset(s->slot, 0xff, sizeof(s->slot));
  for (int n = 0; n < s->num; n++) {
    const int id = w->core_ids[n];
    s->ids[n] = (uint8_t)id;
    s->slot[id] = (uint8_t)n;
    s->x[n] = w->characters[id].core.pos.x;
    s->y[n] = w->characters[id].core.pos.y;
  }
  /* (the rest of the last eight, which are looked at together) */
  for (int n = s->num; n & 7; n++)
    s->x[n] = s->y[n] = 0.0f;
  w->tee_snapshot = s;
}
/* the loop is done with the tee of client_id */
static inline void snapshot_moved(world_t *w, int client_id) {
  tee_snapshot_t *s = w->tee_snapshot;
  if (s && s->slot[client_id] != 0xff) {
    s->x[s->slot[client_id]] = w->characters[client_id].core.pos.x;
    s->y[s->slot[client_id]] = w->characters[client_id].core.pos.y;
  }
}
void world_switch_timer_started(world_t *w, int number, int team);
/* CGameContext::GetPlayerChar: the living tee of a player, NULL otherwise. */
character_t *get_player_char(world_t *w, int client_id);
static inline int character_id(const character_t *chr) { return chr->core.id; }

/* CGameWorld entity lists. An entity is addressed by its list and its index:
 * the client id for characters, the slot in world->entities otherwise. */
void world_insert_entity(world_t *w, int type, int index);
void world_remove_entity(world_t *w, int type, int index);
void world_free_entity(world_t *w, int index);
void world_remove_entities_from_player(world_t *w, int player_id);

/* Allocate an entity slot; the returned index stays valid until the entity is
 * destroyed, but pointers into world->entities do not survive this call. */
int world_new_entity(world_t *w, ddnet_entity_kind_t kind, vec2 pos);

/* CGameWorld::FindEntities for characters: client ids, in list order. */
int world_find_characters(world_t *w, vec2 pos, float radius, int *ids, int max);
character_t *world_intersect_character(world_t *w, vec2 pos0, vec2 pos1, float radius, vec2 *new_pos,
                                       const character_t *not_this, int collide_with,
                                       const character_t *this_only);
void world_release_hooked(world_t *w, int client_id);

/* CGameContext::CreateExplosion */
void create_explosion(world_t *w, vec2 pos, int owner, int weapon, bool no_damage, int activated_team);

/* The events of CGameContext (CreateSound, CreateDeath, ...) as the callbacks of the world (events.h), in
 * the event build (DDNET_PHYSICS_EVENTS, unity_events.c). In the other one they are nothing, and their
 * arguments are not evaluated. */
#ifdef DDNET_PHYSICS_EVENTS
enum { EVENT_BUILD = 1 };
static inline void emit_sound(world_t *w, vec2 pos, int sound, int client_id) {
  if (w->sound && sound >= 0) /* (CreateSound ignores a sound below 0, which a bullet has for its impact) */
    w->sound(pos, sound, client_id, w->user_data);
}
static inline void emit_particle(world_t *w, vec2 pos, int particle, int client_id) {
  if (w->particle)
    w->particle(pos, particle, client_id, w->user_data);
}
static inline void emit_damage_ind(world_t *w, vec2 pos, float angle, int amount, int client_id) {
  if (w->damage_indicator)
    w->damage_indicator(pos, angle, amount, client_id, w->user_data);
}
#define EMIT_SOUND(w, pos, sound, client_id) emit_sound(w, pos, sound, client_id)
#define EMIT_PARTICLE(w, pos, particle, client_id) emit_particle(w, pos, particle, client_id)
#define EMIT_DAMAGE_IND(w, pos, angle, amount, client_id) emit_damage_ind(w, pos, angle, amount, client_id)
#else
enum { EVENT_BUILD = 0 };
#define EMIT_SOUND(w, pos, sound, client_id) ((void)0)
#define EMIT_PARTICLE(w, pos, particle, client_id) ((void)0)
#define EMIT_DAMAGE_IND(w, pos, angle, amount, client_id) ((void)0)
#endif

/* CPlayer */
void player_kill_character(world_t *w, int client_id, int weapon);
void player_respawn(world_t *w, int client_id, bool weak_hook);
/* CPlayer::Pause */
int player_pause(world_t *w, int client_id, int state, bool force);

/* CCharacter::Pause: take a tee out of the world for /spec and put it back */
void character_pause(world_t *w, character_t *chr, bool pause);
/* CCharacter::IsGrounded */
bool character_is_grounded(const world_t *w, const character_t *chr);

/* CEntity helpers */
bool game_layer_clipped(const world_t *w, vec2 check_pos);
bool get_nearest_air_pos(const world_t *w, vec2 pos, vec2 prev_pos, vec2 *out_pos);
bool get_nearest_air_pos_player(const world_t *w, vec2 player_pos, vec2 *out_pos);

static inline ddnet_switcher_t *switchers(world_t *w) { return w->switchers; }

/* ---------------------------------------------------- character_core.c */

void core_reset(world_t *w, core_t *core);
/* Looks up what is known about where a tee is (here_tile, here_flags, quiet). */
static inline void character_here_refresh(const collision_t *col, character_t *chr) {
  /* collision_tile_at_pos() and collision_flags_at(), with what they have in common */
  const vec2 pos = chr->pos;
  if (pos.x >= 0 && pos.y >= 0 && pos.x < col->units_width && pos.y < col->units_height) {
    chr->here_on_map = true;
    chr->here_tile = ((int)pos.y >> 5) * col->width + ((int)pos.x >> 5);
    chr->here_flags = col->tile_flags[chr->here_tile];
  } else {
    chr->here_on_map = false;
    chr->here_tile = collision_tile_at_pos(col, pos);
    chr->here_flags = col->tile_flags[chr->here_tile] & ~(DDNET_TILEFLAG_QUIET | DDNET_TILEFLAG_INERT);
  }
  chr->quiet = (chr->here_flags & DDNET_TILEFLAG_QUIET) != 0;
  chr->quiet_pos = chr->pos;
  chr->quiet_known = true;
}

/* collision_is_quiet() for where a tee is, which is asked several times per
 * tick: looked up again if the tee is somewhere else than the last time
 * (compared as bits: the same bits are the same position). */
static inline bool character_is_quiet(const collision_t *col, character_t *chr) {
  uint64_t now, known;
  memcpy(&now, &chr->pos, sizeof(now));
  memcpy(&known, &chr->quiet_pos, sizeof(known));
  if (!chr->quiet_known || now != known)
    character_here_refresh(col, chr);
  return chr->quiet;
}

uint64_t world_new_universe(void);

/* What ddnet_world_copy() has to copy again, see ddnet_world_t::touch_log. */
enum {
  TOUCH_SWITCHER = 1,
  TOUCH_DRAGGER_TARGETS,
  TOUCH_GUN_TIMERS,
  TOUCH_ENTITY,
  TOUCH_PARKED /* the whole list */
};
enum { TOUCH_LOG_SIZE = 1024 };
void world_touch_reset(world_t *w);
static inline void world_touch(world_t *w, unsigned kind, int index) {
  const uint32_t entry = kind << 24 | (unsigned)index;
  /* The same again right after is not noted again. (A world that was copied
   * from this one in between only looks at what was noted after that, and
   * would miss it: which is why it also takes the one entry before.) */
  if (w->touch_count > 0 && w->touch_log[w->touch_count - 1] == entry)
    return;
  if (!w->touch_log || w->touch_count >= TOUCH_LOG_SIZE)
    world_touch_reset(w);
  if (w->touch_log)
    w->touch_log[w->touch_count++] = entry;
}

/* An entity, to write to: it is noted for ddnet_world_copy(). */
static inline entity_t *entity(world_t *w, int index) {
  world_touch(w, TOUCH_ENTITY, index);
  return &w->entities[index];
}
/* An entity, to look at. */
static inline const entity_t *entity_peek(const world_t *w, int index) { return &w->entities[index]; }
void core_tick(world_t *w, core_t *core, bool use_input, bool do_deferred_tick, int flags);
void core_tick_deferred(world_t *w, core_t *core);
float velocity_ramp(const tuning_t *tuning, float value);
/* The start of CCharacterCore::Move: the velocity ramp, which multiplies vel.x by *ramp_value. False if
 * the speed is below its start, with nothing done (and a factor of 1). */
/* (the factor for a velocity, see below) */
static inline bool ramp_of(vec2 vel, const tuning_t *tuning, float *ramp_value) {
  /* Below the start of the ramp the factor is 1, which changes nothing. The
   * comparison is the one of VelocityRamp(), see velramp_start_squared. */
  *ramp_value = 1.0f;
  if (vdot(vel, vel) < tuning->velramp_start_squared)
    return false;
  *ramp_value = velocity_ramp(tuning, vlength(vel) * 50);
  return true;
}
static inline bool core_ramp(core_t *core, const tuning_t *tuning, float *ramp_value) {
  const bool ramp = ramp_of(core->vel, tuning, ramp_value);
  if (ramp)
    core->vel.x = core->vel.x * *ramp_value;
  return ramp;
}
/* The other tees the move of a core can stop at (after core_ramp()), into near: none if the move only has
 * to round like DDNet's. */
int core_move_tees(world_t *w, const core_t *core, const tuning_t *tuning, int *near);
/* The rest of CCharacterCore::Move, with the ramp and the tees of the two functions above */
void core_move(world_t *w, core_t *core, const tuning_t *tuning, bool ramp, float ramp_value, const int *near,
               int num_near);
void core_quantize(world_t *w, core_t *core);
void core_set_hooked_player(world_t *w, core_t *core, int hooked_player);

/* tuning_t::velramp_powf */
enum { VELRAMP_POWF_LIBRARY, VELRAMP_POWF_SSE2, VELRAMP_POWF_FMA };
void velocity_ramp_prepare(tuning_t *tuning);

typedef struct input_count_t {
  int presses;
  int releases;
} input_count_t;

input_count_t count_input(int prev, int cur);

/* --------------------------------------------------------- character.c */

void character_spawn(world_t *w, int client_id, vec2 pos);
void character_on_predicted_input(world_t *w, character_t *chr, const input_t *new_input);
void character_on_direct_input(world_t *w, character_t *chr, const input_t *new_input);
void character_pre_tick(world_t *w, character_t *chr);
void character_tick(world_t *w, character_t *chr);
void character_tick_deferred(world_t *w, character_t *chr);
void character_die(world_t *w, character_t *chr, int killer, int weapon);
bool character_take_damage(world_t *w, character_t *chr, vec2 force, int dmg, int from, int weapon);

int character_team(const world_t *w, const character_t *chr);
bool character_can_collide(const world_t *w, const character_t *chr, int client_id);
void character_set_solo(world_t *w, character_t *chr, bool solo);
bool character_freeze_seconds(world_t *w, character_t *chr, int seconds);
bool character_freeze(world_t *w, character_t *chr);
bool character_unfreeze(world_t *w, character_t *chr);
void character_give_weapon(world_t *w, character_t *chr, int weapon, bool remove);
void character_give_ninja(world_t *w, character_t *chr);
void character_reset_pickups(character_t *chr);
void character_release_hook(world_t *w, character_t *chr);
void character_set_velocity(character_t *chr, vec2 new_velocity);
void character_set_raw_velocity(character_t *chr, vec2 new_velocity);
void character_add_velocity(character_t *chr, vec2 addition);
void character_apply_move_restrictions(character_t *chr);
bool character_increase_health(character_t *chr, int amount);

/* ---------------------------------------------------------- entities.c */

void entity_tick(world_t *w, int index);
void projectile_unschedule(world_t *w, int index);
void projectile_set_erase(world_t *w, int index);
void projectiles_sync(world_t *w);
void lights_sync(world_t *w);
void projectiles_forget_orbits(world_t *w, int until, bool all);
void pickups_tick_static(world_t *w);
void projectiles_tick(world_t *w);
/* CEntity::m_MarkedForDestroy = true */
static inline void entity_mark_for_destroy(world_t *w, entity_t *ent) {
  if (ent->marked_for_destroy)
    return;
  ent->marked_for_destroy = true;
  w->entities_marked = true;
  if (w->num_marked_entities < DDNET_MAX_MARKED_ENTITIES)
    w->marked_entities[w->num_marked_entities] = (int)(ent - w->entities);
  w->num_marked_entities++;
}
int entity_owner_id(const entity_t *ent);

void projectile_create(world_t *w, int type, int owner, vec2 pos, vec2 dir, int span, bool freeze,
                       bool explosive, int layer, int number, int bouncing);
void laser_create(world_t *w, vec2 pos, vec2 direction, float start_energy, int owner, int type);
void pickup_create(world_t *w, vec2 pos, int type, int subtype, int layer, int number);
bool dragger_create(world_t *w, vec2 pos, float strength, bool ignore_walls, int layer, int number);
bool gun_create(world_t *w, vec2 pos, bool freeze, bool explosive, int layer, int number);
int light_create(world_t *w, vec2 pos, float rotation, int length, int layer, int number);
void light_step(world_t *w, int index);

/* ------------------------------------------------------------- teams.c */

void teams_reset(world_t *w);
void teams_reset_switchers(world_t *w, int team);
void teams_on_character_start(world_t *w, int client_id);
void teams_on_character_finish(world_t *w, int client_id);
void teams_on_character_spawn(world_t *w, int client_id);
void teams_on_character_death(world_t *w, int client_id, int weapon);
void teams_tick(world_t *w);
void teams_set_force_character_team(world_t *w, int client_id, int team);
void teams_check_team_finished(world_t *w, int team);
int teams_team_size(const world_t *w, int team);
bool teams_team_locked(const world_t *w, int team);
void teams_set_team_lock(world_t *w, int team, bool lock);
bool teams_is_valid_team_number(int team);

/* CTeamsCore */
static inline int teams_team(const world_t *w, int client_id) { return w->teams.team[client_id]; }
bool teams_can_keep_hook(const world_t *w, int client_id1, int client_id2);
/* CGameTeams::CanCollide (inline: the loops over pairs of tees ask it for every pair) */
static inline bool teams_can_collide(const world_t *w, int client_id1, int client_id2) {
  const int *team = w->teams.team;
  if (client_id1 == client_id2)
    return true;
  if (w->teams.is_solo[client_id1] || w->teams.is_solo[client_id2])
    return false;
  return team[client_id1] == team[client_id2];
}
bool teams_get_solo(const world_t *w, int client_id);

/* ---------------------------------------------------- gamecontroller.c */

bool controller_can_spawn(world_t *w, vec2 *out_pos, int client_id);
void controller_on_character_spawn(world_t *w, character_t *chr);
void controller_handle_character_tiles(world_t *w, character_t *chr, int map_index);
void controller_set_armor_progress(character_t *chr, int progress);
/* Create the entities of the map (CGameContext::CreateAllEntities). */
bool controller_create_all_entities(world_t *w);

#endif
