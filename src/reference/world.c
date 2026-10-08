/* The world: what CGameWorld, CGameContext, CPlayer and the tick loop of the
 * engine server (CServer::Run) do for the physics. */
#include "internal.h"

#include "../common/console.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------------------------------------------------- teleporters */

/* DDNet picks the exit of a teleporter with several exits at random
 * (CWorldCore::RandomOr0). Here the caller decides. */
int world_pick_tele_out(const world_t *w, int client_id, int count) {
  if (count <= 1)
    return 0;
  int tele_out = client_id >= 0 && client_id < MAX_CLIENTS ? w->characters[client_id].tele_out : w->tele_out;
  return (int)((unsigned)tele_out % (unsigned)count);
}

/* ------------------------------------------------------------- accessors */

character_t *get_player_char(world_t *w, int client_id) {
  if (client_id < 0 || client_id >= MAX_CLIENTS || !w->players[client_id].active)
    return NULL;
  /* CPlayer::GetCharacter */
  if (w->players[client_id].has_character && w->characters[client_id].alive)
    return &w->characters[client_id];
  return NULL;
}

core_t *world_core(world_t *w, int client_id) {
  if (w->players[client_id].has_character && w->characters[client_id].alive &&
      !w->characters[client_id].paused)
    return &w->characters[client_id].core;
  return NULL;
}

int ddnet_character_angle(const ddnet_character_t *chr) { return chr->core.angle; }

ddnet_character_t *ddnet_world_character(ddnet_world_t *world, int client_id) {
  return get_player_char(world, client_id);
}

void ddnet_character_changed(ddnet_world_t *world, int client_id) {
  (void)world;
  (void)client_id;
}

/* ---------------------------------------------------------- entity lists */

static ddnet_entity_link_t *link_of(world_t *w, int type, int index) {
  return type == DDNET_ENTTYPE_CHARACTER ? &w->characters[index].link : &w->entities[index].link;
}

/* CGameWorld::InsertEntity: new entities go to the front of their list. */
void world_insert_entity(world_t *w, int type, int index) {
  ddnet_entity_link_t *link = link_of(w, type, index);
  if (w->first_entity[type] != -1)
    link_of(w, type, w->first_entity[type])->prev = index;
  link->next = w->first_entity[type];
  link->prev = -1;
  w->first_entity[type] = index;
}

