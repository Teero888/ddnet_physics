/* Internal interface of the reference implementation of Unique Race.
 *
 * The reference implementation is a line by line port of the Unique Race
 * server (https://github.com/unique-clan/unique-race, DDNet with the changes
 * of Unique Race on top) for a world of one player that holds only what the
 * maps of Unique Race use: every function names the C++ function
 * it comes from, keeps its statement order, and keeps every arithmetic
 * expression in its original shape and type. What is left out is what can
 * not happen there: other tees, teams, the weapons other than the hammer, the
 * gun and the grenade, and whatever no map of Unique Race has (freeze, switches,
 * specials, laser walls, ..., see ddnet_map_unsupported()). It is the readable
 * specification the optimized implementation is checked against, so clarity
 * beats speed everywhere in here.
 *
 *   Unique Race                        here
 *   ---------------------------------  ------------------
 *   CCollision                         ../ddnet_reference/collision.c, collision.c
 *   CCharacterCore                     character_core.c
 *   CCharacter                         character.c
 *   CProjectile, CPickup               entities.c
 *   IGameController, CGameControllerUnique,
 *   CGameTeams (of a team of one)      gamecontroller.c
 *   CGameWorld, CGameContext, CPlayer  world.c
 *
 * The client id of the player is 0, for the events (events.h). */
#ifndef DDNET_PHYSICS_UNIQUE_REFERENCE_INTERNAL_H
#define DDNET_PHYSICS_UNIQUE_REFERENCE_INTERNAL_H

#include "collision.h"

#include <ddnet_physics/ddnet_physics.h>

typedef ddnet_world_t world_t;
typedef ddnet_player_t player_t;
typedef ddnet_character_t character_t;
typedef ddnet_character_core_t core_t;
typedef ddnet_entity_t entity_t;
typedef ddnet_input_t input_t;
typedef ddnet_tuning_t tuning_t;

enum {
  SERVER_TICK_SPEED = DDNET_TICK_SPEED,

  /* the client id of the player, for the events */
  CLIENT_ID = 0,

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
  COREEVENT_HOOK_ATTACH_GROUND = DDNET_COREEVENT_HOOK_ATTACH_GROUND,
  COREEVENT_HOOK_HIT_NOHOOK = DDNET_COREEVENT_HOOK_HIT_NOHOOK,
  COREEVENT_HOOK_RETRACT = DDNET_COREEVENT_HOOK_RETRACT,

  INPUT_STATE_MASK = 0x3f,

  /* the TEAM_RED and TEAM_BLUE of the fastcap flags */
  TEAM_RED = 0,
  TEAM_BLUE = 1,
};

/* CCharacterCore::PhysicalSize(), also the proximity radius of a character. */
#define PHYSICAL_SIZE 28.0f
#define PHYSICAL_SIZE_VEC2 ((vec2){28.0f, 28.0f})
/* PICKUP_PHYSICS_RADIUS */
#define PICKUP_PROXIMITY_RADIUS 14.0f

/* Data from datasrc/content.py: the respawn time of the pickups (all of them
 * are 15 s) and the maxammo of the grenade. */
enum {
  PICKUP_RESPAWN_TIME = 15, /* s */
  GRENADE_MAX_AMMO = DDNET_GRENADE_MAX_AMMO,
};

/* The server settings that the maps of Unique Race leave at their defaults
 * and that matter: sv_freeze_delay (hearts) and sv_spawn_reload_delay. The
 * others are 0. */
enum {
  SV_FREEZE_DELAY = 3,
  SV_SPAWN_RELOAD_DELAY = 8,
};

/* ------------------------------------------------------------- world.c */

static inline int server_tick(const world_t *w) { return w->tick; }
static inline const collision_t *collision(const world_t *w) { return w->collision; }
static inline tuning_t *tuning_list(world_t *w) { return w->tuning; }
static inline tuning_t *global_tuning(world_t *w) { return &w->tuning[0]; }

/* Replaces CWorldCore::RandomOr0: the teleporter exit, out of count. */
int world_pick_tele_out(const world_t *w, int count);

/* CGameContext::GetPlayerChar: the living tee, NULL otherwise. */
character_t *get_player_char(world_t *w);

