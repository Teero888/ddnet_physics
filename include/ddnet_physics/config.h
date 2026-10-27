#ifndef DDNET_PHYSICS_CONFIG_H
#define DDNET_PHYSICS_CONFIG_H

#include "backend.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(DDNET_PHYSICS_GAME_DDNET)

typedef enum ddnet_gamemode_t { DDNET_MODE_DDRACE = 0, DDNET_NUM_MODES } ddnet_gamemode_t;

/* Values of ddnet_config_t::sv_team. */
enum {
  DDNET_SV_TEAM_FORBIDDEN = 0,
  DDNET_SV_TEAM_ALLOWED = 1,
  DDNET_SV_TEAM_MANDATORY = 2,   /* tees in team 0, or alone in a team, die at the start line */
  DDNET_SV_TEAM_FORCED_SOLO = 3, /* every tee is put into a team of its own */
};

/* Everything outside of the map and the tuning that changes the physics.
 *
 * The sv_* members are DDNet's server settings of the same name. A zero
 * initialized struct is NOT the default configuration, start from
 * ddnet_config_default() or use designated initializers on top of
 * DDNET_CONFIG_DEFAULTS:
 *
 *   ddnet_config_t config = {DDNET_CONFIG_DEFAULTS, .sv_freeze_delay = 1};
 */
typedef struct ddnet_config_t {
  ddnet_gamemode_t gamemode;

  int sv_hit;
  int sv_endless_drag;
  int sv_old_laser;
  int sv_old_teleport_hook;
  int sv_old_teleport_weapons;
  int sv_teleport_hold_hook;
  int sv_teleport_lose_weapons;
  int sv_deepfly;
  int sv_destroy_bullets_on_death;
  int sv_destroy_lasers_on_death;
  int sv_freeze_delay;

  int sv_no_weak_hook;
  int sv_reset_pickups;
  int sv_plasma_range;
  int sv_plasma_per_sec;
  int sv_dragger_range;
  int sv_team;
  int sv_solo_server;
  int sv_max_team_size;
  int sv_rejoin_team_0;
  int sv_pauseable;
  int sv_pause_frequency;
  /* (only for the sound of the explosive bullets of the map, see events.h) */
  int sv_shotgun_bullet_sound;

  /* Map specific compatibility behavior ("mapbug" map setting). */
  bool bug_grenade_double_explosion;
} ddnet_config_t;

#define DDNET_CONFIG_DEFAULTS                                                                                \
  .gamemode = DDNET_MODE_DDRACE, .sv_hit = 1, .sv_endless_drag = 0, .sv_old_laser = 0,                       \
  .sv_old_teleport_hook = 0, .sv_old_teleport_weapons = 0, .sv_teleport_hold_hook = 0,                       \
  .sv_teleport_lose_weapons = 0, .sv_deepfly = 1, .sv_destroy_bullets_on_death = 1,                          \
  .sv_destroy_lasers_on_death = 0, .sv_freeze_delay = 3, .sv_no_weak_hook = 0, .sv_reset_pickups = 0,        \
  .sv_plasma_range = 700, .sv_plasma_per_sec = 3, .sv_dragger_range = 700, .sv_team = DDNET_SV_TEAM_ALLOWED, \
  .sv_solo_server = 0, .sv_max_team_size = 128, .sv_rejoin_team_0 = 1, .sv_pauseable = 0,                    \
  .sv_pause_frequency = 1, .sv_shotgun_bullet_sound = 0, .bug_grenade_double_explosion = false

#elif defined(DDNET_PHYSICS_GAME_UNIQUE)

/* The game modes of Unique Race (sv_gametype). Shorts is race. */
typedef enum ddnet_gamemode_t {
  DDNET_MODE_UNIQUE_RACE = 0,
  DDNET_MODE_UNIQUE_FASTCAP,
  /* fastcap with sv_no_weapons 1: no grenades anywhere */
  DDNET_MODE_UNIQUE_FASTCAP_NO_WEAPONS,
  DDNET_NUM_MODES
} ddnet_gamemode_t;

/* Everything outside of the map and the tuning that changes the physics of
 * Unique Race: the server settings of the same name that its maps set. (The
 * other settings of its servers are at their defaults, and its maps do not
 * change them.)
 *
 * Fastcap has both settings on, whatever they are set to. After
 * ddnet_world_init(), world->config has that applied, like the map settings.
 *
 * A zero initialized struct is the default configuration of race; start from
 * ddnet_config_default() for the other modes, or set the gamemode. */
typedef struct ddnet_config_t {
  ddnet_gamemode_t gamemode;

  /* Tees take damage from their grenades and die of it, have 10 grenades,
   * and hearts, shields and grenades give health, armor and ammo (and are
   * gone for 15 seconds after) instead of freezing and taking the grenade. */
  int sv_health_and_ammo;
  /* The grenades of a tee are gone when it dies (sv_destroy_bullets_on_death). */
  int sv_kill_grenades;
} ddnet_config_t;

#define DDNET_CONFIG_DEFAULTS .gamemode = DDNET_MODE_UNIQUE_RACE, .sv_health_and_ammo = 0, .sv_kill_grenades = 0

#endif

ddnet_config_t ddnet_config_default(ddnet_gamemode_t gamemode);

/* Set a server setting by its console name ("sv_hit", "sv_freeze_delay", ...),
 * clamped to its valid range. Returns false for unknown settings. */
bool ddnet_config_set(ddnet_config_t *config, const char *name, int value);

#endif
