/* What the Unique backends support of a map: what the maps of Unique Race
 * use, and nothing else.
 *
 * That is everything of the physics in the 642 maps of
 * https://github.com/unique-clan/unique-maps (556 race, 43 fastcap, 43
 * fastcap without weapons, as of 2026-10-24):
 *
 *   game, front  solid, death, unhookable, hookthrough (the shortcut,
 *                TILE_THROUGH_CUT), start, finish, time checkpoints, the
 *                three stoppers; spawns, flag stands, hearts, shields and
 *                weapons (of which Unique Race keeps the grenade)
 *   tele         teleporters, evil teleporters, checkpoint teleporters
 *   speedup      the old speedups
 *   tune         tune zones
 *   settings     sv_kill_grenades, sv_health_and_ammo, tune_zone
 *
 * Tiles that do nothing in Unique Race (stray tiles of other layers, time
 * checkpoints in the front layer, ...) are fine. So are weapon teleporters,
 * which the backends ignore: the only map that has them (run_crossover) has
 * them behind its finish line. A map with anything else
 * that would change the physics is refused, because the backends would get it
 * wrong: freeze, the other hookthrough tiles, the tiles of DDNet's specials,
 * hook teleporters, the new speedups, a switch layer (and so doors), laser
 * walls, draggers, turrets, and the map settings that change what the
 * backends take as fixed. */
#include "console.h"

#include <ddnet_map_loader.h>
#include <ddnet_physics/collision.h>

#include <stdlib.h>
#include <strings.h>

/* A tile of the game or front layer that Unique Race reacts to and the
 * backends do not have: NULL if there is none. */
static const char *unsupported_tile(int index) {
  if (index > ENTITY_OFFSET) {
    const int entity = index - ENTITY_OFFSET;
    if (entity >= ENTITY_SPAWN && entity <= ENTITY_WEAPON_LASER)
      return NULL;
    if (entity == ENTITY_DOOR)
      return "doors";
    return "laser walls, draggers, turrets, map shotguns or the shields of single weapons";
  }
  switch (index) {
  case TILE_THROUGH:
  case TILE_THROUGH_ALL:
  case TILE_THROUGH_DIR:
    return "hookthrough other than the shortcut (TILE_THROUGH_CUT)";
  case TILE_FREEZE:
  case TILE_UNFREEZE:
  case TILE_DFREEZE:
  case TILE_DUNFREEZE:
  case TILE_LFREEZE:
  case TILE_LUNFREEZE:
    return "freeze tiles";
  case TILE_WALLJUMP:
  case TILE_EHOOK_ENABLE:
  case TILE_EHOOK_DISABLE:
  case TILE_HIT_ENABLE:
  case TILE_HIT_DISABLE:
  case TILE_SOLO_ENABLE:
  case TILE_SOLO_DISABLE:
  case TILE_REFILL_JUMPS:
  case TILE_CP:
  case TILE_CP_F:
  case TILE_OLDLASER:
  case TILE_NPC:
  case TILE_EHOOK:
  case TILE_NOHIT:
  case TILE_NPH:
  case TILE_UNLOCK_TEAM:
  case TILE_NPC_DISABLE:
  case TILE_UNLIMITED_JUMPS_DISABLE:
  case TILE_JETPACK_DISABLE:
  case TILE_NPH_DISABLE:
  case TILE_TELE_GUN_ENABLE:
  case TILE_TELE_GUN_DISABLE:
  case TILE_NPC_ENABLE:
  case TILE_UNLIMITED_JUMPS_ENABLE:
  case TILE_JETPACK_ENABLE:
  case TILE_NPH_ENABLE:
  case TILE_TELE_GRENADE_ENABLE:
  case TILE_TELE_GRENADE_DISABLE:
  case TILE_TELE_LASER_ENABLE:
  case TILE_TELE_LASER_DISABLE:
    return "the tiles of DDNet's specials (walljump, endless hook, solo, jetpack, teleport guns, ...)";
  default:
    return NULL;
  }
}

/* The map settings: the ones of the backends, and settings that are at the
 * value a Unique Race server has anyway. */
typedef struct settings_check_t {
  const char *unsupported;
} settings_check_t;

static void check_setting(void *user, const char *name, char *args) {
  settings_check_t *check = user;
  if (check->unsupported)
    return;
  /* (settings a server does not know or that a map may not change are ignored by it) */
  static const struct {
    const char *name;
    int value; /* the value of a Unique Race server */
  } FIXED[] = {
      {"sv_fastcap", 0},
      {"sv_endless_drag", 0},
      {"sv_old_teleport_hook", 0},
      {"sv_old_teleport_weapons", 0},
      {"sv_teleport_hold_hook", 0},
      {"sv_teleport_lose_weapons", 0},
      {"sv_freeze_delay", 3},
      {"sv_reset_pickups", 0},
      {"sv_spawn_reload_delay", 8},
  };
  if (strcasecmp(name, "mapbug") == 0) {
    check->unsupported = "the map setting mapbug";
    return;
  }
  for (size_t i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]); i++) {
    if (strcasecmp(name, FIXED[i].name) != 0)
      continue;
    char *argv[DDNET_CONSOLE_MAX_ARGS];
    if (ddnet_console_parse_args(args, "?i", argv) == 1 && (int)strtol(argv[0], NULL, 10) != FIXED[i].value)
      check->unsupported = "a map setting that changes what Unique Race maps keep at the default "
                           "(sv_teleport_hold_hook, sv_freeze_delay, ...)";
    return;
  }
}

const char *ddnet_map_unsupported(const struct map_data_t *map) {
  const int count = map->width * map->height;
  for (int i = 0; i < count; i++) {
    const char *unsupported = unsupported_tile(map->game_layer.data[i]);
    if (!unsupported && map->front_layer.data)
      unsupported = unsupported_tile(map->front_layer.data[i]);
    if (unsupported)
      return unsupported;
    if (map->tele_layer.type && map->tele_layer.number[i] && map->tele_layer.type[i] == TILE_TELEINHOOK)
      return "hook teleporters";
    if (map->speedup_layer.type && map->speedup_layer.force[i] > 0 &&
        map->speedup_layer.type[i] == TILE_SPEED_BOOST)
      return "the new speedups";
    if (map->switch_layer.type && map->switch_layer.type[i])
      return "a switch layer";
  }

  settings_check_t check = {NULL};
  for (int i = 0; i < map->num_settings && !check.unsupported; i++)
    if (map->settings[i])
      ddnet_console_execute_line(map->settings[i], check_setting, &check);
  return check.unsupported;
}
