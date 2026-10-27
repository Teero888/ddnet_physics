/* The world of the optimized Unique backend: what is not the tick (players,
 * spawning, the lifecycle of a world and what its copies share). */
#include "internal.h"

#include "../common/console.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------- the tuning */

/* The floats of the tuning of a zone, and what is worked out from them. */
static void tuning_values_from(tuning_t *values, const ddnet_tuning_t *tuning) {
  const ddnet_tune_param_t *params = (const ddnet_tune_param_t *)tuning;
  float *floats = (float *)values;
  for (int i = 0; i < DDNET_NUM_TUNING_PARAMS; i++)
    floats[i] = params[i] / 100.0f;

  /* The smallest squared speed d for which VelocityRamp() does not return
   * early, which is "sqrtf(d) * 50 < start" being false. That only becomes
   * false once as d grows, so it can be searched in the bits of d. */
  const float start = values->velramp_start;
  uint32_t low = 0, high = 0x7f800000u; /* 0 to infinity */
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    float d;
    memcpy(&d, &middle, sizeof(d));
    if (sqrtf(d) * 50 < start)
      low = middle + 1;
    else
      high = middle;
  }
  memcpy(&values->velramp_start_squared, &low, sizeof(low));

  /* and the smallest d for which "sqrtf(d) > 6000" */
  low = 0;
  high = 0x7f800000u;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    float d;
    memcpy(&d, &middle, sizeof(d));
    if (sqrtf(d) > 6000)
      high = middle;
    else
      low = middle + 1;
  }
  memcpy(&values->speed_clamp_squared, &low, sizeof(low));

  /* and the smallest d for which "sqrtf(d) > hook_length" */
  const float hook_length = values->hook_length;
  low = 0;
  high = 0x7f800000u;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    float d;
    memcpy(&d, &middle, sizeof(d));
    if (sqrtf(d) > hook_length)
      high = middle;
    else
      low = middle + 1;
  }
  /* (no distance at all is above a length that is infinite or not a number) */
  if (!(hook_length < INFINITY))
    low = 0x7fc00000u;
  memcpy(&values->hook_length_squared, &low, sizeof(low));

  velocity_ramp_prepare(values);
}

/* --------------------------------------------------- what copies share */