/* CGameWorld::RemoveEntity */
void world_remove_entity(world_t *w, int type, int index) {
  ddnet_entity_link_t *link = link_of(w, type, index);

  /* not in the list */
  if (link->next == -1 && link->prev == -1 && w->first_entity[type] != index)
    return;

  /* remove */
  if (link->prev != -1)
    link_of(w, type, link->prev)->next = link->next;
  else
    w->first_entity[type] = link->next;
  if (link->next != -1)
    link_of(w, type, link->next)->prev = link->prev;

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

  /* DDNet allocates entities zeroed */
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
 * CGameWorld::m_pNextTraverseEntity. A loop that runs inside of another one
 * (world_remove_entities_from_player is reached from a dying character)
 * overwrites it, which cuts the outer loop short. That is upstream behavior
 * and part of the physics. */
static int traverse_begin(world_t *w, int type, int index) {
  w->next_traverse_entity.type = type;
  w->next_traverse_entity.index = link_of(w, type, index)->next;
  return index;
}

/* CGameWorld::RemoveEntitiesFromPlayers */
void world_remove_entities_from_player(world_t *w, int player_id) {
  for (int type = 0; type < DDNET_NUM_ENTTYPES; type++) {
    for (int index = w->first_entity[type]; index != -1;) {
      traverse_begin(w, type, index);
      /* characters do not have an owner */
      if (type != DDNET_ENTTYPE_CHARACTER && entity_owner_id(entity(w, index)) == player_id) {
        world_remove_entity(w, type, index);
        world_free_entity(w, index);
      }
      index = w->next_traverse_entity.index;
    }
  }
}

/* CGameWorld::RemoveEntities: destroy objects marked for destruction */
static void world_remove_entities(world_t *w) {
  for (int type = 0; type < DDNET_NUM_ENTTYPES; type++) {
    if (type == DDNET_ENTTYPE_CHARACTER)
      continue; /* characters are never marked */
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

/* CGameWorld::FindEntities */
int world_find_characters(world_t *w, vec2 pos, float radius, int *ids, int max) {
  int num = 0;
  for (int id = w->first_entity[DDNET_ENTTYPE_CHARACTER]; id != -1; id = w->characters[id].link.next) {
    if (vdistance(w->characters[id].pos, pos) < radius + PHYSICAL_SIZE) {
      if (ids)
        ids[num] = id;
      num++;
      if (num == max)
        break;
    }
  }
  return num;
}

/* CGameWorld::IntersectCharacter: the first character on the line from pos0
 * to pos1. */
character_t *world_intersect_character(world_t *w, vec2 pos0, vec2 pos1, float radius, vec2 *new_pos,
                                       const character_t *not_this, int collide_with,
                                       const character_t *this_only) {
  float closest_len = vdistance(pos0, pos1) * 100.0f;
  character_t *closest = NULL;

  for (int id = w->first_entity[DDNET_ENTTYPE_CHARACTER]; id != -1; id = w->characters[id].link.next) {
    character_t *chr = &w->characters[id];
    if (chr == not_this)
      continue;

    if (this_only && chr != this_only)
      continue;

    if (collide_with != -1 && !character_can_collide(w, chr, collide_with))
      continue;

    vec2 intersect_pos;
    if (closest_point_on_line(pos0, pos1, chr->pos, &intersect_pos)) {
      float len = vdistance(chr->pos, intersect_pos);
      if (len < PHYSICAL_SIZE + radius) {
        len = vdistance(pos0, intersect_pos);
        if (len < closest_len) {
          *new_pos = intersect_pos;
          closest_len = len;
          closest = chr;
        }
      }
    }
  }

  return closest;
}

/* CGameWorld::ReleaseHooked */
void world_release_hooked(world_t *w, int client_id) {
  for (int id = w->first_entity[DDNET_ENTTYPE_CHARACTER]; id != -1; id = w->characters[id].link.next) {
    character_t *chr = &w->characters[id];
    if (chr->core.hooked_player == client_id)
      character_release_hook(w, chr);
  }
}

/* CGameWorld::Tick */
static void world_tick_entities(world_t *w) {
  /* update all objects */
  for (int type = 0; type < DDNET_NUM_ENTTYPES; type++) {
    /* It's important to call PreTick() and Tick() after each other.
     * If we call PreTick() before, and Tick() after other entities have been processed, it causes physics
     * changes such as a stronger shotgun or grenade. */
    if (w->config.sv_no_weak_hook && type == DDNET_ENTTYPE_CHARACTER) {
      for (int index = w->first_entity[type]; index != -1;) {
        traverse_begin(w, type, index);
        character_pre_tick(w, &w->characters[index]);
        index = w->next_traverse_entity.index;
      }
    }

    for (int index = w->first_entity[type]; index != -1;) {
      traverse_begin(w, type, index);
      if (type == DDNET_ENTTYPE_CHARACTER)
        character_tick(w, &w->characters[index]);
      else
        entity_tick(w, index);
      index = w->next_traverse_entity.index;
    }
  }

  /* only characters do something in their deferred tick */
  for (int index = w->first_entity[DDNET_ENTTYPE_CHARACTER]; index != -1;) {
    traverse_begin(w, DDNET_ENTTYPE_CHARACTER, index);
    character_tick_deferred(w, &w->characters[index]);
    index = w->next_traverse_entity.index;
  }

  world_remove_entities(w);

  /* find the characters' strong/weak id */
  int strong_weak_id = 0;
  for (int id = w->first_entity[DDNET_ENTTYPE_CHARACTER]; id != -1; id = w->characters[id].link.next) {
    w->characters[id].strong_weak_id = strong_weak_id;
    strong_weak_id++;
  }
}

/* -------------------------------------------------------- entity helpers */

/* CEntity::GameLayerClipped */
bool game_layer_clipped(const world_t *w, vec2 check_pos) {
  return round_to_int(check_pos.x) / 32 < -200 ||
         round_to_int(check_pos.x) / 32 > collision(w)->width + 200 ||
         round_to_int(check_pos.y) / 32 < -200 || round_to_int(check_pos.y) / 32 > collision(w)->height + 200;
}

/* CEntity::GetNearestAirPos */
bool get_nearest_air_pos(const world_t *w, vec2 pos, vec2 prev_pos, vec2 *out_pos) {
  const collision_t *col = collision(w);
  for (int k = 0; k < 16 && collision_check_point(col, pos.x, pos.y); k++)
    pos = vsub(pos, vnormalize(vsub(prev_pos, pos)));

  vec2 pos_in_block = v2(round_to_int(pos.x) % 32, round_to_int(pos.y) % 32);
  vec2 block_center =
      vadd(vsub(v2(round_to_int(pos.x), round_to_int(pos.y)), pos_in_block), v2(16.0f, 16.0f));

  *out_pos = v2(block_center.x + (pos_in_block.x < 16 ? -2.0f : 1.0f), pos.y);
  if (!collision_test_box(col, *out_pos, PHYSICAL_SIZE_VEC2))
    return true;

  *out_pos = v2(pos.x, block_center.y + (pos_in_block.y < 16 ? -2.0f : 1.0f));
  if (!collision_test_box(col, *out_pos, PHYSICAL_SIZE_VEC2))
    return true;

  *out_pos = v2(block_center.x + (pos_in_block.x < 16 ? -2.0f : 1.0f),
                block_center.y + (pos_in_block.y < 16 ? -2.0f : 1.0f));
  return !collision_test_box(col, *out_pos, PHYSICAL_SIZE_VEC2);
}

/* CEntity::GetNearestAirPosPlayer */
bool get_nearest_air_pos_player(const world_t *w, vec2 player_pos, vec2 *out_pos) {
  for (int distance = 5; distance >= -1; distance--) {
    *out_pos = v2(player_pos.x, player_pos.y - distance);
    if (!collision_test_box(collision(w), *out_pos, PHYSICAL_SIZE_VEC2))
      return true;
  }
  return false;
}

/* CGameContext::CreateExplosion */
void create_explosion(world_t *w, vec2 pos, int owner, int weapon, bool no_damage, int activated_team) {
  /* create the event */
  create_particle(w, pos, DDNET_PARTICLE_EXPLOSION, owner);

  /* deal damage */
  int ents[MAX_CLIENTS];
  float radius = 135.0f;
  float inner_radius = 48.0f;
  int num = world_find_characters(w, pos, radius, ents, MAX_CLIENTS);
  bool team_mask[NUM_DDRACE_TEAMS];
  for (int i = 0; i < NUM_DDRACE_TEAMS; i++)
    team_mask[i] = true;
  for (int i = 0; i < num; i++) {
    character_t *chr = &w->characters[ents[i]];
    vec2 diff = vsub(chr->pos, pos);
    vec2 force_dir = v2(0, 1);
    float l = vlength(diff);
    if (l)
      force_dir = vnormalize(diff);
    l = 1 - clampf((l - inner_radius) / (radius - inner_radius), 0.0f, 1.0f);
    float strength;
    if (owner == -1 || !w->players[owner].active || !w->players[owner].tune_zone)
      strength = ddnet_tune(global_tuning(w)->explosion_strength);
    else
      strength = ddnet_tune(tuning_list(w)[w->players[owner].tune_zone].explosion_strength);

    float dmg = strength * l;
    if (!(int)dmg)
      continue;

    character_t *owner_char = get_player_char(w, owner);
    if ((owner_char ? !owner_char->core.grenade_hit_disabled : w->config.sv_hit) || no_damage ||
        owner == character_id(chr)) {
      if (owner != -1 && chr->alive && !character_can_collide(w, chr, owner))
        continue;
      if (owner == -1 && activated_team != -1 && chr->alive && character_team(w, chr) != activated_team)
        continue;

      /* Explode at most once per team */
      int player_team = character_team(w, chr);
      if ((owner_char ? owner_char->core.grenade_hit_disabled : !w->config.sv_hit) || no_damage) {
        if (!team_mask[player_team])
          continue;
        team_mask[player_team] = false;
      }

      character_take_damage(w, chr, vscale(vscale(force_dir, dmg), 2), (int)dmg, owner, weapon);
    }
  }
}

/* --------------------------------------------------------------- players */

/* CPlayer::KillCharacter */
void player_kill_character(world_t *w, int client_id, int weapon) {
  player_t *player = &w->players[client_id];
  if (player->has_character) {
    character_die(w, &w->characters[client_id], client_id, weapon);
    player->has_character = false;
  }
}

/* CPlayer::Respawn */
void player_respawn(world_t *w, int client_id, bool weak_hook) {
  w->players[client_id].weak_hook_spawn = weak_hook;
  w->players[client_id].spawning = true;
}

/* CPlayer::TryRespawn */
static void player_try_respawn(world_t *w, int client_id) {
  player_t *player = &w->players[client_id];
  vec2 spawn_pos;

  if (!controller_can_spawn(w, &spawn_pos, client_id))
    return;

  player->weak_hook_spawn = false;
  player->spawning = false;
  player->view_pos = spawn_pos;
  character_spawn(w, client_id, spawn_pos);
  create_particle(w, spawn_pos, DDNET_PARTICLE_PLAYER_SPAWN, client_id);

  if (w->config.sv_team == DDNET_SV_TEAM_FORCED_SOLO)
    character_set_solo(w, &w->characters[client_id], true);
}

/* CPlayer::ProcessPause: a tee is only taken out of the world for /spec once
 * it stands still on the ground (CPlayer::CanSpec). */
static void player_process_pause(world_t *w, int client_id) {
  player_t *player = &w->players[client_id];
  character_t *chr = &w->characters[client_id];
  if (player->paused == DDNET_PAUSE_SPEC && !chr->paused && character_is_grounded(w, chr) &&
      veq(chr->pos, chr->prev_pos)) {
    character_pause(w, chr, true);
    create_particle(w, chr->pos, DDNET_PARTICLE_PLAYER_DEATH, client_id);
    create_sound(w, chr->pos, DDNET_SOUND_PLAYER_DIE, client_id);
  }
}

/* CPlayer::Pause */
int player_pause(world_t *w, int client_id, int state, bool force) {
  player_t *player = &w->players[client_id];
  if (!player->has_character)
    return 0;

  if (state != player->paused) {
    character_t *chr = &w->characters[client_id];
    /* Get to wanted state */
    if (state == DDNET_PAUSE_PAUSED || state == DDNET_PAUSE_NONE) {
      if (chr->paused) {
        if (!force && player->last_pause &&
            player->last_pause + (int64_t)w->config.sv_pause_frequency * SERVER_TICK_SPEED > server_tick(w))
          return player->paused; /* Can't /spec that quickly. */
        character_pause(w, chr, false);
        player->view_pos = chr->pos;
        create_particle(w, chr->pos, DDNET_PARTICLE_PLAYER_SPAWN, client_id);
      }
    }

    /* Update state */
    player->paused = state;
    player->last_pause = server_tick(w);
  }

  return player->paused;
}

/* The /spec chat command (CGameContext::ConToggleSpec) */
static void player_toggle_spec(world_t *w, int client_id) {
  player_t *player = &w->players[client_id];
  int pause_type = w->config.sv_pauseable ? DDNET_PAUSE_SPEC : DDNET_PAUSE_PAUSED;

  /* ToggleSpecPause */
  if (player->paused != DDNET_PAUSE_NONE)
    player_pause(w, client_id, DDNET_PAUSE_NONE, false);
  else
    player_pause(w, client_id, pause_type, false);
}

/* CPlayer::Tick */
static void player_tick(world_t *w, int client_id) {
  player_t *player = &w->players[client_id];

  int earliest_respawn_tick = player->previous_die_tick + SERVER_TICK_SPEED * 3;
  int respawn_tick = maxi(player->die_tick, earliest_respawn_tick) + 2;
  if (!player->has_character && respawn_tick <= server_tick(w))
    player->spawning = true;

  if (player->has_character) {
    character_t *chr = &w->characters[client_id];
    if (chr->alive) {
      player_process_pause(w, client_id);
      if (!player->paused)
        player->view_pos = chr->pos;
    } else if (!chr->paused) {
      player->has_character = false; /* the character died during this tick */
    }
  } else if (player->spawning && !player->weak_hook_spawn) {
    player_try_respawn(w, client_id);
  }

  /* determine needed tunings with viewpos */
  int current_index = collision_get_map_index(collision(w), player->view_pos);
  player->tune_zone = collision_is_tune(collision(w), current_index);
}

/* CPlayer::PostPostTick */
static void player_post_post_tick(world_t *w, int client_id) {
  player_t *player = &w->players[client_id];
  if (!player->has_character && player->spawning && player->weak_hook_spawn)
    player_try_respawn(w, client_id);
}

/* CPlayer::OnPredictedEarlyInput */
static void player_on_predicted_early_input(world_t *w, int client_id, const input_t *new_input) {
  player_t *player = &w->players[client_id];
  player->player_flags = new_input->player_flags;

  if (!player->has_character && (new_input->fire & 1))
    player->spawning = true;

  /* skip the input if chat is active */
  if (player->player_flags & DDNET_PLAYERFLAG_CHATTING)
    return;

  if (player->has_character && !player->paused && !(player->player_flags & DDNET_PLAYERFLAG_SPEC_CAM))
    character_on_direct_input(w, &w->characters[client_id], new_input);
}

/* CPlayer::OnPredictedInput */
static void player_on_predicted_input(world_t *w, int client_id, const input_t *new_input) {
  player_t *player = &w->players[client_id];
  /* skip the input if chat is active */
  if ((player->player_flags & DDNET_PLAYERFLAG_CHATTING) &&
      (new_input->player_flags & DDNET_PLAYERFLAG_CHATTING))
    return;

  if (player->has_character && !player->paused && !(new_input->player_flags & DDNET_PLAYERFLAG_SPEC_CAM))
    character_on_predicted_input(w, &w->characters[client_id], new_input);
}

bool ddnet_player_join(ddnet_world_t *w, int client_id) {
  if (client_id < 0 || client_id >= MAX_CLIENTS || w->players[client_id].active)
    return false;

  /* CPlayer::CPlayer and CPlayer::Reset */
  player_t *player = &w->players[client_id];
  memset(player, 0, sizeof(*player));
  player->active = true;
  player->die_tick = server_tick(w);
  player->previous_die_tick = player->die_tick;
  player->unique_id = w->next_unique_id++;
  player->finish_tick = -1;
  if (client_id >= w->num_clients)
    w->num_clients = client_id + 1;

  /* IGameController::OnPlayerConnect */
  player_respawn(w, client_id, false);
  return true;
}

/* CPlayer::TryRespawn right away, for a player that is to spawn (see world.h) */
bool ddnet_player_spawn(ddnet_world_t *w, int client_id) {
  if (client_id < 0 || client_id >= MAX_CLIENTS || !w->players[client_id].active)
    return false;
  player_t *player = &w->players[client_id];
  if (!player->has_character && player->spawning && !player->weak_hook_spawn)
    player_try_respawn(w, client_id);
  return player->has_character;
}

void ddnet_player_leave(ddnet_world_t *w, int client_id) {
  if (client_id < 0 || client_id >= MAX_CLIENTS || !w->players[client_id].active)
    return;

  /* CGameControllerDDNet::OnPlayerDisconnect */
  player_kill_character(w, client_id, WEAPON_GAME);
  if (w->config.sv_team != DDNET_SV_TEAM_FORCED_SOLO)
    teams_set_force_character_team(w, client_id, TEAM_FLOCK);

  w->players[client_id].active = false;
}

/* CGameContext::OnKillNetMessage, without the kill delay and kill protection */
void ddnet_player_kill(ddnet_world_t *w, int client_id) {
  if (!w->players[client_id].active || w->players[client_id].paused)
    return;
  if (!get_player_char(w, client_id))
    return;
  player_kill_character(w, client_id, WEAPON_SELF);
  player_respawn(w, client_id, false);
}

void ddnet_player_set_team(ddnet_world_t *w, int client_id, int team) {
  if (client_id < 0 || client_id >= MAX_CLIENTS || !teams_is_valid_team_number(team))
    return;
  teams_set_force_character_team(w, client_id, team);
}

/* ------------------------------------------------------------------ tick */

/* CGameContext::OnTick */
static void game_on_tick(world_t *w) {
  world_tick_entities(w);

  /* CGameControllerDDNet::Tick */
  teams_tick(w);

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (w->players[i].active)
      player_tick(w, i);
  }

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (w->players[i].active)
      player_post_post_tick(w, i);
  }

  /* timed switches fall back to their other state */
  for (int s = 0; s < w->num_switchers; s++) {
    ddnet_switcher_t *switcher = &switchers(w)[s];
    for (int j = 0; j < NUM_DDRACE_TEAMS; ++j) {
      if (switcher->end_tick[j] <= server_tick(w) && switcher->type[j] == TILE_SWITCHTIMEDOPEN) {
        switcher->status[j] = false;
        switcher->end_tick[j] = 0;
        switcher->type[j] = TILE_SWITCHCLOSE;
      } else if (switcher->end_tick[j] <= server_tick(w) && switcher->type[j] == TILE_SWITCHTIMEDCLOSE) {
        switcher->status[j] = true;
        switcher->end_tick[j] = 0;
        switcher->type[j] = TILE_SWITCHOPEN;
      }
    }
  }
}

void ddnet_world_sync(ddnet_world_t *w) { (void)w; }
void ddnet_world_changed(ddnet_world_t *w) { (void)w; }

/* The reference implementation reads world->tuning directly. */
/* (each world has tuning of its own here) */
ddnet_tuning_t *ddnet_world_tuning_edit(ddnet_world_t *w) { return w->tuning; }

void ddnet_world_tuning_changed(ddnet_world_t *w) { (void)w; }

/* One iteration of the tick loop in CServer::Run. */
void ddnet_world_tick(ddnet_world_t *w) {
  /* Chat commands arrive between two ticks. */
  for (int c = 0; c < MAX_CLIENTS; c++) {
    if (w->players[c].active && (w->characters[c].spec != 0) != (w->players[c].paused != DDNET_PAUSE_NONE))
      player_toggle_spec(w, c);
  }

  /* The inputs for the upcoming tick are handed to the game twice. First
   * before the tick counter advances: this is when weapons fire. */
  for (int c = 0; c < MAX_CLIENTS; c++) {
    if (w->players[c].active)
      player_on_predicted_early_input(w, c, &w->players[c].input);
  }

  w->tick++;

  /* apply new input */
  for (int c = 0; c < MAX_CLIENTS; c++) {
    if (w->players[c].active)
      player_on_predicted_input(w, c, &w->players[c].input);
  }

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
  } else if (strcasecmp(name, "switch_open") == 0) {
    /* CGameContext::ConSwitchOpen */
    if (ddnet_console_parse_args(args, "i", argv) == 1) {
      int number = (int)strtol(argv[0], NULL, 10);
      if (number >= 0 && number <= w->num_switchers - 1)
        switchers(w)[number].initial = false;
    }
  } else if (strcasecmp(name, "mapbug") == 0) {
    /* CGameContext::ConMapbug */
    if (ddnet_console_parse_args(args, "s", argv) == 1 &&
        strcmp(argv[0], "grenade-doubleexplosion@ddnet.tw") == 0)
      w->config.bug_grenade_double_explosion = true;
  } else {
    if (ddnet_console_parse_args(args, "?i", argv) == 1)
      ddnet_config_set_from_map(&w->config, name, (int)strtol(argv[0], NULL, 10));
  }
}

