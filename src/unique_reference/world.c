/* The world: what CGameWorld, CGameContext, CPlayer and the tick loop of the
 * engine server (CServer::Run) of Unique Race do for the physics, for a
 * world of one player. */
#include "internal.h"

#include "../common/console.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------------------------------------------------- teleporters */

/* The server picks the exit of a teleporter with several exits at random
 * (CWorldCore::RandomOr0). Here the caller decides. */
int world_pick_tele_out(const world_t *w, int count) {
  if (count <= 1)
    return 0;
  return (int)((unsigned)w->tele_out % (unsigned)count);
}

/* ------------------------------------------------------------- accessors */

character_t *get_player_char(world_t *w) {
  /* CPlayer::GetCharacter */
  if (w->player.has_character && w->character.alive)
    return &w->character;
  return NULL;
}

int ddnet_character_angle(const ddnet_character_t *chr) { return chr->core.angle; }

ddnet_character_t *ddnet_world_character(ddnet_world_t *world) { return get_player_char(world); }

void ddnet_character_changed(ddnet_world_t *world) { (void)world; }

/* ---------------------------------------------------------- entity lists */

/* CGameWorld::InsertEntity: new entities go to the front of their list. */
void world_insert_entity(world_t *w, int type, int index) {
  ddnet_entity_link_t *link = &w->entities[index].link;
  if (w->first_entity[type] != -1)
    w->entities[w->first_entity[type]].link.prev = index;
  link->next = w->first_entity[type];
  link->prev = -1;
  w->first_entity[type] = index;
}

/* CGameWorld::RemoveEntity */
void world_remove_entity(world_t *w, int type, int index) {
  ddnet_entity_link_t *link = &w->entities[index].link;

  /* not in the list */
  if (link->next == -1 && link->prev == -1 && w->first_entity[type] != index)
    return;

  /* remove */
  if (link->prev != -1)
    w->entities[link->prev].link.next = link->next;
  else
    w->first_entity[type] = link->next;
  if (link->next != -1)
    w->entities[link->next].link.prev = link->prev;

  /* keep list traversing valid */
  if (w->next_traverse_entity.type == type && w->next_traverse_entity.index == index)
    w->next_traverse_entity.index = link->next;

  link->next = -1;
  link->prev = -1;
}

int world_new_entity(world_t *w, ddnet_entity_kind_t kind, vec2 pos) {
  int index;
  if (w->first_free_entity != -1) {
    index = w->first_free_entity;
    w->first_free_entity = w->entities[index].link.next;
  } else {
    if (w->num_entities == w->entity_capacity) {
      int capacity = w->entity_capacity ? w->entity_capacity * 2 : 64;
      entity_t *entities = realloc(w->entities, (size_t)capacity * sizeof(*entities));
      if (!entities)
        return -1;
      w->entities = entities;
      w->entity_capacity = capacity;
    }
    index = w->num_entities++;
  }

  /* the server allocates entities zeroed */
  entity_t *ent = &w->entities[index];
  memset(ent, 0, sizeof(*ent));
  ent->kind = kind;
  ent->link.prev = -1;
  ent->link.next = -1;
  ent->pos = pos;
  return index;
}

/* CEntity::Destroy: the entity was already removed from its list. */
static void world_free_entity(world_t *w, int index) {
  entity_t *ent = &w->entities[index];
  ent->kind = DDNET_ENTITY_FREE;
  ent->link.prev = -1;
  ent->link.next = w->first_free_entity;
  w->first_free_entity = index;
}

/* The traversal pointer is shared by every loop over the entity lists, like
 * CGameWorld::m_pNextTraverseEntity. */
static int traverse_begin(world_t *w, int type, int index) {
  w->next_traverse_entity.type = type;
  w->next_traverse_entity.index = w->entities[index].link.next;
  return index;
}

/* CGameWorld::RemoveEntities: destroy objects marked for destruction */
static void world_remove_entities(world_t *w) {
  for (int type = 0; type < DDNET_NUM_ENTTYPES; type++) {
    for (int index = w->first_entity[type]; index != -1;) {
      traverse_begin(w, type, index);
      if (entity(w, index)->marked_for_destroy) {
        world_remove_entity(w, type, index);
        world_free_entity(w, index);
      }
      index = w->next_traverse_entity.index;
    }
  }
}

