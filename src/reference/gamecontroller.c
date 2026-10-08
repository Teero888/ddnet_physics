/* Port of the game controller: IGameController (src/game/server/gamecontroller.cpp)
 * and the game mode on top of it (src/game/server/gamemodes/).
 *
 * This is where game modes differ: where tees spawn, what the map entities
 * are, and what start, finish and the other race tiles do. */
#include "internal.h"

/* ---------------------------------------------------------------- spawns */

typedef struct spawn_eval_t {
  vec2 pos;
  bool got;
  float score;
} spawn_eval_t;

/* IGameController::EvaluateSpawnPos: lower is better, far away from others. */
static float evaluate_spawn_pos(world_t *w, vec2 pos, int client_id) {
  float score = 0.0f;
  for (int id = w->first_entity[DDNET_ENTTYPE_CHARACTER]; id != -1; id = w->characters[id].link.next) {
    const character_t *chr = &w->characters[id];
    if (!character_can_collide(w, chr, client_id))
      continue;

    float d = vdistance(pos, chr->pos);
    score += d == 0 ? 1000000000.0f : 1.0f / d;
  }

  return score;
}

/* IGameController::EvaluateSpawnType */
static void evaluate_spawn_type(world_t *w, spawn_eval_t *eval, int spawn_type, int client_id) {
  const collision_t *col = collision(w);
  const bool player_collision = ddnet_tune(global_tuning(w)->player_collision);

  bool player_collision_disabled = false;
  character_t *player_character = get_player_char(w, client_id);
  if (player_character)
    player_collision_disabled = player_character->core.collision_disabled;

  /* make sure players keep spawning at the same tile
   * on race maps no matter what */
  if (!player_collision && eval->got)
    return;

  /* j == 0: Find an empty slot, j == 1: Take any slot if no empty one found */
  for (int j = 0; j < 2; j++) {
    /* get spawn point */
    for (int s = 0; s < col->num_spawn_points[spawn_type]; s++) {
      const vec2 spawn_point = col->spawn_points[spawn_type][s];
      vec2 p = spawn_point;
      if (j == 0) {
        /* check if the position is occupado */
        int ents[MAX_CLIENTS];
        int num = world_find_characters(w, spawn_point, 64, ents, MAX_CLIENTS);
        /* start, left, up, right, down */
        vec2 positions[5] = {{0.0f, 0.0f}, {-32.0f, 0.0f}, {0.0f, -32.0f}, {32.0f, 0.0f}, {0.0f, 32.0f}};
        int result = -1;
        for (int index = 0; index < 5 && result == -1; ++index) {
          result = index;
          if (!player_collision || player_collision_disabled)
            break;
          for (int c = 0; c < num; ++c) {
            const character_t *chr = &w->characters[ents[c]];
            const bool can_collide =
                character_can_collide(w, chr, client_id) && !chr->core.collision_disabled;

            vec2 check_pos = vadd(spawn_point, positions[index]);
            if (collision_check_point(col, check_pos.x, check_pos.y) ||
                (can_collide && vdistance(chr->pos, vadd(spawn_point, positions[index])) <= PHYSICAL_SIZE)) {
              result = -1;
              break;
            }
          }
        }
        if (result == -1)
          continue; /* try next spawn point */

        p = vadd(p, positions[result]);
      }

      float s_score = evaluate_spawn_pos(w, p, client_id);
      if (!eval->got || (j == 0 && eval->score > s_score)) {
        eval->got = true;
        eval->score = s_score;
        eval->pos = p;
      }
    }
  }
}

/* IGameController::CanSpawn */
bool controller_can_spawn(world_t *w, vec2 *out_pos, int client_id) {
  spawn_eval_t eval;
  eval.got = false;
  eval.pos = v2(100, 100);
  eval.score = 0.0f;
  evaluate_spawn_type(w, &eval, 0, client_id); /* SPAWNTYPE_DEFAULT */
  evaluate_spawn_type(w, &eval, 1, client_id); /* SPAWNTYPE_RED */
  evaluate_spawn_type(w, &eval, 2, client_id); /* SPAWNTYPE_BLUE */

  *out_pos = eval.pos;
  return eval.got;
}

/* IGameController::OnCharacterSpawn */
void controller_on_character_spawn(world_t *w, character_t *chr) {

  teams_on_character_spawn(w, character_id(chr));

  /* default health */
  character_increase_health(chr, 10);

  /* give default weapons */
  character_give_weapon(w, chr, WEAPON_HAMMER, false);
  character_give_weapon(w, chr, WEAPON_GUN, false);
}