/* CGameContext::OnInit */
bool ddnet_world_init(ddnet_world_t *w, const ddnet_collision_t *col, const ddnet_config_t *config) {
  memset(w, 0, sizeof(*w));
  w->collision = col;
  w->config = *config;
  w->first_free_entity = -1;
  for (int i = 0; i < DDNET_NUM_ENTTYPES; i++)
    w->first_entity[i] = -1;
  w->next_traverse_entity.index = -1;

  w->players = calloc(MAX_CLIENTS, sizeof(*w->players));
  w->characters = calloc(MAX_CLIENTS, sizeof(*w->characters));
  /* Only the tune zones that exist in the map can ever be looked up. */
  w->num_tune_zones = col->highest_tune_zone + 1;
  w->tuning = calloc((size_t)w->num_tune_zones, sizeof(*w->tuning));
  /* CWorldCore::InitSwitchers */
  w->num_switchers = col->highest_switch_number > 0 ? col->highest_switch_number + 1 : 0;
  w->switchers = calloc((size_t)(w->num_switchers ? w->num_switchers : 1), sizeof(*w->switchers));
  if (!w->players || !w->characters || !w->tuning || !w->switchers)
    goto fail;

  for (int s = 0; s < w->num_switchers; s++) {
    ddnet_switcher_t *switcher = &w->switchers[s];
    switcher->initial = true;
    for (int j = 0; j < NUM_DDRACE_TEAMS; j++) {
      switcher->status[j] = true;
      switcher->end_tick[j] = 0;
      switcher->type[j] = 0;
      switcher->last_update_tick[j] = 0;
    }
  }

  /* Reset Tunezones */
  for (int i = 0; i < w->num_tune_zones; i++)
    ddnet_tuning_init_ddrace(&w->tuning[i]);

  /* CGameContext::LoadMapSettings */
  for (int i = 0; i < col->num_settings; i++)
    ddnet_console_execute_line(col->settings[i], world_execute_map_command, w);

  if (w->config.sv_solo_server) {
    w->config.sv_team = DDNET_SV_TEAM_FORCED_SOLO;

    for (int i = 0; i < w->num_tune_zones; i++) {
      ddnet_tuning_set_by_name(&w->tuning[i], "player_collision", 0);
      ddnet_tuning_set_by_name(&w->tuning[i], "player_hooking", 0);
    }
  }

  /* the game controller is created, with its CGameTeams */
  teams_reset(w);

  /* create all entities from the game layer */
  if (!controller_create_all_entities(w))
    goto fail;
  return true;

fail:
  ddnet_world_free(w);
  return false;
}