/* CGameWorld::FindEntities for the character list, which holds the tee while
 * it is alive. */
bool world_find_character(world_t *w, vec2 pos, float radius) {
  return w->player.has_character && w->character.alive && vdistance(w->character.pos, pos) < radius + PHYSICAL_SIZE;
}

/* CGameWorld::Tick: the projectiles, the pickups, then the tee (its list comes
 * last, after the ones of lasers and flags, which are empty). */
static void world_tick_entities(world_t *w) {
  for (int type = 0; type < DDNET_NUM_ENTTYPES; type++) {
    for (int index = w->first_entity[type]; index != -1;) {
      traverse_begin(w, type, index);
      entity_tick(w, index);
      index = w->next_traverse_entity.index;
    }
  }
  if (get_player_char(w))
    character_tick(w, &w->character);

  /* only characters do something in their deferred tick */
  if (get_player_char(w))
    character_tick_deferred(w, &w->character);

  world_remove_entities(w);
}

/* -------------------------------------------------------- entity helpers */

/* CEntity::GameLayerClipped */
bool game_layer_clipped(const world_t *w, vec2 check_pos) {
  return round_to_int(check_pos.x) / 32 < -200 ||
         round_to_int(check_pos.x) / 32 > collision(w)->width + 200 ||
         round_to_int(check_pos.y) / 32 < -200 || round_to_int(check_pos.y) / 32 > collision(w)->height + 200;
}

/* CGameContext::CreateExplosion of a grenade of the tee, which only it can
 * feel. */
void create_explosion(world_t *w, vec2 pos, int weapon) {
  /* create the event */
  create_particle(w, pos, DDNET_PARTICLE_EXPLOSION);

  /* deal damage */
  float radius = 135.0f;
  float inner_radius = 48.0f;
  if (!world_find_character(w, pos, radius))
    return;
  character_t *chr = &w->character;
  vec2 diff = vsub(chr->pos, pos);
  vec2 force_dir = v2(0, 1);
  float l = vlength(diff);
  if (l)
    force_dir = vnormalize(diff);
  l = 1 - clampf((l - inner_radius) / (radius - inner_radius), 0.0f, 1.0f);
  float strength;
  if (!w->player.tune_zone)
    strength = ddnet_tune(global_tuning(w)->explosion_strength);
  else
    strength = ddnet_tune(tuning_list(w)[w->player.tune_zone].explosion_strength);

  float dmg = strength * l;
  if (!(int)dmg)
    return;

  /* (the owner is the tee: it is always hit) */
  chr->hit_num += (int)dmg;
  character_take_damage(w, chr, vscale(vscale(force_dir, dmg), 2), (int)dmg, weapon);
}

/* --------------------------------------------------------------- players */

/* CPlayer::KillCharacter */
void player_kill_character(world_t *w, int weapon) {
  player_t *player = &w->player;
  if (player->has_character) {
    character_die(w, &w->character, weapon);
    player->has_character = false;
  }
}

/* CPlayer::Respawn */
void player_respawn(world_t *w) { w->player.spawning = true; }

/* CPlayer::TryRespawn */
static void player_try_respawn(world_t *w) {
  player_t *player = &w->player;
  vec2 spawn_pos;

  if (!controller_can_spawn(w, &spawn_pos))
    return;

  player->spawning = false;
  player->view_pos = spawn_pos;
  character_spawn(w, spawn_pos);
  create_particle(w, spawn_pos, DDNET_PARTICLE_PLAYER_SPAWN);
}

/* CPlayer::Tick */
static void player_tick(world_t *w) {
  player_t *player = &w->player;

  int earliest_respawn_tick = player->previous_die_tick + SERVER_TICK_SPEED * 3;
  int respawn_tick = maxi(player->die_tick, earliest_respawn_tick) + 2;
  if (!player->has_character && respawn_tick <= server_tick(w))
    player->spawning = true;

  if (player->has_character) {
    if (w->character.alive)
      player->view_pos = w->character.pos;
    else
      player->has_character = false; /* the character died during this tick */
  } else if (player->spawning) {
    player_try_respawn(w);
  }

  /* determine needed tunings with viewpos */
  int current_index = collision_get_map_index(collision(w), player->view_pos);
  player->tune_zone = collision_is_tune(collision(w), current_index);
}