static void shared_release(ddnet_world_shared_t *shared) {
  if (shared && __atomic_sub_fetch(&shared->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    free(shared->tuning);
    free(shared->tuning_values);
    free(shared->pickups);
    free(shared->pickup_of_map);
    free(shared);
  }
}

static ddnet_world_shared_t *shared_new(int num_tune_zones, int num_pickups, int num_map_pickups) {
  ddnet_world_shared_t *shared = calloc(1, sizeof(*shared));
  if (!shared)
    return NULL;
  shared->refs = 1;
  shared->tuning = calloc((size_t)num_tune_zones, sizeof(*shared->tuning));
  shared->tuning_values = calloc((size_t)num_tune_zones, sizeof(*shared->tuning_values));
  shared->pickups = calloc((size_t)(num_pickups ? num_pickups : 1), sizeof(*shared->pickups));
  shared->pickup_of_map = calloc((size_t)(num_map_pickups ? num_map_pickups : 1), sizeof(*shared->pickup_of_map));
  if (!shared->tuning || !shared->tuning_values || !shared->pickups || !shared->pickup_of_map) {
    shared_release(shared);
    return NULL;
  }
  return shared;
}

static void world_use_shared(world_t *w, ddnet_world_shared_t *shared) {
  w->shared = shared;
  w->tuning = shared->tuning;
  w->tuning_values = shared->tuning_values;
  w->pickups = shared->pickups;
}

ddnet_tuning_t *ddnet_world_tuning_edit(ddnet_world_t *w) {
  if (__atomic_load_n(&w->shared->refs, __ATOMIC_ACQUIRE) > 1) {
    ddnet_world_shared_t *own = shared_new(w->num_tune_zones, w->num_pickups, w->collision->num_pickups);
    if (!own)
      return NULL;
    memcpy(own->tuning, w->tuning, (size_t)w->num_tune_zones * sizeof(*own->tuning));
    memcpy(own->tuning_values, w->tuning_values, (size_t)w->num_tune_zones * sizeof(*own->tuning_values));
    memcpy(own->pickups, w->pickups, (size_t)w->num_pickups * sizeof(*own->pickups));
    memcpy(own->pickup_of_map, w->shared->pickup_of_map,
           (size_t)w->collision->num_pickups * sizeof(*own->pickup_of_map));
    shared_release(w->shared);
    world_use_shared(w, own);
  }
  return w->shared->tuning;
}

void ddnet_world_tuning_changed(ddnet_world_t *w) {
  if (!ddnet_world_tuning_edit(w))
    return;
  for (int zone = 0; zone < w->num_tune_zones; zone++)
    tuning_values_from(&w->shared->tuning_values[zone], &w->shared->tuning[zone]);
}

void ddnet_world_sync(ddnet_world_t *w) { (void)w; }
void ddnet_world_changed(ddnet_world_t *w) { (void)w; }

/* ------------------------------------------------------------- the tee */

int ddnet_character_angle(const ddnet_character_t *chr) {
  /* CCharacterCore::Tick with the aim of its last tick; atan2 is the double version, called with integers */
  float tmp_angle = atan2(chr->core.aim_y, chr->core.aim_x);
  if (tmp_angle < -(PI / 2.0f))
    return (int)((tmp_angle + (2.0f * PI)) * 256.0f);
  return (int)(tmp_angle * 256.0f);
}

ddnet_character_t *ddnet_world_character(ddnet_world_t *w) { return get_player_char(w); }

void ddnet_character_changed(ddnet_world_t *w) { (void)w; }

/* IGameController::EvaluateSpawnType and CanSpawn of Unique Race, see the reference backend */
bool controller_can_spawn(world_t *w, vec2 *out_pos) {
  const collision_t *col = collision(w);
  const int type = w->config.gamemode != DDNET_MODE_UNIQUE_RACE ? (w->player.fastcap_side == 1 ? 1 : 2) : 0;
  const character_t *chr = get_player_char(w);
  bool got = false;
  float best = 0.0f;
  *out_pos = v2(100, 100);
  for (int s = 0; s < col->num_spawn_points[type]; s++) {
    const vec2 p = col->spawn_points[type][s];
    float score = 0.0f;
    if (chr) {
      float d = vdistance(p, chr->pos);
      score += d == 0 ? 1000000000.0f : 1.0f / d;
    }
    if (!got || best > score) {
      got = true;
      best = score;
      *out_pos = p;
    }
  }
  return got;
}

/* CCharacter::CCharacter, Spawn and DDRaceInit, IGameController::OnCharacterSpawn */
static void character_spawn(world_t *w, vec2 pos) {
  const collision_t *col = collision(w);
  character_t *chr = &w->character;
  player_t *player = &w->player;

  memset(chr, 0, sizeof(*chr));
  chr->input = player->input;
  chr->input.target_x = 0;
  chr->input.target_y = -1;
  chr->reload_timer = SV_SPAWN_RELOAD_DELAY;
  chr->last_weapon = WEAPON_HAMMER;
  chr->queued_weapon = -1;
  chr->pos = pos;
  chr->spawn_tick = server_tick(w);

  core_t *core = &chr->core;
  core->hook_state = HOOK_IDLE;
  core->jumps = 2;
  core->input.target_y = -1;
  core->active_weapon = WEAPON_GUN;
  core->pos = pos;

  chr->health = 10;
  chr->armor = w->config.sv_health_and_ammo && w->config.gamemode == DDNET_MODE_UNIQUE_RACE ? 0 : 10;
  if (w->config.gamemode == DDNET_MODE_UNIQUE_FASTCAP) {
    core->weapons[WEAPON_GRENADE].ammo = GRENADE_MAX_AMMO; /* (fastcap has sv_health_and_ammo) */
    core->weapons[WEAPON_GRENADE].got = true;
    /* CCharacter::SetWeapon, silent */
    chr->last_weapon = core->active_weapon;
    core->active_weapon = WEAPON_GRENADE;
  }

  player->has_character = true;
  chr->alive = true;

  /* give default weapons */
  core->weapons[WEAPON_HAMMER].ammo = -1;
  core->weapons[WEAPON_HAMMER].got = true;
  core->weapons[WEAPON_GUN].ammo = -1;
  core->weapons[WEAPON_GUN].got = true;

  chr->race_state = DDNET_RACE_NONE;
  chr->prev_pos = chr->pos;
  chr->tune_zone = collision_is_tune(col, collision_get_map_index(col, pos));
}

/* ------------------------------------------------------------- the player */

static void player_kill_character(world_t *w) {
  if (w->player.has_character) {
    character_t *chr = &w->character;
    EMIT_SOUND(w, chr->pos, DDNET_SOUND_PLAYER_DIE);
    EMIT_PARTICLE(w, chr->pos, DDNET_PARTICLE_PLAYER_DEATH);
    w->player.previous_die_tick = w->player.die_tick;
    w->player.die_tick = server_tick(w);
    chr->alive = false;
    w->player.has_character = false;
  }
}

/* CPlayer::TryRespawn */
static void player_try_respawn(world_t *w) {
  vec2 spawn_pos;
  if (!controller_can_spawn(w, &spawn_pos))
    return;
  w->player.spawning = false;
  w->player.view_pos = spawn_pos;
  character_spawn(w, spawn_pos);
  EMIT_PARTICLE(w, spawn_pos, DDNET_PARTICLE_PLAYER_SPAWN);
}

/* CPlayer::Tick */
void player_tick(world_t *w) {
  player_t *player = &w->player;

  int earliest_respawn_tick = player->previous_die_tick + SERVER_TICK_SPEED * 3;
  int respawn_tick = maxi(player->die_tick, earliest_respawn_tick) + 2;
  if (!player->has_character && respawn_tick <= server_tick(w))
    player->spawning = true;

  if (player->has_character) {
    if (w->character.alive)
      player->view_pos = w->character.pos;
    else
      player->has_character = false;
  } else if (player->spawning) {
    player_try_respawn(w);
  }

  /* determine needed tunings with viewpos */
  const collision_t *col = collision(w);
  player->tune_zone = col->tune ? collision_is_tune(col, collision_get_map_index(col, player->view_pos)) : 0;
}

bool ddnet_player_spawn(ddnet_world_t *w) {
  if (!w->player.has_character && w->player.spawning)
    player_try_respawn(w);
  return w->player.has_character;
}

static bool player_kill_delayed(const world_t *w) {
  return w->player.last_kill && w->player.last_kill + SERVER_TICK_SPEED / 2 > server_tick(w);
}

bool ddnet_player_kill(ddnet_world_t *w) {
  if (player_kill_delayed(w) || !get_player_char(w))
    return false;
  w->player.last_kill = server_tick(w);
  player_kill_character(w);
  w->player.spawning = true;
  return true;
}

bool ddnet_player_set_fastcap_side(ddnet_world_t *w, int side) {
  if (w->config.gamemode == DDNET_MODE_UNIQUE_RACE || (side != 1 && side != 2))
    return false;
  w->player.fastcap_side = side;
  return ddnet_player_kill(w);
}

/* ------------------------------------------------------------- lifecycle */

static void world_execute_map_command(void *user, const char *name, char *args) {
  world_t *w = user;
  char *argv[DDNET_CONSOLE_MAX_ARGS];
  ddnet_tuning_t *tuning = w->shared->tuning;

  if (strcasecmp(name, "tune") == 0) {
    if (ddnet_console_parse_args(args, "s ?f", argv) == 2)
      ddnet_tuning_set_by_name(&tuning[0], argv[0], strtod(argv[1], NULL));
  } else if (strcasecmp(name, "tune_zone") == 0) {
    if (ddnet_console_parse_args(args, "i s f", argv) == 3) {
      int list = (int)strtol(argv[0], NULL, 10);
      if (list >= 0 && list < w->num_tune_zones)
        ddnet_tuning_set_by_name(&tuning[list], argv[1], strtod(argv[2], NULL));
    }
  } else {
    if (ddnet_console_parse_args(args, "?i", argv) == 1)
      ddnet_config_set_from_map(&w->config, name, (int)strtol(argv[0], NULL, 10));
  }
}

bool ddnet_world_init(ddnet_world_t *w, const ddnet_collision_t *col, const ddnet_config_t *config) {
  memset(w, 0, sizeof(*w));
  w->collision = col;
  w->config = *config;
  w->num_tune_zones = col->highest_tune_zone + 1;

  /* (at most as many pickups as the map has, of which Unique Race keeps some) */
  ddnet_world_shared_t *shared = shared_new(w->num_tune_zones, col->num_pickups, col->num_pickups);
  if (!shared)
    return false;
  world_use_shared(w, shared);

  for (int i = 0; i < w->num_tune_zones; i++)
    ddnet_tuning_init_ddrace(&shared->tuning[i]);

  /* CGameContext::LoadMapSettings */
  for (int i = 0; i < col->num_settings; i++)
    ddnet_console_execute_line(col->settings[i], world_execute_map_command, w);

  /* Unique: fastcap forces damage, and the grenades die with their tee */
  if (w->config.gamemode != DDNET_MODE_UNIQUE_RACE) {
    w->config.sv_health_and_ammo = 1;
    w->config.sv_kill_grenades = 1;
  }

  /* sv_solo_server */
  for (int i = 0; i < w->num_tune_zones; i++) {
    ddnet_tuning_set_by_name(&shared->tuning[i], "player_collision", 0);
    ddnet_tuning_set_by_name(&shared->tuning[i], "player_hooking", 0);
    tuning_values_from(&shared->tuning_values[i], &shared->tuning[i]);
  }

  /* The pickups of IGameController::OnEntity of Unique Race, in the order of
   * their list: the one made last first. */
  for (int i = col->num_pickups - 1; i >= 0; i--) {
    const ddnet_map_pickup_t *map_pickup = &col->pickups[i];
    shared->pickup_of_map[i] = -1;
    int type;
    if (map_pickup->type == DDNET_POWERUP_FREEZE)
      type = DDNET_UNIQUE_POWERUP_HEALTH;
    else if (map_pickup->type == DDNET_POWERUP_ARMOR)
      type = DDNET_UNIQUE_POWERUP_ARMOR;
    else if (map_pickup->type == DDNET_POWERUP_WEAPON && map_pickup->subtype == WEAPON_GRENADE &&
             w->config.gamemode != DDNET_MODE_UNIQUE_FASTCAP_NO_WEAPONS)
      type = DDNET_UNIQUE_POWERUP_WEAPON;
    else
      continue;
    shared->pickup_of_map[i] = w->num_pickups;
    shared->pickups[w->num_pickups++] = (ddnet_pickup_t){map_pickup->pos, type, map_pickup->subtype};
  }
  w->next_respawn = INT_MAX;
  if (w->config.sv_health_and_ammo && w->num_pickups) {
    w->pickup_spawn_ticks = malloc((size_t)w->num_pickups * sizeof(*w->pickup_spawn_ticks));
    if (!w->pickup_spawn_ticks)
      goto fail;
    for (int i = 0; i < w->num_pickups; i++)
      w->pickup_spawn_ticks[i] = -1;
  }

  /* the flag stands, the last one of a color counts */
  for (int y = 0; y < col->height; y++) {
    for (int x = 0; x < col->width; x++) {
      const int index = y * col->width + x;
      const int entities[2] = {col->game[index].index - ENTITY_OFFSET,
                               col->front ? col->front[index].index - ENTITY_OFFSET : 0};
      for (int l = 0; l < 2; l++) {
        if (entities[l] == ENTITY_FLAGSTAND_RED)
          w->flag_pos[TEAM_RED] = v2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
        else if (entities[l] == ENTITY_FLAGSTAND_BLUE)
          w->flag_pos[TEAM_BLUE] = v2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
      }
    }
  }

  /* CPlayer::CPlayer and CPlayer::Reset, IGameController::OnPlayerConnect */
  w->player.die_tick = server_tick(w);
  w->player.previous_die_tick = w->player.die_tick;
  w->player.fastcap_side = 1;
  w->player.finish_tick = -1;
  w->player.spawning = true;
  return true;

fail:
  ddnet_world_free(w);
  return false;
}

void ddnet_world_free(ddnet_world_t *w) {
  shared_release(w->shared);
  free(w->projectiles);
  free(w->pickup_spawn_ticks);
  memset(w, 0, sizeof(*w));
}

bool ddnet_world_copy(ddnet_world_t *dst, const ddnet_world_t *src) {
  if (dst == src)
    return true;

  ddnet_world_t old = *dst;
  *dst = *src;
  /* (a copy of a copy holds the same block already) */
  if (old.shared != src->shared) {
    if (src->shared)
      __atomic_add_fetch(&src->shared->refs, 1, __ATOMIC_RELAXED);
    shared_release(old.shared);
  }

  dst->projectiles = old.projectiles;
  dst->projectile_capacity = old.projectile_capacity;
  if (dst->projectile_capacity < src->num_projectiles) {
    projectile_t *projectiles = realloc(dst->projectiles, (size_t)src->projectile_capacity * sizeof(*projectiles));
    if (!projectiles)
      goto fail;
    dst->projectiles = projectiles;
    dst->projectile_capacity = src->projectile_capacity;
  }
  if (src->num_projectiles)
    memcpy(dst->projectiles, src->projectiles, (size_t)src->num_projectiles * sizeof(*dst->projectiles));

  dst->pickup_spawn_ticks = old.pickup_spawn_ticks;
  if (src->pickup_spawn_ticks) {
    if (!dst->pickup_spawn_ticks || old.num_pickups < src->num_pickups) {
      int *ticks = realloc(dst->pickup_spawn_ticks, (size_t)src->num_pickups * sizeof(*ticks));
      if (!ticks)
        goto fail;
      dst->pickup_spawn_ticks = ticks;
    }
    memcpy(dst->pickup_spawn_ticks, src->pickup_spawn_ticks, (size_t)src->num_pickups * sizeof(int));
  } else {
    free(dst->pickup_spawn_ticks);
    dst->pickup_spawn_ticks = NULL;
  }
  return true;

fail:
  ddnet_world_free(dst);
  return false;
}