void ddnet_world_free(ddnet_world_t *w) {
  free(w->tuning);
  free(w->switchers);
  free(w->players);
  free(w->characters);
  free(w->entities);
  free(w->dragger_targets);
  free(w->gun_timers);
  memset(w, 0, sizeof(*w));
}

/* Copy count elements of size into *buffer, which holds old_count elements.
 * The allocation is reused if the element count did not change. */
static bool copy_buffer(void *buffer_ptr, int old_count, const void *src, int count, size_t size) {
  void *buffer;
  memcpy(&buffer, buffer_ptr, sizeof(buffer));
  if (!buffer || old_count != count) {
    void *resized = realloc(buffer, (size_t)(count ? count : 1) * size);
    if (!resized)
      return false;
    buffer = resized;
    memcpy(buffer_ptr, &buffer, sizeof(buffer));
  }
  if (count)
    memcpy(buffer, src, (size_t)count * size);
  return true;
}

bool ddnet_world_copy(ddnet_world_t *dst, const ddnet_world_t *src) {
  if (dst == src)
    return true;

  /* keep the buffers of dst */
  ddnet_world_t old = *dst;
  *dst = *src;
  dst->tuning = old.tuning;
  dst->switchers = old.switchers;
  dst->players = old.players;
  dst->characters = old.characters;
  dst->entities = old.entities;
  dst->entity_capacity = old.entity_capacity;
  dst->dragger_targets = old.dragger_targets;
  dst->gun_timers = old.gun_timers;

  if (!dst->players) {
    dst->players = calloc(MAX_CLIENTS, sizeof(*dst->players));
    dst->characters = calloc(MAX_CLIENTS, sizeof(*dst->characters));
    if (!dst->players || !dst->characters)
      goto fail;
  }
  /* Only the slots that were ever used need to be copied. Slots that only dst
   * used are emptied. */
  memcpy(dst->players, src->players, (size_t)src->num_clients * sizeof(*dst->players));
  memcpy(dst->characters, src->characters, (size_t)src->num_clients * sizeof(*dst->characters));
  if (old.num_clients > src->num_clients)
    memset(&dst->players[src->num_clients], 0,
           (size_t)(old.num_clients - src->num_clients) * sizeof(*dst->players));

  if (dst->entity_capacity < src->num_entities) {
    entity_t *entities = realloc(dst->entities, (size_t)src->entity_capacity * sizeof(*entities));
    if (!entities)
      goto fail;
    dst->entities = entities;
    dst->entity_capacity = src->entity_capacity;
  }
  if (src->num_entities)
    memcpy(dst->entities, src->entities, (size_t)src->num_entities * sizeof(*dst->entities));

  if (!copy_buffer(&dst->tuning, old.num_tune_zones, src->tuning, src->num_tune_zones,
                   sizeof(*dst->tuning)) ||
      !copy_buffer(&dst->switchers, old.num_switchers, src->switchers, src->num_switchers,
                   sizeof(*dst->switchers)) ||
      !copy_buffer(&dst->dragger_targets, old.num_draggers, src->dragger_targets, src->num_draggers,
                   sizeof(*dst->dragger_targets)) ||
      !copy_buffer(&dst->gun_timers, old.num_guns, src->gun_timers, src->num_guns, sizeof(*dst->gun_timers)))
    goto fail;
  return true;

fail:
  ddnet_world_free(dst);
  return false;
}
