/* Worlds: the dynamic state of a game and the functions that advance it.
 *
 * The functions are the same for both implementations. The structures are
 * not: they belong to the implementation the library was built as, see
 * backend.h. */
#ifndef DDNET_PHYSICS_WORLD_H
#define DDNET_PHYSICS_WORLD_H

#include "backend.h"
#include "collision.h"
#include "config.h"
#include "tuning.h"
#include "types.h"
#include "vmath.h"

#if defined(DDNET_PHYSICS_BACKEND_REFERENCE)
#include "reference/world.h"
#elif defined(DDNET_PHYSICS_BACKEND_OPTIMIZED)
#include "optimized/world.h"
#endif

/* Create a world for a map. The config is copied, then the settings embedded
 * in the map are applied on top of it, like a DDNet server does on map load.
 * Returns false if memory ran out. */
bool ddnet_world_init(ddnet_world_t *world, const ddnet_collision_t *collision, const ddnet_config_t *config);
void ddnet_world_free(ddnet_world_t *world);

/* Make dst an exact copy of src.
 *
 * dst must be zero initialized or a world that was initialized or copied to
 * before: its buffers are reused, so copying into the same world again and
 * again does not allocate. Returns false if memory ran out. */
bool ddnet_world_copy(ddnet_world_t *dst, const ddnet_world_t *src);

/* Brings the entities in the world's structures up to date. The optimized
 * backend does not move bullets of map shotguns that no tee is near, gun
 * bullets of a tee that is alone and laser walls that no tee is near until it
 * matters; call this before reading world->entities, for example to draw them
 * (its event build keeps them up to date). Does nothing in the reference
 * backend. */
void ddnet_world_sync(ddnet_world_t *world);

/* Tells the world that its entities, switchers, tuning or the per-player
 * tables of its draggers and turrets were written to from outside. The
 * optimized backend keeps track of what its own functions change there, so
 * that ddnet_world_copy() between two worlds that were the same before only
 * copies that; after writing to them directly, call this (the next copy is a
 * full one). Players and characters are always copied. Does nothing in the
 * reference backend. */
void ddnet_world_changed(ddnet_world_t *world);

/* Tell the world that world->tuning was changed after ddnet_world_init().
 * Tuning is read through this call only, so that implementations can keep it in
 * the form they need. */
void ddnet_world_tuning_changed(ddnet_world_t *world);

/* Advance the world by one tick (20 ms): apply players[i].input for every
 * player, then tick everything. */
void ddnet_world_tick(ddnet_world_t *world);

/* Connect a player. The tee spawns during the next tick. Returns false if the
 * client id is out of range or in use. */
bool ddnet_player_join(ddnet_world_t *world, int client_id);
/* The tee of a player that just joined (or died, and is to respawn) spawns in
 * the next tick, after the tick of the world, as on a server. This spawns it
 * right away instead, where it would have spawned then: for setting up a world
 * whose tees stand at the spawn on its first tick. Returns whether the player
 * has a tee now (false if there is no free spawn). */
bool ddnet_player_spawn(ddnet_world_t *world, int client_id);
void ddnet_player_leave(ddnet_world_t *world, int client_id);
/* Kill the tee of a player like the kill bind does, it respawns on the next tick.
 * Like on a server, this does nothing while the player is in /spec. */
void ddnet_player_kill(ddnet_world_t *world, int client_id);
/* Put a player into a ddrace team (0 = no team), without the checks of the
 * /team chat command. */
void ddnet_player_set_team(ddnet_world_t *world, int client_id, int team);
/* The direction a tee looks in, as it is sent to clients: radians * 256
 * (CCharacterCore::m_Angle). */
int ddnet_character_angle(const ddnet_character_t *chr);
/* The tee of a player, or NULL if the player has no living tee. */
ddnet_character_t *ddnet_world_character(ddnet_world_t *world, int client_id);
/* Tells the world that the tee of a player was written to from outside
 * (where it is, its speed, hook, weapons, freeze and so on, for example to put
 * it somewhere else or to show a recorded state). The optimized backend keeps
 * what it knows about where the tees are and what is near them; call this
 * after writing to a tee directly, before the next tick. Does nothing in the
 * reference backend. */
void ddnet_character_changed(ddnet_world_t *world, int client_id);
/* Where a projectile is time seconds after its start tick (CProjectile::GetPos):
 * on tick t, (t - projectile->u.projectile.start_tick) / 50.0f, which is where
 * DDNet and its clients draw it; a tick earlier for where it came from. */
ddnet_vec2_t ddnet_projectile_get_pos(const ddnet_world_t *world, const ddnet_entity_t *projectile, float time);

#if defined(DDNET_PHYSICS_BACKEND_OPTIMIZED)
/* The event build of the optimized backend (if the library was built with
 * DDNET_PHYSICS_EVENTS, the default, which defines DDNET_PHYSICS_HAS_EVENTS): the same functions, which call the
 * callbacks of the world for effects and sounds (events.h), while the ones
 * above never do and are not slowed down by them. Both work on the same
 * structures: a world can be ticked by either, and copied from one to the
 * other, for example the build above to look for or seek to a tick and this
 * one to show it, with the same events from then on as if it had been this
 * one all the time. (What a world did on the ticks of the other build made no
 * events.) */
bool ddnet_ev_world_init(ddnet_world_t *world, const ddnet_collision_t *collision, const ddnet_config_t *config);
void ddnet_ev_world_free(ddnet_world_t *world);
bool ddnet_ev_world_copy(ddnet_world_t *dst, const ddnet_world_t *src);
void ddnet_ev_world_sync(ddnet_world_t *world);
void ddnet_ev_world_changed(ddnet_world_t *world);
void ddnet_ev_world_tuning_changed(ddnet_world_t *world);
void ddnet_ev_world_tick(ddnet_world_t *world);
bool ddnet_ev_player_join(ddnet_world_t *world, int client_id);
bool ddnet_ev_player_spawn(ddnet_world_t *world, int client_id);
void ddnet_ev_player_leave(ddnet_world_t *world, int client_id);
void ddnet_ev_player_kill(ddnet_world_t *world, int client_id);
void ddnet_ev_player_set_team(ddnet_world_t *world, int client_id, int team);
int ddnet_ev_character_angle(const ddnet_character_t *chr);
ddnet_character_t *ddnet_ev_world_character(ddnet_world_t *world, int client_id);
#endif

#endif
