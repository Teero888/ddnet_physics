/* Port of the game controller of Unique Race: IGameController
 * (src/game/server/gamecontroller.cpp) with its changes for Unique Race,
 * CGameControllerUnique (src/game/server/gamemodes/unique.cpp), and what
 * CGameTeams (src/game/server/teams.cpp) does for a tee that is alone in its
 * team, which on a solo server every tee is. */
#include "internal.h"

/* ---------------------------------------------------------------- spawns */

typedef struct spawn_eval_t {
  vec2 pos;
  bool got;
  float score;
} spawn_eval_t;

/* IGameController::EvaluateSpawnPos: lower is better, far away from the tees
 * that can collide with the one that spawns, which is only itself (when it
 * is teleported to the spawn). */
static float evaluate_spawn_pos(world_t *w, vec2 pos) {
  float score = 0.0f;
  if (w->player.has_character && w->character.alive) {
    float d = vdistance(pos, w->character.pos);
    score += d == 0 ? 1000000000.0f : 1.0f / d;
  }
  return score;
}

/* IGameController::EvaluateSpawnType. A solo server has no player collision,
 * so the place around a spawn point is not searched for room. */
static void evaluate_spawn_type(world_t *w, spawn_eval_t *eval, int spawn_type) {
  const collision_t *col = collision(w);

  /* make sure players keep spawning at the same tile
   * on race maps no matter what */
  if (eval->got)
    return;

  for (int s = 0; s < col->num_spawn_points[spawn_type]; s++) {
    vec2 p = col->spawn_points[spawn_type][s];
    float s_score = evaluate_spawn_pos(w, p);
    if (!eval->got || eval->score > s_score) {
      eval->got = true;
      eval->score = s_score;
      eval->pos = p;
    }
  }
}

/* IGameController::CanSpawn of Unique Race: race spawns at the spawns, fastcap
 * at the red or the blue ones (the /red and /blue commands). */
bool controller_can_spawn(world_t *w, vec2 *out_pos) {
  spawn_eval_t eval;
  eval.got = false;
  eval.pos = v2(100, 100);
  eval.score = 0.0f;
  if (w->config.gamemode != DDNET_MODE_UNIQUE_RACE)
    evaluate_spawn_type(w, &eval, w->player.fastcap_side == 1 ? 1 /* SPAWNTYPE_RED */ : 2 /* SPAWNTYPE_BLUE */);
  else
    evaluate_spawn_type(w, &eval, 0 /* SPAWNTYPE_DEFAULT */);

  *out_pos = eval.pos;
  return eval.got;
}

/* IGameController::OnCharacterSpawn */
void controller_on_character_spawn(world_t *w, character_t *chr) {
  /* (CGameTeams::OnCharacterSpawn: the team of the tee is open to start) */

  /* default health */
  character_increase_health(chr, 10);

  /* give default weapons */
  character_give_weapon(w, chr, WEAPON_HAMMER);
  character_give_weapon(w, chr, WEAPON_GUN);
}

/* CGameControllerUnique::SetArmorProgress: the armor bar shows the freeze
 * time, unless it is armor. */
void controller_set_armor_progress(world_t *w, character_t *chr, int progress) {
  if (!w->config.sv_health_and_ammo)
    chr->armor = clampi(10 - (progress / 15), 0, 10);
}

/* ------------------------------------------------------------ the race */

/* CGameTeams::OnCharacterStart. The team of the tee is open to start from
 * its spawn on until it starts, so this starts the race unless it already
 * runs or is finished. */
void teams_on_character_start(world_t *w, character_t *chr, float fraction_of_tick) {
  (void)w;
  if (chr->race_state == DDNET_RACE_STARTED || chr->race_state == DDNET_RACE_FINISHED)
    return;

  chr->race_state = DDNET_RACE_STARTED;
  chr->start_time = server_tick(w);
  chr->start_time_offset = fraction_of_tick;
}

/* CGameTeams::OnCharacterFinish and CGameTeams::OnFinish of Unique Race: the
 * time is taken in fractions of a tick. */