/* CGameControllerDDNet::SetArmorProgress: the armor bar shows freeze and ninja time. */
void controller_set_armor_progress(character_t *chr, int progress) {
  chr->armor = clampi(10 - (progress / 15), 0, 10);
}

/* ------------------------------------------------------------ race tiles */

/* CGameControllerDDNet::HandleCharacterTiles */
static void ddrace_handle_character_tiles(world_t *w, character_t *chr, int map_index) {
  const collision_t *col = collision(w);
  const ddnet_config_t *config = &w->config;
  const int client_id = character_id(chr);

  int tile_index = collision_get_tile_index(col, map_index);
  int tile_f_index = collision_get_front_tile_index(col, map_index);

  /* Sensitivity */
  int s1 = collision_get_pure_map_index(
      col, v2(chr->pos.x + PHYSICAL_SIZE / 3.f, chr->pos.y - PHYSICAL_SIZE / 3.f));
  int s2 = collision_get_pure_map_index(
      col, v2(chr->pos.x + PHYSICAL_SIZE / 3.f, chr->pos.y + PHYSICAL_SIZE / 3.f));
  int s3 = collision_get_pure_map_index(
      col, v2(chr->pos.x - PHYSICAL_SIZE / 3.f, chr->pos.y - PHYSICAL_SIZE / 3.f));
  int s4 = collision_get_pure_map_index(
      col, v2(chr->pos.x - PHYSICAL_SIZE / 3.f, chr->pos.y + PHYSICAL_SIZE / 3.f));
  int tile1 = collision_get_tile_index(col, s1);
  int tile2 = collision_get_tile_index(col, s2);
  int tile3 = collision_get_tile_index(col, s3);
  int tile4 = collision_get_tile_index(col, s4);
  int ftile1 = collision_get_front_tile_index(col, s1);
  int ftile2 = collision_get_front_tile_index(col, s2);
  int ftile3 = collision_get_front_tile_index(col, s3);
  int ftile4 = collision_get_front_tile_index(col, s4);

  const ddnet_race_state_t player_race_state = chr->race_state;
  bool is_on_start_tile = (tile_index == TILE_START) || (tile_f_index == TILE_START) ||
                          ftile1 == TILE_START || ftile2 == TILE_START || ftile3 == TILE_START ||
                          ftile4 == TILE_START || tile1 == TILE_START || tile2 == TILE_START ||
                          tile3 == TILE_START || tile4 == TILE_START;
  /* start */
  if (is_on_start_tile && player_race_state != DDNET_RACE_CHEATED) {
    const int team = teams_team(w, client_id);
    if (config->sv_team == DDNET_SV_TEAM_MANDATORY && (team == TEAM_FLOCK || teams_team_size(w, team) <= 1)) {
      /* You have to be in a team with other tees to start */
      character_die(w, chr, client_id, WEAPON_WORLD);
      return;
    }
    if (config->sv_reset_pickups)
      character_reset_pickups(chr);

    teams_on_character_start(w, client_id);
    chr->last_time_cp = -1;
    for (int i = 0; i < DDNET_MAX_CHECKPOINTS; i++)
      chr->current_time_cp[i] = 0.0f;
  }

  /* finish */
  if (((tile_index == TILE_FINISH) || (tile_f_index == TILE_FINISH) || ftile1 == TILE_FINISH ||
       ftile2 == TILE_FINISH || ftile3 == TILE_FINISH || ftile4 == TILE_FINISH || tile1 == TILE_FINISH ||
       tile2 == TILE_FINISH || tile3 == TILE_FINISH || tile4 == TILE_FINISH) &&
      player_race_state == DDNET_RACE_STARTED)
    teams_on_character_finish(w, client_id);

  /* unlock team */
  else if (((tile_index == TILE_UNLOCK_TEAM) || (tile_f_index == TILE_UNLOCK_TEAM)) &&
           teams_team_locked(w, teams_team(w, client_id)))
    teams_set_team_lock(w, teams_team(w, client_id), false);

  /* solo part */
  if (((tile_index == TILE_SOLO_ENABLE) || (tile_f_index == TILE_SOLO_ENABLE)) &&
      !teams_get_solo(w, client_id))
    character_set_solo(w, chr, true);
  else if (((tile_index == TILE_SOLO_DISABLE) || (tile_f_index == TILE_SOLO_DISABLE)) &&
           teams_get_solo(w, client_id))
    character_set_solo(w, chr, false);
}