/* CPlayer::OnPredictedEarlyInput */
static void player_on_predicted_early_input(world_t *w, const input_t *new_input) {
  player_t *player = &w->player;
  player->player_flags = new_input->player_flags;

  if (!player->has_character && (new_input->fire & 1))
    player->spawning = true;

  /* skip the input if chat is active */
  if (player->player_flags & DDNET_PLAYERFLAG_CHATTING)
    return;

  if (player->has_character && !(player->player_flags & DDNET_PLAYERFLAG_SPEC_CAM))
    character_on_direct_input(w, &w->character, new_input);
}

/* CPlayer::OnPredictedInput */
static void player_on_predicted_input(world_t *w, const input_t *new_input) {
  player_t *player = &w->player;
  /* skip the input if chat is active */
  if ((player->player_flags & DDNET_PLAYERFLAG_CHATTING) && (new_input->player_flags & DDNET_PLAYERFLAG_CHATTING))
    return;

  if (player->has_character && !(new_input->player_flags & DDNET_PLAYERFLAG_SPEC_CAM))
    character_on_predicted_input(w, &w->character, new_input);
}

/* CPlayer::TryRespawn right away, for a player that is to spawn (see world.h) */
bool ddnet_player_spawn(ddnet_world_t *w) {
  player_t *player = &w->player;
  if (!player->has_character && player->spawning)
    player_try_respawn(w);
  return player->has_character;
}

/* The kill delay of Unique Race (half a second, for grenade lineups). */
static bool player_kill_delayed(const world_t *w) {
  return w->player.last_kill && w->player.last_kill + SERVER_TICK_SPEED / 2 > server_tick(w);
}

/* CGameContext::OnKillNetMessage, without the kill protection */
bool ddnet_player_kill(ddnet_world_t *w) {
  if (player_kill_delayed(w))
    return false;
  if (!get_player_char(w))
    return false;

  w->player.last_kill = server_tick(w);
  player_kill_character(w, WEAPON_SELF);
  player_respawn(w);
  return true;
}

/* CGameContext::ConRed and CGameContext::ConBlue */
bool ddnet_player_set_fastcap_side(ddnet_world_t *w, int side) {
  if (w->config.gamemode == DDNET_MODE_UNIQUE_RACE || (side != 1 && side != 2))
    return false;

  w->player.fastcap_side = side;

  if (player_kill_delayed(w))
    return false;
  if (!get_player_char(w))
    return false;
  w->player.last_kill = server_tick(w);
  player_kill_character(w, WEAPON_SELF);
  player_respawn(w);
  return true;
}

/* ------------------------------------------------------------------ tick */

/* CGameContext::OnTick */
static void game_on_tick(world_t *w) {
  world_tick_entities(w);

  /* (CGameControllerUnique::Tick: nothing that a team of one needs) */

  player_tick(w);
}

void ddnet_world_sync(ddnet_world_t *w) { (void)w; }
void ddnet_world_changed(ddnet_world_t *w) { (void)w; }

/* The reference implementation reads world->tuning directly. */
ddnet_tuning_t *ddnet_world_tuning_edit(ddnet_world_t *w) { return w->tuning; }

void ddnet_world_tuning_changed(ddnet_world_t *w) { (void)w; }

/* One iteration of the tick loop in CServer::Run. */
void ddnet_world_tick(ddnet_world_t *w) {
  /* Before anything of the tick, the hits of the tee wear off by one: the
   * hits of the tick add to what is left (ddnet_character_t::hit_num). And
   * what its hook gave comes from the tick alone
   * (ddnet_character_core_t::hook_accel). */
  if (w->character.hit_num > 0)
    w->character.hit_num--;
  w->character.core.hook_accel = (vec2){0, 0};

  /* The input for the upcoming tick is handed to the game twice. First
   * before the tick counter advances: this is when weapons fire. */
  player_on_predicted_early_input(w, &w->player.input);

  w->tick++;

  /* apply new input */
  player_on_predicted_input(w, &w->player.input);

  game_on_tick(w);
}