void teams_on_character_finish(world_t *w, character_t *chr, float fraction_of_tick) {
  int time_ticks = server_tick(w) - chr->start_time;
  if (time_ticks < 0)
    return;

  float time = (fraction_of_tick - chr->start_time_offset + time_ticks) / (float)SERVER_TICK_SPEED;
  w->player.finish_tick = server_tick(w);
  w->player.finish_time = time;

  chr->race_state = DDNET_RACE_FINISHED;

  /* Confetti */
  create_particle(w, chr->pos, DDNET_PARTICLE_CONFETTI);
}

/* CGameControllerUnique::HandleCharacterTiles. sv_legacy_proximity_checks is
 * on in Unique Race: only the tile itself counts, not the tiles around the
 * tee. */
void controller_handle_character_tiles(world_t *w, character_t *chr, int map_index, float fraction_of_tick) {
  const collision_t *col = collision(w);

  int tile_index = collision_get_tile_index(col, map_index);
  int tile_f_index = collision_get_front_tile_index(col, map_index);

  const ddnet_race_state_t player_race_state = chr->race_state;
  bool is_on_start_tile = (tile_index == TILE_START) || (tile_f_index == TILE_START);
  /* start */
  if (is_on_start_tile && player_race_state != DDNET_RACE_CHEATED) {
    if (chr->race_state != DDNET_RACE_STARTED)
      teams_on_character_start(w, chr, fraction_of_tick);
    /* (and the time checkpoints are reset, which are only shown) */
  }

  /* finish */
  if (((tile_index == TILE_FINISH) || (tile_f_index == TILE_FINISH)) && player_race_state == DDNET_RACE_STARTED)
    teams_on_character_finish(w, chr, fraction_of_tick);
}

/* ---------------------------------------------------------- map entities */

/* IGameController::OnEntity of Unique Race. Spawn points are part of the
 * collision; the flag stands are where the flags of fastcap are. */
static bool controller_on_entity(world_t *w, int index, int x, int y) {
  const vec2 pos = v2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);

  int type = -1;
  int subtype = 0;

  if (index == ENTITY_ARMOR_1)
    type = DDNET_UNIQUE_POWERUP_ARMOR;
  else if (index == ENTITY_HEALTH_1)
    type = DDNET_UNIQUE_POWERUP_HEALTH;
  else if (index == ENTITY_WEAPON_SHOTGUN || index == ENTITY_WEAPON_LASER || index == ENTITY_POWERUP_NINJA) {
    /* (Unique Race leaves out the weapons that are not the grenade) */
  } else if (index == ENTITY_WEAPON_GRENADE) {
    /* (and all of them in fastcap without weapons) */
    if (w->config.gamemode != DDNET_MODE_UNIQUE_FASTCAP_NO_WEAPONS) {
      type = DDNET_UNIQUE_POWERUP_WEAPON;
      subtype = WEAPON_GRENADE;
    }
  } else if (index == ENTITY_FLAGSTAND_RED)
    w->flag_pos[TEAM_RED] = pos;
  else if (index == ENTITY_FLAGSTAND_BLUE)
    w->flag_pos[TEAM_BLUE] = pos;

  if (type != -1)
    return pickup_create(w, pos, type, subtype);
  return true;
}

/* CGameContext::CreateAllEntities */
bool controller_create_all_entities(world_t *w) {
  const collision_t *col = collision(w);

  for (int y = 0; y < col->height; y++) {
    for (int x = 0; x < col->width; x++) {
      const int index = y * col->width + x;

      /* Game layer (the tiles that switch settings on are not in the maps) */
      const int game_index = col->game[index].index;
      if (game_index >= ENTITY_OFFSET && !controller_on_entity(w, game_index - ENTITY_OFFSET, x, y))
        return false;

      if (col->front) {
        const int front_index = col->front[index].index;
        if (front_index >= ENTITY_OFFSET && !controller_on_entity(w, front_index - ENTITY_OFFSET, x, y))
          return false;
      }
    }
  }
  return true;
}
