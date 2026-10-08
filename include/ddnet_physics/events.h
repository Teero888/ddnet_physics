/* Effects and sounds: what the game makes happen besides its state, for
 * whoever shows it (a renderer, a demo writer).
 *
 * A world calls three callbacks, which are fields of the world in both
 * backends (ddnet_world_t::sound and so on, with ddnet_world_t::user_data):
 * the reference backend always, the optimized backend only in its event build
 * (the functions prefixed ddnet_ev_, see the main README). They are called in
 * the middle of the tick, in the order DDNet creates the events (the optimized
 * backend has a few of them in another order within a tick, see its README),
 * and may not change the world.
 *
 * What is called is what DDNet's server creates (CGameContext::CreateSound,
 * CreateExplosion, CreateDeath and so on), wherever it creates it and with
 * the same position, regardless of who would receive it (no team masks).
 * Besides those, two effects that a client makes itself from what it sees:
 * the smoke of grenades and the double jump (DDNET_PARTICLE_SMOKE and
 * DDNET_PARTICLE_AIR_JUMP). Sounds that a client plays for an effect, those of
 * a hammer hit, a spawn and a double jump (DDNET_SOUND_HAMMER_HIT,
 * DDNET_SOUND_PLAYER_SPAWN, DDNET_SOUND_PLAYER_AIRJUMP), are left to the
 * caller, as in DDNet. Not
 * called: the damage indicators of the freeze and ninja countdowns, which DDNet
 * only sends to clients without its newer HUD. */
#ifndef DDNET_PHYSICS_EVENTS_H
#define DDNET_PHYSICS_EVENTS_H

#include "vmath.h"

/* The sounds, numbered like DDNet's network protocol (SOUND_*). */
typedef enum {
  DDNET_SOUND_GUN_FIRE = 0,
  DDNET_SOUND_SHOTGUN_FIRE,
  DDNET_SOUND_GRENADE_FIRE,
  DDNET_SOUND_HAMMER_FIRE,
  DDNET_SOUND_HAMMER_HIT,
  DDNET_SOUND_NINJA_FIRE,
  DDNET_SOUND_GRENADE_EXPLODE,
  DDNET_SOUND_NINJA_HIT,
  DDNET_SOUND_LASER_FIRE,
  DDNET_SOUND_LASER_BOUNCE,
  DDNET_SOUND_WEAPON_SWITCH,
  DDNET_SOUND_PLAYER_PAIN_SHORT,
  DDNET_SOUND_PLAYER_PAIN_LONG,
  DDNET_SOUND_BODY_LAND,
  DDNET_SOUND_PLAYER_AIRJUMP,
  DDNET_SOUND_PLAYER_JUMP,
  DDNET_SOUND_PLAYER_DIE,
  DDNET_SOUND_PLAYER_SPAWN,
  DDNET_SOUND_PLAYER_SKID,
  DDNET_SOUND_TEE_CRY,
  DDNET_SOUND_HOOK_LOOP,
  DDNET_SOUND_HOOK_ATTACH_GROUND,
  DDNET_SOUND_HOOK_ATTACH_PLAYER,
  DDNET_SOUND_HOOK_NOATTACH,
  DDNET_SOUND_PICKUP_HEALTH,
  DDNET_SOUND_PICKUP_ARMOR,
  DDNET_SOUND_PICKUP_GRENADE,
  DDNET_SOUND_PICKUP_SHOTGUN,
  DDNET_SOUND_PICKUP_NINJA,
  DDNET_SOUND_WEAPON_SPAWN,
  DDNET_SOUND_WEAPON_NOAMMO,
  DDNET_SOUND_HIT,
} ddnet_sound_t;

/* The effects. The numbers are the ones of the earlier ddnet_physics_c. */
typedef enum {
  DDNET_PARTICLE_PLAYER_SPAWN = 0, /* CreatePlayerSpawn: a tee spawns, or comes back from /spec */
  DDNET_PARTICLE_PLAYER_DEATH,     /* CreateDeath: a tee dies, goes to /spec, or is teleported by its gun */
  DDNET_PARTICLE_SMOKE,            /* the client's trail of a grenade: on every tick, where it was a tick ago */
  DDNET_PARTICLE_BULLET_TRAIL,     /* (not called) */
  DDNET_PARTICLE_BULLET_STARS,     /* (not called) */
  DDNET_PARTICLE_HAMMER_HIT,       /* CreateHammerHit */
  DDNET_PARTICLE_EXPLOSION,        /* CreateExplosion: client_id is the owner of what exploded, -1 for the map */
  DDNET_PARTICLE_AIR_JUMP,         /* the client's double jump effect */
  DDNET_PARTICLE_CONFETTI,         /* CreateFinishEffect: a tee finished */
} ddnet_particle_t;

/* client_id is the tee the event is about (or the owner of the projectile, laser or plasma), -1 if none. */
typedef void (*ddnet_sound_fn)(ddnet_vec2_t pos, int sound, int client_id, void *user_data);
typedef void (*ddnet_particle_fn)(ddnet_vec2_t pos, int particle, int client_id, void *user_data);
/* CreateDamageInd: amount stars around angle (radians; a bullet's hit, amount 10) */
typedef void (*ddnet_damage_indicator_fn)(ddnet_vec2_t pos, float angle, int amount, int client_id,
                                          void *user_data);

#endif