/* CGameWorld entity lists. An entity is addressed by its slot in
 * world->entities. */
void world_insert_entity(world_t *w, int type, int index);
void world_remove_entity(world_t *w, int type, int index);

/* Allocate an entity slot; the returned index stays valid until the entity is
 * destroyed, but pointers into world->entities do not survive this call. */
int world_new_entity(world_t *w, ddnet_entity_kind_t kind, vec2 pos);
static inline entity_t *entity(world_t *w, int index) { return &w->entities[index]; }

/* CGameWorld::FindEntities and CGameWorld::IntersectCharacter for the one
 * tee: whether it is in the world and within radius (+ its size) of pos. */
bool world_find_character(world_t *w, vec2 pos, float radius);

/* CGameContext::CreateExplosion */
void create_explosion(world_t *w, vec2 pos, int weapon);

/* The events of CGameContext (CreateSound, CreateDeath, ...), as the callbacks of the world (events.h). */
static inline void create_sound(world_t *w, vec2 pos, int sound) {
  if (w->sound && sound >= 0)
    w->sound(pos, sound, CLIENT_ID, w->user_data);
}
static inline void create_particle(world_t *w, vec2 pos, int particle) {
  if (w->particle)
    w->particle(pos, particle, CLIENT_ID, w->user_data);
}
static inline void create_damage_ind(world_t *w, vec2 pos, float angle, int amount) {
  if (w->damage_indicator)
    w->damage_indicator(pos, angle, amount, CLIENT_ID, w->user_data);
}

/* CPlayer */
void player_kill_character(world_t *w, int weapon);
void player_respawn(world_t *w);

/* CEntity helpers */
bool game_layer_clipped(const world_t *w, vec2 check_pos);

/* ---------------------------------------------------- character_core.c */

void core_reset(core_t *core);
void core_tick(world_t *w, core_t *core, bool use_input);
void core_tick_deferred(world_t *w, core_t *core);
void core_move(world_t *w, core_t *core);
void core_quantize(core_t *core);
float velocity_ramp(float value, float start, float range, float curvature);

typedef struct input_count_t {
  int presses;
  int releases;
} input_count_t;

input_count_t count_input(int prev, int cur);

/* --------------------------------------------------------- character.c */

void character_spawn(world_t *w, vec2 pos);
void character_on_predicted_input(world_t *w, character_t *chr, const input_t *new_input);
void character_on_direct_input(world_t *w, character_t *chr, const input_t *new_input);
void character_tick(world_t *w, character_t *chr);
void character_tick_deferred(world_t *w, character_t *chr);
void character_die(world_t *w, character_t *chr, int weapon);
bool character_take_damage(world_t *w, character_t *chr, vec2 force, int dmg, int weapon);

bool character_freeze(world_t *w, character_t *chr);
void character_give_weapon(world_t *w, character_t *chr, int weapon);
void character_set_weapon(world_t *w, character_t *chr, int weapon, bool silent);
bool character_increase_health(character_t *chr, int amount);
bool character_increase_armor(character_t *chr, int amount);

/* ---------------------------------------------------------- entities.c */

void entity_tick(world_t *w, int index);

void projectile_create(world_t *w, vec2 pos, vec2 dir, int span);
bool pickup_create(world_t *w, vec2 pos, int type, int subtype);

/* ---------------------------------------------------- gamecontroller.c */

bool controller_can_spawn(world_t *w, vec2 *out_pos);
void controller_on_character_spawn(world_t *w, character_t *chr);
void controller_handle_character_tiles(world_t *w, character_t *chr, int map_index, float fraction_of_tick);
void controller_set_armor_progress(world_t *w, character_t *chr, int progress);
/* CGameTeams::OnCharacterStart and CGameTeams::OnCharacterFinish, for a tee
 * that is alone in its team */
void teams_on_character_start(world_t *w, character_t *chr, float fraction_of_tick);
void teams_on_character_finish(world_t *w, character_t *chr, float fraction_of_tick);
/* Create the entities of the map (CGameContext::CreateAllEntities). */
bool controller_create_all_entities(world_t *w);

#endif