void controller_handle_character_tiles(world_t *w, character_t *chr, int map_index) {
  switch (w->config.gamemode) {
  case DDNET_MODE_DDRACE:
  default:
    ddrace_handle_character_tiles(w, chr, map_index);
    break;
  }
}

/* ---------------------------------------------------------- map entities */

/* IGameController::OnEntity. Spawn points and doors are part of the
 * collision, see load_static_entity(). */
static bool controller_on_entity(world_t *w, int index, int x, int y, int layer, int flags, int number) {
  const collision_t *col = collision(w);
  const vec2 pos = v2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);

  int sides[8];
  sides[0] = collision_entity(col, x, y + 1, layer);
  sides[1] = collision_entity(col, x + 1, y + 1, layer);
  sides[2] = collision_entity(col, x + 1, y, layer);
  sides[3] = collision_entity(col, x + 1, y - 1, layer);
  sides[4] = collision_entity(col, x, y - 1, layer);
  sides[5] = collision_entity(col, x - 1, y - 1, layer);
  sides[6] = collision_entity(col, x - 1, y, layer);
  sides[7] = collision_entity(col, x - 1, y + 1, layer);

  if (index == ENTITY_CRAZY_SHOTGUN_EX) {
    int dir;
    if (!flags)
      dir = 0;
    else if (flags == ROTATION_90)
      dir = 1;
    else if (flags == ROTATION_180)
      dir = 2;
    else
      dir = 3;
    float deg = dir * (PI / 2);
    projectile_create(w, WEAPON_SHOTGUN, -1, pos, v2(sinf(deg), cosf(deg)), -2, true, true, layer, number,
                      2 - (dir % 2));
  } else if (index == ENTITY_CRAZY_SHOTGUN) {
    int dir;
    if (!flags)
      dir = 0;
    else if (flags == (TILEFLAG_ROTATE))
      dir = 1;
    else if (flags == (TILEFLAG_XFLIP | TILEFLAG_YFLIP))
      dir = 2;
    else
      dir = 3;
    float deg = dir * (PI / 2);
    projectile_create(w, WEAPON_SHOTGUN, -1, pos, v2(sinf(deg), cosf(deg)), -2, true, false, layer, number,
                      2 - (dir % 2));
  }

  int type = -1;
  int subtype = 0;

  if (index == ENTITY_ARMOR_1)
    type = DDNET_POWERUP_ARMOR;
  else if (index == ENTITY_ARMOR_SHOTGUN)
    type = DDNET_POWERUP_ARMOR_SHOTGUN;
  else if (index == ENTITY_ARMOR_GRENADE)
    type = DDNET_POWERUP_ARMOR_GRENADE;
  else if (index == ENTITY_ARMOR_NINJA)
    type = DDNET_POWERUP_ARMOR_NINJA;
  else if (index == ENTITY_ARMOR_LASER)
    type = DDNET_POWERUP_ARMOR_LASER;
  else if (index == ENTITY_HEALTH_1)
    type = DDNET_POWERUP_FREEZE;
  else if (index == ENTITY_WEAPON_SHOTGUN) {
    type = DDNET_POWERUP_WEAPON;
    subtype = WEAPON_SHOTGUN;
  } else if (index == ENTITY_WEAPON_GRENADE) {
    type = DDNET_POWERUP_WEAPON;
    subtype = WEAPON_GRENADE;
  } else if (index == ENTITY_WEAPON_LASER) {
    type = DDNET_POWERUP_WEAPON;
    subtype = WEAPON_LASER;
  } else if (index == ENTITY_POWERUP_NINJA) {
    type = DDNET_POWERUP_NINJA;
    subtype = WEAPON_NINJA;
  } else if (index >= ENTITY_LASER_FAST_CCW && index <= ENTITY_LASER_FAST_CW) {
    int sides2[8];
    sides2[0] = collision_entity(col, x, y + 2, layer);
    sides2[1] = collision_entity(col, x + 2, y + 2, layer);
    sides2[2] = collision_entity(col, x + 2, y, layer);
    sides2[3] = collision_entity(col, x + 2, y - 2, layer);
    sides2[4] = collision_entity(col, x, y - 2, layer);
    sides2[5] = collision_entity(col, x - 2, y - 2, layer);
    sides2[6] = collision_entity(col, x - 2, y, layer);
    sides2[7] = collision_entity(col, x - 2, y + 2, layer);

    int ind = index - ENTITY_LASER_STOP;
    int m;
    if (ind < 0) {
      ind = -ind;
      m = 1;
    } else if (ind == 0)
      m = 0;
    else
      m = -1;

    float angular_speed = 0.0f;
    if (ind == 0)
      angular_speed = 0.0f;
    else if (ind == 1)
      angular_speed = PI / 360;
    else if (ind == 2)
      angular_speed = PI / 180;
    else if (ind == 3)
      angular_speed = PI / 90;
    angular_speed *= m;

    for (int i = 0; i < 8; i++) {
      if (sides[i] >= ENTITY_LASER_SHORT && sides[i] <= ENTITY_LASER_LONG) {
        int light_index = light_create(w, pos, PI / 4 * i, 32 * 3 + 32 * (sides[i] - ENTITY_LASER_SHORT) * 3,
                                       layer, number);
        if (light_index < 0)
          return false;
        ddnet_light_t *light = &entity(w, light_index)->u.light;
        light->angular_speed = angular_speed;
        if (sides2[i] >= ENTITY_LASER_C_SLOW && sides2[i] <= ENTITY_LASER_C_FAST) {
          light->speed = 1 + (sides2[i] - ENTITY_LASER_C_SLOW) * 2;
          light->curve_length = light->length;
        } else if (sides2[i] >= ENTITY_LASER_O_SLOW && sides2[i] <= ENTITY_LASER_O_FAST) {
          light->speed = 1 + (sides2[i] - ENTITY_LASER_O_SLOW) * 2;
          light->curve_length = 0;
        } else
          light->curve_length = light->length;
      }
    }
  } else if (index >= ENTITY_DRAGGER_WEAK && index <= ENTITY_DRAGGER_STRONG) {
    if (!dragger_create(w, pos, index - ENTITY_DRAGGER_WEAK + 1, false, layer, number))
      return false;
  } else if (index >= ENTITY_DRAGGER_WEAK_NW && index <= ENTITY_DRAGGER_STRONG_NW) {
    if (!dragger_create(w, pos, index - ENTITY_DRAGGER_WEAK_NW + 1, true, layer, number))
      return false;
  } else if (index == ENTITY_PLASMAE) {
    if (!gun_create(w, pos, false, true, layer, number))
      return false;
  } else if (index == ENTITY_PLASMAF) {
    if (!gun_create(w, pos, true, false, layer, number))
      return false;
  } else if (index == ENTITY_PLASMA) {
    if (!gun_create(w, pos, true, true, layer, number))
      return false;
  } else if (index == ENTITY_PLASMAU) {
    if (!gun_create(w, pos, false, false, layer, number))
      return false;
  }

  if (type != -1)
    pickup_create(w, pos, type, subtype, layer, number);
  return true;
}