/* ------------------------------------------------------------- lifecycle */

/* One console command from the settings of the map. */
static void world_execute_map_command(void *user, const char *name, char *args) {
  world_t *w = user;
  char *argv[DDNET_CONSOLE_MAX_ARGS];

  if (strcasecmp(name, "tune") == 0) {
    /* CGameContext::ConTuneParam */
    if (ddnet_console_parse_args(args, "s ?f", argv) == 2)
      ddnet_tuning_set_by_name(global_tuning(w), argv[0], strtod(argv[1], NULL));
  } else if (strcasecmp(name, "tune_zone") == 0) {
    /* CGameContext::ConTuneZone */
    if (ddnet_console_parse_args(args, "i s f", argv) == 3) {
      int list = (int)strtol(argv[0], NULL, 10);
      if (list >= 0 && list < w->num_tune_zones)
        ddnet_tuning_set_by_name(&tuning_list(w)[list], argv[1], strtod(argv[2], NULL));
    }
  } else {
    if (ddnet_console_parse_args(args, "?i", argv) == 1)
      ddnet_config_set_from_map(&w->config, name, (int)strtol(argv[0], NULL, 10));
  }
}

/* CGameContext::OnInit, and the player connecting */
bool ddnet_world_init(ddnet_world_t *w, const ddnet_collision_t *col, const ddnet_config_t *config) {
  memset(w, 0, sizeof(*w));
  w->collision = col;
  w->config = *config;
  w->first_free_entity = -1;
  for (int i = 0; i < DDNET_NUM_ENTTYPES; i++)
    w->first_entity[i] = -1;
  w->next_traverse_entity.index = -1;

  /* Only the tune zones that exist in the map can ever be looked up. */
  w->num_tune_zones = col->highest_tune_zone + 1;
  w->tuning = calloc((size_t)w->num_tune_zones, sizeof(*w->tuning));
  if (!w->tuning)
    goto fail;

  /* Reset Tunezones */
  for (int i = 0; i < w->num_tune_zones; i++)
    ddnet_tuning_init_ddrace(&w->tuning[i]);

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
    ddnet_tuning_set_by_name(&w->tuning[i], "player_collision", 0);
    ddnet_tuning_set_by_name(&w->tuning[i], "player_hooking", 0);
  }

  /* create all entities from the game layer */
  if (!controller_create_all_entities(w))
    goto fail;

  /* CPlayer::CPlayer and CPlayer::Reset */
  player_t *player = &w->player;
  player->die_tick = server_tick(w);
  player->previous_die_tick = player->die_tick;
  player->fastcap_side = 1;
  player->finish_tick = -1;

  /* IGameController::OnPlayerConnect */
  player_respawn(w);
  return true;

fail:
  ddnet_world_free(w);
  return false;
}

void ddnet_world_free(ddnet_world_t *w) {
  free(w->tuning);
  free(w->entities);
  memset(w, 0, sizeof(*w));
}

bool ddnet_world_copy(ddnet_world_t *dst, const ddnet_world_t *src) {
  if (dst == src)
    return true;

  /* keep the buffers of dst */
  ddnet_world_t old = *dst;
  *dst = *src;
  dst->tuning = old.tuning;
  dst->entities = old.entities;
  dst->entity_capacity = old.entity_capacity;

  if (!dst->tuning || old.num_tune_zones != src->num_tune_zones) {
    ddnet_tuning_t *tuning = realloc(dst->tuning, (size_t)src->num_tune_zones * sizeof(*tuning));
    if (!tuning)
      goto fail;
    dst->tuning = tuning;
  }
  memcpy(dst->tuning, src->tuning, (size_t)src->num_tune_zones * sizeof(*dst->tuning));

  if (dst->entity_capacity < src->num_entities) {
    entity_t *entities = realloc(dst->entities, (size_t)src->entity_capacity * sizeof(*entities));
    if (!entities)
      goto fail;
    dst->entities = entities;
    dst->entity_capacity = src->entity_capacity;
  }
  if (src->num_entities)
    memcpy(dst->entities, src->entities, (size_t)src->num_entities * sizeof(*dst->entities));
  return true;

fail:
  ddnet_world_free(dst);
  return false;
}
