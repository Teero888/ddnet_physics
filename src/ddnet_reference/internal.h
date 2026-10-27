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
#ifndef DDNET_PHYSICS_REFERENCE_INTERNAL_H
#define DDNET_PHYSICS_REFERENCE_INTERNAL_H

#include "collision.h"

#include <ddnet_physics/ddnet_physics.h>

typedef ddnet_world_t world_t;
typedef ddnet_player_t player_t;
typedef ddnet_character_t character_t;
typedef ddnet_character_core_t core_t;
typedef ddnet_entity_t entity_t;
typedef ddnet_input_t input_t;
typedef ddnet_tuning_t tuning_t;
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
static inline tuning_t *tuning_list(world_t *w) { return w->tuning; }
static inline tuning_t *global_tuning(world_t *w) { return &w->tuning[0]; }

/* Replaces CWorldCore::RandomOr0: the teleporter exit, out of count, that
 * something owned by a player (or by nobody, -1) takes. */
int world_pick_tele_out(const world_t *w, int client_id, int count);

/* CWorldCore::m_apCharacters[client_id]: the core of a tee that takes part in
 * the physics, NULL otherwise. */
core_t *world_core(world_t *w, int client_id);
/* CGameContext::GetPlayerChar: the living tee of a player, NULL otherwise. */
character_t *get_player_char(world_t *w, int client_id);
static inline int character_id(const character_t *chr) { return chr->core.id; }

/* CGameWorld entity lists. An entity is addressed by its list and its index:
 * the client id for characters, the slot in world->entities otherwise. */
void world_insert_entity(world_t *w, int type, int index);
void world_remove_entity(world_t *w, int type, int index);
void world_remove_entities_from_player(world_t *w, int player_id);

/* Allocate an entity slot; the returned index stays valid until the entity is
 * destroyed, but pointers into world->entities do not survive this call. */
int world_new_entity(world_t *w, ddnet_entity_kind_t kind, vec2 pos);
static inline entity_t *entity(world_t *w, int index) { return &w->entities[index]; }

/* CGameWorld::FindEntities for characters: client ids, in list order. */
int world_find_characters(world_t *w, vec2 pos, float radius, int *ids, int max);
character_t *world_intersect_character(world_t *w, vec2 pos0, vec2 pos1, float radius, vec2 *new_pos,
                                       const character_t *not_this, int collide_with,
                                       const character_t *this_only);
void world_release_hooked(world_t *w, int client_id);

/* CGameContext::CreateExplosion */
void create_explosion(world_t *w, vec2 pos, int owner, int weapon, bool no_damage, int activated_team);

/* The events of CGameContext (CreateSound, CreateDeath, ...), as the callbacks of the world (events.h). */
static inline void create_sound(world_t *w, vec2 pos, int sound, int client_id) {
  if (w->sound && sound >= 0) /* (CreateSound ignores a sound below 0, which a bullet has for its impact) */
    w->sound(pos, sound, client_id, w->user_data);
}
static inline void create_particle(world_t *w, vec2 pos, int particle, int client_id) {
  if (w->particle)
    w->particle(pos, particle, client_id, w->user_data);
}
static inline void create_damage_ind(world_t *w, vec2 pos, float angle, int amount, int client_id) {
  if (w->damage_indicator)
    w->damage_indicator(pos, angle, amount, client_id, w->user_data);
}

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
void core_tick(world_t *w, core_t *core, bool use_input, bool do_deferred_tick);
void core_tick_deferred(world_t *w, core_t *core);
void core_move(world_t *w, core_t *core);
void core_quantize(world_t *w, core_t *core);
void core_set_hooked_player(world_t *w, core_t *core, int hooked_player);
float velocity_ramp(float value, float start, float range, float curvature);

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
bool teams_can_collide(const world_t *w, int client_id1, int client_id2);
bool teams_get_solo(const world_t *w, int client_id);

/* ---------------------------------------------------- gamecontroller.c */

bool controller_can_spawn(world_t *w, vec2 *out_pos, int client_id);
void controller_on_character_spawn(world_t *w, character_t *chr);
void controller_handle_character_tiles(world_t *w, character_t *chr, int map_index);
void controller_set_armor_progress(character_t *chr, int progress);
/* Create the entities of the map (CGameContext::CreateAllEntities). */
bool controller_create_all_entities(world_t *w);

#endif
