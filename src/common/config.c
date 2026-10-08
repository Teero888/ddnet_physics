#include "console.h"

#include <stddef.h>
#include <strings.h>

/* name, default, min, max, may be set by a map (CFGFLAG_GAME) */
#define CONFIG_VARIABLES(X)                                                                                  \
  X(sv_hit, "sv_hit", 1, 0, 1, true)                                                                         \
  X(sv_endless_drag, "sv_endless_drag", 0, 0, 1, true)                                                       \
  X(sv_old_laser, "sv_old_laser", 0, 0, 1, true)                                                             \
  X(sv_old_teleport_hook, "sv_old_teleport_hook", 0, 0, 1, true)                                             \
  X(sv_old_teleport_weapons, "sv_old_teleport_weapons", 0, 0, 1, true)                                       \
  X(sv_teleport_hold_hook, "sv_teleport_hold_hook", 0, 0, 1, true)                                           \
  X(sv_teleport_lose_weapons, "sv_teleport_lose_weapons", 0, 0, 1, true)                                     \
  X(sv_deepfly, "sv_deepfly", 1, 0, 1, true)                                                                 \
  X(sv_destroy_bullets_on_death, "sv_destroy_bullets_on_death", 1, 0, 1, true)                               \
  X(sv_destroy_lasers_on_death, "sv_destroy_lasers_on_death", 0, 0, 1, true)                                 \
  X(sv_freeze_delay, "sv_freeze_delay", 3, 1, 30, true)                                                      \
  X(sv_no_weak_hook, "sv_no_weak_hook", 0, 0, 1, true)                                                       \
  X(sv_reset_pickups, "sv_reset_pickups", 0, 0, 1, true)                                                     \
  X(sv_plasma_range, "sv_plasma_range", 700, 1, 99999, true)                                                 \
  X(sv_plasma_per_sec, "sv_plasma_per_sec", 3, 0, 50, true)                                                  \
  X(sv_dragger_range, "sv_dragger_range", 700, 1, 99999, true)                                               \
  X(sv_team, "sv_team", 1, 0, 3, true)                                                                       \
  X(sv_solo_server, "sv_solo_server", 0, 0, 1, true)                                                         \
  X(sv_max_team_size, "sv_max_team_size", 128, 1, 128, true)                                                 \
  X(sv_rejoin_team_0, "sv_rejoin_team_0", 1, 0, 1, false)                                                    \
  X(sv_pauseable, "sv_pauseable", 0, 0, 1, true)                                                             \
  X(sv_pause_frequency, "sv_pause_frequency", 1, 0, 9999, false)                                             \
  X(sv_shotgun_bullet_sound, "sv_shotgun_bullet_sound", 0, 0, 1, false)

typedef struct config_variable_t {
  const char *name;
  size_t offset;
  int default_value, min, max;
  bool game;
} config_variable_t;

static const config_variable_t CONFIG_VARIABLE_LIST[] = {
#define X(member, name, def, min, max, game) {name, offsetof(ddnet_config_t, member), def, min, max, game},
    CONFIG_VARIABLES(X)
#undef X
};

enum { NUM_CONFIG_VARIABLES = sizeof(CONFIG_VARIABLE_LIST) / sizeof(CONFIG_VARIABLE_LIST[0]) };

ddnet_config_t ddnet_config_default(ddnet_gamemode_t gamemode) {
  ddnet_config_t config = {DDNET_CONFIG_DEFAULTS};
  config.gamemode = gamemode;
  for (int i = 0; i < NUM_CONFIG_VARIABLES; i++)
    *(int *)((char *)&config + CONFIG_VARIABLE_LIST[i].offset) = CONFIG_VARIABLE_LIST[i].default_value;
  return config;
}

static bool config_set(ddnet_config_t *config, const char *name, int value, bool from_map) {
  for (int i = 0; i < NUM_CONFIG_VARIABLES; i++) {
    const config_variable_t *variable = &CONFIG_VARIABLE_LIST[i];
    if (strcasecmp(name, variable->name) != 0)
      continue;
    if (from_map && !variable->game)
      return false;
    /* SIntConfigVariable::CommandCallback */
    if (variable->min != variable->max) {
      if (value < variable->min)
        value = variable->min;
      if (variable->max != 0 && value > variable->max)
        value = variable->max;
    }
    *(int *)((char *)config + variable->offset) = value;
    return true;
  }
  return false;
}

bool ddnet_config_set(ddnet_config_t *config, const char *name, int value) {
  return config_set(config, name, value, false);
}

bool ddnet_config_set_from_map(ddnet_config_t *config, const char *name, int value) {
  return config_set(config, name, value, true);
}