/* The settings that single tiles anywhere in the map switch on. */
static void apply_setting_tile(world_t *w, int tile) {
  if (tile == TILE_OLDLASER)
    w->config.sv_old_laser = 1;
  else if (tile == TILE_NPC)
    ddnet_tuning_set_by_name(global_tuning(w), "player_collision", 0);
  else if (tile == TILE_EHOOK)
    w->config.sv_endless_drag = 1;
  else if (tile == TILE_NOHIT)
    w->config.sv_hit = 0;
  else if (tile == TILE_NPH)
    ddnet_tuning_set_by_name(global_tuning(w), "player_hooking", 0);
}

/* CGameContext::CreateAllEntities */
bool controller_create_all_entities(world_t *w) {
  const collision_t *col = collision(w);

  for (int y = 0; y < col->height; y++) {
    for (int x = 0; x < col->width; x++) {
      const int index = y * col->width + x;

      /* Game layer */
      {
        const int game_index = col->game[index].index;
        if (game_index >= ENTITY_OFFSET) {
          if (!controller_on_entity(w, game_index - ENTITY_OFFSET, x, y, LAYER_GAME, col->game[index].flags,
                                    0))
            return false;
        } else {
          apply_setting_tile(w, game_index);
        }
      }

      if (col->front) {
        const int front_index = col->front[index].index;
        if (front_index >= ENTITY_OFFSET) {
          if (!controller_on_entity(w, front_index - ENTITY_OFFSET, x, y, LAYER_FRONT,
                                    col->front[index].flags, 0))
            return false;
        } else {
          apply_setting_tile(w, front_index);
        }
      }

      if (col->switch_tiles) {
        const int switch_type = col->switch_tiles[index].type;
        if (switch_type >= ENTITY_OFFSET) {
          if (!controller_on_entity(w, switch_type - ENTITY_OFFSET, x, y, LAYER_SWITCH,
                                    col->switch_tiles[index].flags, col->switch_tiles[index].number))
            return false;
        }
      }
    }
  }
  return true;
}
