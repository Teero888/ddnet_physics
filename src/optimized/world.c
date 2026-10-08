/* The world: what CGameWorld, CGameContext, CPlayer and the tick loop of the
 * engine server (CServer::Run) do for the physics. */
#include "internal.h"

#include "../common/console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------------------------------------------------- teleporters */

/* DDNet picks the exit of a teleporter with several exits at random
 * (CWorldCore::RandomOr0). Here the caller decides. */
int world_pick_tele_out(const world_t *w, int client_id, int count) {
  if (count <= 1)
    return 0;
  int tele_out = client_id >= 0 && client_id < w->num_clients ? w->characters[client_id].tele_out : w->tele_out;
  return (int)((unsigned)tele_out % (unsigned)count);
}

/* Makes room in players and characters for the client ids below count, with the new slots empty. */
static bool world_reserve_clients(world_t *w, int count) {
  if (count <= w->client_capacity)
    return true;
  int capacity = w->client_capacity ? w->client_capacity : 1;
  while (capacity < count)
    capacity *= 2;
  if (capacity > MAX_CLIENTS)
    capacity = MAX_CLIENTS;
  player_t *players = realloc(w->players, (size_t)capacity * sizeof(*players));
  if (!players)
    return false;
  w->players = players;
  character_t *characters = realloc(w->characters, (size_t)capacity * sizeof(*characters));
  if (!characters)
    return false;
  w->characters = characters;
  memset(&players[w->client_capacity], 0, (size_t)(capacity - w->client_capacity) * sizeof(*players));
  memset(&characters[w->client_capacity], 0, (size_t)(capacity - w->client_capacity) * sizeof(*characters));
  w->client_capacity = capacity;
  return true;
}

/* ------------------------------------------------------------- accessors */

DDNET_HOT character_t *get_player_char(world_t *w, int client_id) {
  if (client_id < 0 || client_id >= w->num_clients || !w->players[client_id].active)
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

/* What CCharacterCore::Tick stores in m_Angle on every tick. */
int ddnet_character_angle(const ddnet_character_t *chr) {
  const core_t *core = &chr->core;
  if (!core->has_ticked)
    return 0;
  /* atan2 is the double version, called with integers */
  float tmp_angle = atan2(core->input.target_y, core->input.target_x);
  if (tmp_angle < -(PI / 2.0f))
    return (int)((tmp_angle + (2.0f * PI)) * 256.0f);
  return (int)(tmp_angle * 256.0f);
}

ddnet_character_t *ddnet_world_character(ddnet_world_t *world, int client_id) {
  return get_player_char(world, client_id);
}

void ddnet_character_changed(ddnet_world_t *w, int client_id) {
  if (client_id < 0 || client_id >= w->num_clients)
    return;
  /* where it is (looked up again), and everything that was worked out from where the tees were: the
   * positions of the tick, the lazy bullets and the bullets that were known to be far from all tees (see
   * world_core_add()) */
  w->characters[client_id].quiet_known = false;
  /* (its team may be another one now, see switch_status()) */
  world_switch_reserve(w, w->teams.team[client_id]);
  w->tee_snapshot = NULL;
  w->lazy_wake = true;
  w->projectile_wakeup = 0;
  w->projectile_parked_slack = -1.0f;
}

/* Keep a sorted list of ids. */
static void id_list_add(uint8_t *ids, int *count, int id) {
  int n = *count;
  for (int i = 0; i < n; i++)
    if (ids[i] == id)
      return;
  while (n > 0 && ids[n - 1] > id) {
    ids[n] = ids[n - 1];
    n--;
  }
  ids[n] = (uint8_t)id;
  (*count)++;
}

static void id_list_remove(uint8_t *ids, int *count, int id) {
  for (int i = 0; i < *count; i++) {
    if (ids[i] == id) {
      for (int j = i + 1; j < *count; j++)
        ids[j - 1] = ids[j];
      (*count)--;
      return;
    }
  }
}

/* CWorldCore::m_apCharacters[client_id] is set or cleared. */
void world_core_add(world_t *w, int client_id) {
  id_list_add(w->core_ids, &w->num_cores, client_id);
  w->tee_snapshot = NULL;
  w->lazy_wake = true;
  w->projectile_wakeup = 0;
  w->projectile_parked_slack = -1.0f; /* the parked ones are compared with the tees */
}
void world_core_remove(world_t *w, int client_id) {
  id_list_remove(w->core_ids, &w->num_cores, client_id);
  w->tee_snapshot = NULL;
  w->lazy_wake = true;
  w->projectile_wakeup = 0;
  w->projectile_parked_slack = -1.0f; /* the parked ones are compared with the tees */
}

/* ---------------------------------------------------------- entity lists */

static ddnet_entity_link_t *link_of(world_t *w, int type, int index) {
  return type == DDNET_ENTTYPE_CHARACTER ? &w->characters[index].link : &entity(w, index)->link;
}

/* CGameWorld::InsertEntity: new entities go to the front of their list. */
DDNET_HOT void world_insert_entity(world_t *w, int type, int index) {
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

DDNET_HOT int world_new_entity(world_t *w, ddnet_entity_kind_t kind, vec2 pos) {
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
  entity_t *ent = entity(w, index);
  memset(ent, 0, sizeof(*ent));
  ent->kind = kind;
  ent->link.prev = -1;
  ent->link.next = -1;
  ent->pos = pos;
  return index;
}

/* CEntity::Destroy: the entity was already removed from its list. */
void world_free_entity(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  if (ent->kind == DDNET_ENTITY_PROJECTILE) {
    projectile_unschedule(w, index);
    projectile_set_erase(w, index);
    if (ent->u.projectile.lazy)
      w->num_lazy_projectiles--;
  }
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
  /* The tees that the loop over the ticks of the tees had still to come to lose their tick, on which DDNet
   * counts down the timer of their pain sound (see character_fire_weapon_switched()). Only that loop leaves
   * a tee here: every loop ends at -1, and of the other loops over the tees, the one over the deferred ticks
   * kills nobody, and the tees the one over CCharacter::PreTick does not come to still tick after it. */
  if (w->next_traverse_entity.type == DDNET_ENTTYPE_CHARACTER && !w->pre_ticking_characters)
    for (int index = w->next_traverse_entity.index; index != -1; index = w->characters[index].link.next)
      w->characters[index].pain_sound_tick++;
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
DDNET_NOINLINE static void world_remove_entities(world_t *w) {
  if (!w->entities_marked)
    return;
  w->entities_marked = false;
  const int num_marked = w->num_marked_entities;
  w->num_marked_entities = 0;
  if (num_marked <= DDNET_MAX_MARKED_ENTITIES) {
    /* the few that are known */
    for (int i = 0; i < num_marked; i++) {
      const int index = w->marked_entities[i];
      const entity_t *ent = entity(w, index);
      /* (it may be gone already, with the entities of its owner) */
      if (ent->kind == DDNET_ENTITY_FREE || !ent->marked_for_destroy)
        continue;
      const int type = ent->kind == DDNET_ENTITY_PROJECTILE
                           ? DDNET_ENTTYPE_PROJECTILE
                           : (ent->kind == DDNET_ENTITY_PICKUP ? DDNET_ENTTYPE_PICKUP : DDNET_ENTTYPE_LASER);
      world_remove_entity(w, type, index);
      world_free_entity(w, index);
    }
    return;
  }
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
/* ------------------------------------------------- map entities that sleep */

/* The range of a turret, dragger or laser wall as half the side of a square
 * around it, or -1 if it is none of them. No tee outside of it matters to it. */
/* Copies of a world share its laser grid, which does not change: the last of them frees it. */
static void laser_grid_release(ddnet_laser_grid_t *grid) {
  if (grid && __atomic_sub_fetch(&grid->refs, 1, __ATOMIC_ACQ_REL) == 0)
    free(grid);
}

static float laser_grid_range(const world_t *w, const entity_t *ent) {
  switch (ent->kind) {
  case DDNET_ENTITY_DRAGGER:
    return maxi(w->config.sv_dragger_range, 0) + 4.0f;
  case DDNET_ENTITY_GUN:
    return maxi(w->config.sv_plasma_range, 0) + PHYSICAL_SIZE + 4.0f;
  case DDNET_ENTITY_LIGHT:
    /* the beam is not longer than this */
    return maxi(ent->u.light.length, absolutei(ent->u.light.curve_length)) + PHYSICAL_SIZE + 4.0f;
  default:
    return -1.0f;
  }
}

static int laser_grid_cell(int tiles, float pos) {
  if (!(pos > 0.0f))
    return 0;
  if (pos >= tiles * 32.0f)
    return (tiles - 1) / DDNET_LASER_GRID_CELL;
  return ((int)pos >> 5) / DDNET_LASER_GRID_CELL;
}

/* The grid for the ranges the config has now, or NULL without memory. */
static const ddnet_laser_grid_t *laser_grid(world_t *w) {
  if (w->laser_grid && w->laser_grid->dragger_range == w->config.sv_dragger_range &&
      w->laser_grid->plasma_range == w->config.sv_plasma_range)
    return w->laser_grid;

  const collision_t *col = collision(w);
  const int cells_x = (col->width - 1) / DDNET_LASER_GRID_CELL + 1;
  const int cells_y = (col->height - 1) / DDNET_LASER_GRID_CELL + 1;
  const int num_cells = cells_x * cells_y;

  /* count, then fill */
  ddnet_laser_grid_t *grid = NULL;
  bool always_all = false;
  for (int pass = 0; pass < 2; pass++) {
    int *counts = calloc((size_t)num_cells + 1, sizeof(*counts));
    if (!counts) {
      free(grid);
      return NULL;
    }
    for (int index = w->first_static_laser; index != -1; index = entity_peek(w, index)->link.next) {
      const entity_t *ent = entity(w, index);
      const float range = laser_grid_range(w, ent);
      vec2 speed = v2(0.0f, 0.0f);
      collision_mover_speed(col, ent->pos.x, ent->pos.y, &speed);
      /* something else: everything has to tick on every tick */
      if (range < 0.0f || !(absolutef(ent->pos.x) < 1e6f) || !(absolutef(ent->pos.y) < 1e6f) ||
          !(range < 1e6f)) {
        always_all = true;
        continue;
      }
      /* on a conveyor: it moves, and ticks on every tick */
      if (vne(speed, v2(0.0f, 0.0f))) {
        if (pass == 1)
          grid->data[grid->movers + counts[num_cells]] = index;
        counts[num_cells]++;
        continue;
      }
      const int x0 = laser_grid_cell(col->width, ent->pos.x - range);
      const int x1 = laser_grid_cell(col->width, ent->pos.x + range);
      const int y0 = laser_grid_cell(col->height, ent->pos.y - range);
      const int y1 = laser_grid_cell(col->height, ent->pos.y + range);
      for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
          const int cell = y * cells_x + x;
          if (pass == 1)
            grid->data[num_cells + 1 + grid->data[cell] + counts[cell]] = index;
          counts[cell]++;
        }
      }
    }
    if (pass == 0) {
      int total = 0;
      for (int cell = 0; cell < num_cells; cell++)
        total += counts[cell];
      const size_t bytes = sizeof(*grid) + ((size_t)num_cells + 1 + total + counts[num_cells]) * sizeof(int);
      grid = malloc(bytes);
      if (!grid) {
        free(counts);
        return NULL;
      }
      grid->bytes = bytes;
      grid->movers = num_cells + 1 + total;
      grid->num_movers = counts[num_cells];
      int offset = 0;
      for (int cell = 0; cell < num_cells; cell++) {
        grid->data[cell] = offset;
        offset += counts[cell];
      }
      grid->data[num_cells] = offset;
    }
    free(counts);
  }
  grid->id = world_new_universe();
  grid->dragger_range = w->config.sv_dragger_range;
  grid->plasma_range = w->config.sv_plasma_range;
  grid->cells_x = cells_x;
  grid->cells_y = cells_y;
  grid->always_all = always_all;
  grid->refs = 1;
  laser_grid_release(w->laser_grid);
  w->laser_grid = grid;
  return grid;
}

/* The loop of CGameWorld::Tick over the laser list, from an entity on. */
static void lasers_tick_from(world_t *w, int index) {
  while (index != -1) {
    traverse_begin(w, DDNET_ENTTYPE_LASER, index);
    entity_tick(w, index);
    index = w->next_traverse_entity.index;
  }
}

/* The laser list: lasers, plasma and dragger beams, which are in front, and
 * behind them the turrets, draggers and laser walls of the map. Of those only
 * the ones that have a tee in their range do something on a tick. */
/* The entities of a cell of the laser grid that matter to a tee at a
 * position, see ddnet_laser_tile_t. False if all of the cell have to tick. */
static bool laser_tile_entities(world_t *w, const ddnet_laser_grid_t *grid, vec2 pos, int begin, int end,
                                const int **ids, int *count) {
  const collision_t *col = collision(w);
  if (!(pos.x >= 0 && pos.y >= 0 && pos.x < col->units_width && pos.y < col->units_height))
    return false;
  if (!w->laser_tiles) {
    w->laser_tiles = calloc(DDNET_LASER_TILE_CACHE_SIZE, sizeof(*w->laser_tiles));
    if (!w->laser_tiles)
      return false;
  }
  const int tile = ((int)pos.y >> 5) * col->width + ((int)pos.x >> 5);
  ddnet_laser_tile_t *entry =
      &w->laser_tiles[((uint32_t)tile * 0x9E3779B1u >> 16) % DDNET_LASER_TILE_CACHE_SIZE];
  if (entry->grid_id != grid->id || entry->tile != tile) {
    const int *entities = &grid->data[grid->cells_x * grid->cells_y + 1];
    const float x0 = (tile % col->width) * 32.0f, y0 = (tile / col->width) * 32.0f;
    entry->grid_id = grid->id;
    entry->tile = tile;
    entry->count = 0;
    for (int i = begin; i < end && entry->count >= 0; i++) {
      const entity_t *ent = &w->entities[entities[i]];
      bool matters = true;
      if (ent->kind == DDNET_ENTITY_DRAGGER || ent->kind == DDNET_ENTITY_GUN) {
        /* how far a tee can be for them to look at it, and a unit for rounding */
        const float range = ent->kind == DDNET_ENTITY_DRAGGER
                                ? maxi(w->config.sv_dragger_range, 0) + 1.0f
                                : maxi(w->config.sv_plasma_range, 0) + PHYSICAL_SIZE + 1.0f;
        /* the closest a position on the tile gets */
        const float dx = maxf(maxf(x0 - ent->pos.x, ent->pos.x - (x0 + 32.0f)), 0.0f);
        const float dy = maxf(maxf(y0 - ent->pos.y, ent->pos.y - (y0 + 32.0f)), 0.0f);
        const int kind = ent->kind == DDNET_ENTITY_GUN ? 0 : (ent->u.dragger.ignore_walls ? 2 : 1);
        if (dx * dx + dy * dy > range * range || collision_tile_out_of_sight(col, kind, ent->pos, tile))
          matters = false;
      }
      if (!matters)
        continue;
      if (entry->count == DDNET_LASER_TILE_MAX)
        entry->count = -1;
      else
        entry->ids[entry->count++] = entities[i];
    }
  }
  if (entry->count < 0)
    return false;
  *ids = entry->ids;
  *count = entry->count;
  return true;
}

DDNET_NOINLINE static void lasers_tick(world_t *w) {
  enum { MAX_AWAKE = 512 };
  const int first = w->first_entity[DDNET_ENTTYPE_LASER];
  if (first == -1)
    return;
  const ddnet_laser_grid_t *grid = w->first_static_laser != -1 ? laser_grid(w) : NULL;
  /* (all of them in the event build, which is shown: a laser wall that sleeps does not turn) */
  if (EVENT_BUILD || !grid || grid->always_all || w->busy_draggers_overflow) {
    lasers_tick_from(w, first);
    return;
  }

  /* everything that was created after the map */
  int index = first;
  while (index != w->first_static_laser && index != -1) {
    const int next = entity_peek(w, index)->link.next;
    traverse_begin(w, DDNET_ENTTYPE_LASER, index);
    entity_tick(w, index);
    /* A loop inside of the tick took over the traversal (see
     * traverse_begin), DDNet goes on from wherever that one ended. */
    if (w->next_traverse_entity.type != DDNET_ENTTYPE_LASER || w->next_traverse_entity.index != next) {
      lasers_tick_from(w, w->next_traverse_entity.index);
      return;
    }
    index = next;
  }

  /* the entities of the map around the tees and the ones that move, in the
   * order of the list: the newest first */
  const int num_cells = grid->cells_x * grid->cells_y;
  const int *entities = &grid->data[num_cells + 1];
  int awake[MAX_AWAKE];
  int num_awake = 0;
  const int *list = awake;
  const collision_t *col = collision(w);
  const bool merge = w->num_cores > 1 || grid->num_movers > 0 || w->busy_draggers > 0;
  if (grid->num_movers + w->busy_draggers > MAX_AWAKE) {
    lasers_tick_from(w, w->first_static_laser);
    return;
  }
  for (int i = 0; i < grid->num_movers; i++)
    awake[num_awake++] = grid->data[grid->movers + i];
  /* A dragger with a target or a beam has to see its tee leave, wherever to. */
  for (int i = 0; i < w->busy_draggers; i++)
    awake[num_awake++] = w->busy_dragger_ids[i];
  for (int n = 0; n < w->num_cores; n++) {
    const vec2 pos = w->characters[w->core_ids[n]].pos;
    const int cell = laser_grid_cell(col->height, pos.y) * grid->cells_x + laser_grid_cell(col->width, pos.x);
    const int begin = grid->data[cell], end = grid->data[cell + 1];
    /* of the cell, the ones a tee on this tile can matter to */
    const int *near = &entities[begin];
    int num_near = end - begin;
    if (num_near > 0)
      laser_tile_entities(w, grid, pos, begin, end, &near, &num_near);
    if (!merge) {
      list = near;
      num_awake = num_near;
      break;
    }
    if (num_awake + num_near > MAX_AWAKE) {
      /* too many to sort out */
      lasers_tick_from(w, w->first_static_laser);
      return;
    }
    for (int i = 0; i < num_near; i++)
      awake[num_awake++] = near[i];
  }
  if (merge) {
    /* descending and without duplicates */
    for (int i = 1; i < num_awake; i++) {
      const int value = awake[i];
      int k = i;
      for (; k > 0 && awake[k - 1] < value; k--)
        awake[k] = awake[k - 1];
      awake[k] = value;
    }
    int unique = 0;
    for (int i = 0; i < num_awake; i++)
      if (unique == 0 || awake[unique - 1] != awake[i])
        awake[unique++] = awake[i];
    num_awake = unique;
  }

  /* a dragger only does something on every seventh tick */
  const bool dragger_tick = server_tick(w) % (int)(SERVER_TICK_SPEED * 0.15f) == 0;
  for (int i = 0; i < num_awake; i++) {
    index = list[i];
    if (!dragger_tick && entity_peek(w, index)->kind == DDNET_ENTITY_DRAGGER)
      continue;
    const int next = entity_peek(w, index)->link.next;
    traverse_begin(w, DDNET_ENTTYPE_LASER, index);
    entity_tick(w, index);
    if (w->next_traverse_entity.type != DDNET_ENTTYPE_LASER || w->next_traverse_entity.index != next) {
      lasers_tick_from(w, w->next_traverse_entity.index);
      return;
    }
  }
  /* where the traversal of the whole list ends */
  w->next_traverse_entity.type = DDNET_ENTTYPE_LASER;
  w->next_traverse_entity.index = -1;
}

static inline void world_tick_entities_end(world_t *w);

/* the ticks of the entities of a list that has nothing faster */
static void entities_tick_list(world_t *w, int type) {
  for (int index = w->first_entity[type]; index != -1;) {
    traverse_begin(w, type, index);
    entity_tick(w, index);
    index = w->next_traverse_entity.index;
  }
}

DDNET_HOT static void world_tick_entities(world_t *w) {
  /* update all objects, list by list */
  _Static_assert(DDNET_ENTTYPE_PROJECTILE == 0 && DDNET_ENTTYPE_LASER == 1 && DDNET_ENTTYPE_PICKUP == 2 &&
                     DDNET_ENTTYPE_FLAG == 3 && DDNET_ENTTYPE_CHARACTER == 4 && DDNET_NUM_ENTTYPES == 5,
                 "the lists in the order of CGameWorld");
  PROF_BEGIN(PROF_PROJECTILES);
  projectiles_tick(w);
  PROF_END(PROF_PROJECTILES);
  if (w->first_entity[DDNET_ENTTYPE_LASER] != -1)
    lasers_tick(w);
  if (!w->pickups_generic)
    pickups_tick_static(w);
  else
    entities_tick_list(w, DDNET_ENTTYPE_PICKUP);
  if (w->first_entity[DDNET_ENTTYPE_FLAG] != -1)
    entities_tick_list(w, DDNET_ENTTYPE_FLAG);

  /* It's important to call PreTick() and Tick() after each other.
   * If we call PreTick() before, and Tick() after other entities have been processed, it causes physics
   * changes such as a stronger shotgun or grenade. */
  tee_snapshot_t snapshot;
  if (w->config.sv_no_weak_hook) {
    snapshot_take(w, &snapshot);
    w->pre_ticking_characters = true;
    for (int index = w->first_entity[DDNET_ENTTYPE_CHARACTER]; index != -1;) {
      traverse_begin(w, DDNET_ENTTYPE_CHARACTER, index);
      character_pre_tick(w, &w->characters[index]);
      snapshot_moved(w, index);
      index = w->next_traverse_entity.index;
    }
    w->pre_ticking_characters = false;
    w->tee_snapshot = NULL;
  }
  snapshot_take(w, &snapshot);
  for (int index = w->first_entity[DDNET_ENTTYPE_CHARACTER]; index != -1;) {
    traverse_begin(w, DDNET_ENTTYPE_CHARACTER, index);
    character_tick(w, &w->characters[index]);
    snapshot_moved(w, index);
    index = w->next_traverse_entity.index;
  }
  w->tee_snapshot = NULL;

  world_tick_entities_end(w);
}

/* CGameWorld::Tick after the ticks of the entities */
static inline void world_tick_entities_end(world_t *w) {
  /* only characters do something in their deferred tick */
  tee_snapshot_t snapshot;
  snapshot_take(w, &snapshot);
  for (int index = w->first_entity[DDNET_ENTTYPE_CHARACTER]; index != -1;) {
    traverse_begin(w, DDNET_ENTTYPE_CHARACTER, index);
    character_tick_deferred(w, &w->characters[index]);
    snapshot_moved(w, index);
    index = w->next_traverse_entity.index;
  }
  w->tee_snapshot = NULL;

  if (w->entities_marked)
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
  return round_fast(check_pos.x) / 32 < -200 || round_fast(check_pos.x) / 32 > collision(w)->width + 200 ||
         round_fast(check_pos.y) / 32 < -200 || round_fast(check_pos.y) / 32 > collision(w)->height + 200;
}

/* CEntity::GetNearestAirPos */
DDNET_COLD bool get_nearest_air_pos(const world_t *w, vec2 pos, vec2 prev_pos, vec2 *out_pos) {
  const collision_t *col = collision(w);
  for (int k = 0; k < 16 && collision_check_point(col, pos.x, pos.y); k++)
    pos = vsub(pos, vnormalize(vsub(prev_pos, pos)));

  vec2 pos_in_block = v2(round_fast(pos.x) % 32, round_fast(pos.y) % 32);
  vec2 block_center = vadd(vsub(v2(round_fast(pos.x), round_fast(pos.y)), pos_in_block), v2(16.0f, 16.0f));

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
  EMIT_PARTICLE(w, pos, DDNET_PARTICLE_EXPLOSION, owner);

  /* deal damage */
  int ents[MAX_CLIENTS];
  float radius = 135.0f;
  float inner_radius = 48.0f;
  int num = world_find_characters(w, pos, radius, ents, MAX_CLIENTS);
  /* nobody is hit */
  if (num == 0)
    return;
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
  EMIT_PARTICLE(w, spawn_pos, DDNET_PARTICLE_PLAYER_SPAWN, client_id);

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
    EMIT_PARTICLE(w, chr->pos, DDNET_PARTICLE_PLAYER_DEATH, client_id);
    EMIT_SOUND(w, chr->pos, DDNET_SOUND_PLAYER_DIE, client_id);
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
        EMIT_PARTICLE(w, chr->pos, DDNET_PARTICLE_PLAYER_SPAWN, client_id);
      }
    }

    /* Update state */
    player->paused = state;
    player->last_pause = server_tick(w);
  }

  return player->paused;
}

/* The /spec chat command (CGameContext::ConToggleSpec) */
DDNET_COLD static void player_toggle_spec(world_t *w, int client_id) {
  player_t *player = &w->players[client_id];
  int pause_type = w->config.sv_pauseable ? DDNET_PAUSE_SPEC : DDNET_PAUSE_PAUSED;

  /* ToggleSpecPause */
  if (player->paused != DDNET_PAUSE_NONE)
    player_pause(w, client_id, DDNET_PAUSE_NONE, false);
  else
    player_pause(w, client_id, pause_type, false);
}

/* CPlayer::Tick */
DDNET_NOINLINE static void player_tick(world_t *w, int client_id) {
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
  if (!collision(w)->tune) {
    player->tune_zone = 0;
    return;
  }
  int current_index = collision_get_map_index(collision(w), player->view_pos);
  player->tune_zone = collision_is_tune(collision(w), current_index);
}

/* CPlayer::PostPostTick */
DDNET_NOINLINE static void player_post_post_tick(world_t *w, int client_id) {
  player_t *player = &w->players[client_id];
  if (!player->has_character && player->spawning && player->weak_hook_spawn)
    player_try_respawn(w, client_id);
}

/* CPlayer::OnPredictedEarlyInput */
DDNET_HOT static void player_on_predicted_early_input(world_t *w, int client_id, const input_t *new_input) {
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
DDNET_HOT static void player_on_predicted_input(world_t *w, int client_id, const input_t *new_input) {
  player_t *player = &w->players[client_id];
  /* skip the input if chat is active */
  if ((player->player_flags & DDNET_PLAYERFLAG_CHATTING) &&
      (new_input->player_flags & DDNET_PLAYERFLAG_CHATTING))
    return;

  if (player->has_character && !player->paused && !(new_input->player_flags & DDNET_PLAYERFLAG_SPEC_CAM))
    character_on_predicted_input(w, &w->characters[client_id], new_input);
}

bool ddnet_player_join(ddnet_world_t *w, int client_id) {
  if (client_id < 0 || client_id >= MAX_CLIENTS || !world_reserve_clients(w, client_id + 1) ||
      w->players[client_id].active || !world_switch_reserve(w, w->teams.team[client_id]))
    return false;

  /* CPlayer::CPlayer and CPlayer::Reset */
  player_t *player = &w->players[client_id];
  memset(player, 0, sizeof(*player));
  player->active = true;
  player->die_tick = server_tick(w);
  player->previous_die_tick = player->die_tick;
  player->unique_id = w->next_unique_id++;
  player->finish_tick = -1;
  id_list_add(w->player_ids, &w->num_players, client_id);
  if (client_id >= w->num_clients)
    w->num_clients = client_id + 1;

  /* IGameController::OnPlayerConnect */
  player_respawn(w, client_id, false);
  return true;
}

/* CPlayer::TryRespawn right away, for a player that is to spawn (see world.h) */
bool ddnet_player_spawn(ddnet_world_t *w, int client_id) {
  if (client_id < 0 || client_id >= w->num_clients || !w->players[client_id].active)
    return false;
  player_t *player = &w->players[client_id];
  if (!player->has_character && player->spawning && !player->weak_hook_spawn)
    player_try_respawn(w, client_id);
  return player->has_character;
}

void ddnet_player_leave(ddnet_world_t *w, int client_id) {
  if (client_id < 0 || client_id >= w->num_clients || !w->players[client_id].active)
    return;

  /* CGameControllerDDNet::OnPlayerDisconnect */
  player_kill_character(w, client_id, WEAPON_GAME);
  if (w->config.sv_team != DDNET_SV_TEAM_FORCED_SOLO)
    teams_set_force_character_team(w, client_id, TEAM_FLOCK);

  w->players[client_id].active = false;
  id_list_remove(w->player_ids, &w->num_players, client_id);
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
  if (client_id < 0 || client_id >= w->num_clients || !teams_is_valid_team_number(team))
    return;
  teams_set_force_character_team(w, client_id, team);
}

/* ------------------------------------------------------------------ tick */

/* The timer of one switch of one team. Returns whether it is still running. */
static bool switch_timer_tick(world_t *w, int s, int j) {
  /* (a team without a row has no timer running) */
  if (j >= w->switch_teams)
    return false;
  const int index = j * w->num_switchers + s;
  ddnet_switch_state_t *switcher = &w->switch_states[index];
  if (switcher->type != TILE_SWITCHTIMEDOPEN && switcher->type != TILE_SWITCHTIMEDCLOSE)
    return false;
  if (switcher->end_tick <= server_tick(w) && switcher->type == TILE_SWITCHTIMEDOPEN) {
    world_touch(w, TOUCH_SWITCHER, index);
    w->switches_touched[j] = true;
    switcher->status = false;
    switcher->end_tick = 0;
    switcher->type = TILE_SWITCHCLOSE;
    return false;
  } else if (switcher->end_tick <= server_tick(w) && switcher->type == TILE_SWITCHTIMEDCLOSE) {
    world_touch(w, TOUCH_SWITCHER, index);
    w->switches_touched[j] = true;
    switcher->status = true;
    switcher->end_tick = 0;
    switcher->type = TILE_SWITCHOPEN;
    return false;
  }
  return true;
}

DDNET_COLD bool world_switch_grow(world_t *w, int team) {
  /* rows for the teams up to this one, as the map starts their switches (a
   * copy of only what changed is then not enough anymore, see ddnet_world_copy()) */
  const int rows = team + 1;
  if (w->num_switchers) {
    ddnet_switch_state_t *states =
        realloc(w->switch_states, (size_t)rows * (size_t)w->num_switchers * sizeof(*states));
    if (!states)
      return false;
    const ddnet_switch_state_t start = {0, 0, true, 0};
    for (int i = w->switch_teams * w->num_switchers; i < rows * w->num_switchers; i++)
      states[i] = start;
    w->switch_states = states;
  }
  w->switch_teams = rows;
  return true;
}

ddnet_switch_state_t *world_switch_write(world_t *w, int number, int team) {
  if (!world_switch_reserve(w, team))
    return NULL;
  const int index = team * w->num_switchers + number;
  world_touch(w, TOUCH_SWITCHER, index);
  return &w->switch_states[index];
}

/* A switch of a team got a timer. */
void world_switch_timer_started(world_t *w, int number, int team) {
  if (w->timed_switches_overflow)
    return;
  const int entry = number << 8 | team;
  for (int n = 0; n < w->num_timed_switches; n++)
    if (w->timed_switches[n] == entry)
      return;
  if (w->num_timed_switches == DDNET_MAX_TIMED_SWITCHES) {
    w->num_timed_switches = 0;
    w->timed_switches_overflow = true;
    return;
  }
  w->timed_switches[w->num_timed_switches++] = entry;
}

/* CGameContext::OnTick */
static inline void game_on_tick_end(world_t *w);
static void world_switch_timers_tick(world_t *w);

DDNET_HOT static void game_on_tick(world_t *w) {
  world_tick_entities(w);
  game_on_tick_end(w);
}

/* CGameContext::OnTick after the tick of the world */
static inline void game_on_tick_end(world_t *w) {
  /* CGameControllerDDNet::Tick */
  if (w->unfinishable_teams)
    teams_tick(w);

  PROF_BEGIN(PROF_PLAYERS);
  /* (whether a player is left without a tee, which only the long way of its tick can leave it) */
  bool without = false;
  for (int n = 0; n < w->num_players; n++) {
    const int c = w->player_ids[n];
    player_t *player = &w->players[c];
    const character_t *chr = &w->characters[c];
    if (player->has_character && chr->alive && player->paused == DDNET_PAUSE_NONE) {
      /* CPlayer::Tick for a player with a tee that is not about to pause (most of them), see
       * world_tick_lone()
       */
      player->view_pos = chr->pos;
      if (!collision(w)->tune)
        player->tune_zone = 0;
      else
        player->tune_zone =
            collision_is_tune(collision(w), collision_get_map_index(collision(w), player->view_pos));
    } else {
      player_tick(w, c);
      without |= !player->has_character;
    }
  }
  PROF_END(PROF_PLAYERS);

  /* (which only does something for a player without a tee) */
  if (without)
    for (int n = 0; n < w->num_players; n++)
      if (!w->players[w->player_ids[n]].has_character)
        player_post_post_tick(w, w->player_ids[n]);

  if (w->timed_switches_overflow | w->num_timed_switches)
    world_switch_timers_tick(w);
}

/* timed switches fall back to their other state */
DDNET_NOINLINE static void world_switch_timers_tick(world_t *w) {
  if (w->timed_switches_overflow) {
    /* too many timers to keep track of: look at all of them, like DDNet */
    bool any = false;
    for (int s = 0; s < w->num_switchers; s++)
      for (int j = 0; j < w->switch_teams; ++j)
        any |= switch_timer_tick(w, s, j);
    w->timed_switches_overflow = any;
  } else {
    for (int n = 0; n < w->num_timed_switches;) {
      const int entry = w->timed_switches[n];
      if (switch_timer_tick(w, entry >> 8, entry & 0xff))
        n++;
      else
        w->timed_switches[n] = w->timed_switches[--w->num_timed_switches];
    }
  }
}

#ifndef DDNET_PHYSICS_EVENTS
/* A number no other world with another map or tuning has. (The event build takes the one of the other build:
 * one counter for the worlds of both, which can be copied into each other.) */
uint64_t world_new_universe(void) {
  static uint64_t counter;
  return __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
}
#endif

/* A new name for the state of a world: no other world is a copy of it, and its log starts here. */
static void world_new_lineage(world_t *w) {
  /* a number of its own per world, and a count below it */
  if (!w->lineage_base || (w->lineage & 0xffffff) == 0xffffff) {
    w->lineage_base = world_new_universe() << 24;
    w->lineage = w->lineage_base;
  }
  w->lineage++;
  w->touch_count = 0;
}

/* The log is full or was not there yet: it starts again, and what was copied
 * from or to this world before is not known to be the same anymore. */
void world_touch_reset(world_t *w) {
  if (!w->touch_log)
    w->touch_log = malloc(TOUCH_LOG_SIZE * sizeof(*w->touch_log));
  w->origin_lineage = 0;
  world_new_lineage(w);
}

void ddnet_world_changed(ddnet_world_t *w) {
  world_touch_reset(w);
  /* the switchers may have been written to */
  memset(w->switches_touched, 1, sizeof(w->switches_touched));
  /* (and the teams, see switch_status()) */
  for (int n = 0; n < w->num_players; n++)
    world_switch_reserve(w, w->teams.team[w->player_ids[n]]);
}

/* Convert the tuning to floats exactly like every read of a CTuneParam does. */
void ddnet_world_sync(ddnet_world_t *w) {
  projectiles_sync(w);
  lights_sync(w);
}

void ddnet_world_tuning_changed(ddnet_world_t *w) {
  /* with the tuning as it was */
  if (w->tuning_values && w->entities)
    projectiles_forget_orbits(w, server_tick(w), true);
  w->memo_universe = world_new_universe();
  /* the tuning is only copied with everything else */
  world_touch_reset(w);

  /* the paths of the projectiles depend on the tuning */
  for (int index = w->first_entity[DDNET_ENTTYPE_PROJECTILE]; index != -1;
       index = entity_peek(w, index)->link.next)
    entity(w, index)->u.projectile.next_tick = 0;
  w->projectile_wakeup = 0;

  for (int zone = 0; zone < w->num_tune_zones; zone++) {
    const ddnet_tune_param_t *params = (const ddnet_tune_param_t *)&w->tuning[zone];
    float *values = (float *)&w->tuning_values[zone];
    for (int i = 0; i < DDNET_NUM_TUNING_PARAMS; i++)
      values[i] = params[i] / 100.0f;

    /* The smallest squared speed d for which VelocityRamp() does not return
     * early, which is "sqrtf(d) * 50 < start" being false. That only becomes
     * false once as d grows, so it can be searched in the bits of d. */
    const float start = w->tuning_values[zone].velramp_start;
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
    memcpy(&w->tuning_values[zone].velramp_start_squared, &low, sizeof(low));

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
    memcpy(&w->tuning_values[zone].speed_clamp_squared, &low, sizeof(low));

    /* and the smallest d for which "sqrtf(d) > hook_length" */
    const float hook_length = w->tuning_values[zone].hook_length;
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
    memcpy(&w->tuning_values[zone].hook_length_squared, &low, sizeof(low));

    velocity_ramp_prepare(&w->tuning_values[zone]);
  }
}

/* One iteration of the tick loop in CServer::Run. */
/* ddnet_world_tick() for a world with one player whose tee is in it and who
 * does nothing special (no /spec, chat or pause): the same calls in the same
 * order, without the loops over players and kinds of entities around them.
 * False if the world is not like that, with nothing done. */
static inline bool world_tick_lone(world_t *w) {
  const int c = w->player_ids[0];
  player_t *player = &w->players[c];
  character_t *chr = &w->characters[c];
  const input_t *input = &player->input;
  if (!player->has_character | (player->paused != DDNET_PAUSE_NONE) | (chr->spec != 0) |
      (input->player_flags != 0) | w->config.sv_no_weak_hook | w->pickups_generic |
      (w->first_entity[DDNET_ENTTYPE_FLAG] != -1))
    return false;

  /* CPlayer::OnPredictedEarlyInput */
  player->player_flags = input->player_flags;
  character_on_direct_input(w, chr, input);

  w->tick++;

  /* CPlayer::OnPredictedInput: the same input and the same correction of its aim as just now */
  chr->saved_input = chr->latest_input;

  /* CGameWorld::Tick */
  if (w->first_entity[DDNET_ENTTYPE_PROJECTILE] != -1)
    projectiles_tick(w);
  if (w->first_entity[DDNET_ENTTYPE_LASER] != -1)
    lasers_tick(w);
  /* What is known about where the tee is, which nothing changes anymore until it moves. Then the
   * pickups, unless the tee is in the world on a tile where the map has none near, with none on conveyors. */
  const bool quiet = character_is_quiet(collision(w), chr);
  if (w->num_pickup_movers | (w->num_cores != 1) || !quiet)
    pickups_tick_static(w);
  /* (the tee alone in the world; the short way of its tick changes nothing about who is in it) */
  bool alone = w->first_entity[DDNET_ENTTYPE_CHARACTER] == c && chr->link.next == -1;
  if (alone) {
    /* the one tee that is there: traverse_begin() and character_tick() for it */
    w->next_traverse_entity.type = DDNET_ENTTYPE_CHARACTER;
    w->next_traverse_entity.index = -1;
    if (!character_tick_plain_fresh(w, chr)) {
      character_tick_any(w, chr);
      /* (a loop inside of its tick may have left the traversal somewhere else) */
      for (int index = w->next_traverse_entity.index; index != -1;) {
        traverse_begin(w, DDNET_ENTTYPE_CHARACTER, index);
        character_tick(w, &w->characters[index]);
        index = w->next_traverse_entity.index;
      }
      alone = w->first_entity[DDNET_ENTTYPE_CHARACTER] == c && chr->link.next == -1;
    }
  } else {
    for (int index = w->first_entity[DDNET_ENTTYPE_CHARACTER]; index != -1;) {
      traverse_begin(w, DDNET_ENTTYPE_CHARACTER, index);
      character_tick(w, &w->characters[index]);
      index = w->next_traverse_entity.index;
    }
    alone = w->first_entity[DDNET_ENTTYPE_CHARACTER] == c && chr->link.next == -1;
  }
#ifdef __SSE4_1__
  if (alone && w->num_cores == 1) {
    /* world_tick_entities_end() for the one tee that is there */
    w->next_traverse_entity.type = DDNET_ENTTYPE_CHARACTER;
    w->next_traverse_entity.index = -1;
    character_tick_deferred_lone(w, chr, core_tuning(w, &chr->core));
    if (w->entities_marked)
      world_remove_entities(w);
    chr->strong_weak_id = 0;
  } else
#endif
    world_tick_entities_end(w);

  /* CGameContext::OnTick after the tick of the world, see game_on_tick_end() */
  if (w->unfinishable_teams)
    teams_tick(w);
  if (player->has_character && chr->alive && player->paused == DDNET_PAUSE_NONE) {
    /* CPlayer::Tick for a player with a tee that is not about to pause */
    player->view_pos = chr->pos;
    if (!collision(w)->tune)
      player->tune_zone = 0;
    else
      player->tune_zone =
          collision_is_tune(collision(w), collision_get_map_index(collision(w), player->view_pos));
  } else {
    player_tick(w, c);
  }
  if (!player->has_character)
    player_post_post_tick(w, c);
  if (w->timed_switches_overflow | w->num_timed_switches)
    world_switch_timers_tick(w);
  return true;
}

static void world_tick_any(world_t *w);

DDNET_HOT void ddnet_world_tick(ddnet_world_t *w) {
  if (w->num_players == 1 && world_tick_lone(w))
    return;
  world_tick_any(w);
}

/* ddnet_world_tick() for any world (kept out of the function above, like character_tick_any()) */
DDNET_NOINLINE static void world_tick_any(world_t *w) {
  /* Chat commands arrive between two ticks. */
  for (int n = 0; n < w->num_players; n++) {
    const int c = w->player_ids[n];
    if ((w->characters[c].spec != 0) != (w->players[c].paused != DDNET_PAUSE_NONE))
      player_toggle_spec(w, c);
  }

  /* The inputs for the upcoming tick are handed to the game twice. First
   * before the tick counter advances: this is when weapons fire. Then after it
   * ("apply new input"), which only looks at the player and its tee and only
   * sets the input it saves, and does not look at the tick: that comes right
   * after the first for each player, which nothing a weapon of another player
   * does changes (a weapon kills nobody, pauses nobody). */
  PROF_BEGIN(PROF_DIRECT_INPUT);
  for (int n = 0; n < w->num_players; n++) {
    const int c = w->player_ids[n];
    player_on_predicted_early_input(w, c, &w->players[c].input);
    player_on_predicted_input(w, c, &w->players[c].input);
  }
  PROF_END(PROF_DIRECT_INPUT);

  w->tick++;

  game_on_tick(w);
}

#ifdef DDNET_PHYSICS_PROFILE
unsigned long long ddnet_prof_cycles[PROF_NUM], ddnet_prof_calls[PROF_NUM];
unsigned long long ddnet_prof_counters[PROF_MAX_COUNTERS];
const char *ddnet_prof_counter_names[PROF_MAX_COUNTERS];
void ddnet_prof_report(long ticks) {
  static const char *const NAMES[PROF_NUM] = {
      "direct input", "predicted input", "projectiles", "other entities", "ddrace tick", "core tick",
      "weapons",      "post core tick",  "move",        "quantize",       "players"};
  unsigned long long total = 0;
  for (int i = 0; i < PROF_NUM; i++)
    total += ddnet_prof_cycles[i];
  for (int i = 0; i < PROF_NUM; i++)
    fprintf(stderr, "%-16s %7.1f cycles/tick  %5.1f%%  (%.2f calls/tick)\n", NAMES[i],
            (double)ddnet_prof_cycles[i] / ticks, 100.0 * ddnet_prof_cycles[i] / total,
            (double)ddnet_prof_calls[i] / ticks);
  fprintf(stderr, "%-16s %7.1f cycles/tick\n", "sum", (double)total / ticks);
  for (int i = 0; i < PROF_MAX_COUNTERS; i++)
    if (ddnet_prof_counter_names[i])
      fprintf(stderr, "%-28s %8.4f per tick\n", ddnet_prof_counter_names[i],
              (double)ddnet_prof_counters[i] / ticks);
}
#endif

/* ------------------------------------------------------------- lifecycle */

/* One console command from the settings of the map. */
static void world_execute_map_command(void *user, const char *name, char *args) {
  world_t *w = user;
  char *argv[DDNET_CONSOLE_MAX_ARGS];

  if (strcasecmp(name, "tune") == 0) {
    /* CGameContext::ConTuneParam */
    if (ddnet_console_parse_args(args, "s ?f", argv) == 2)
      ddnet_tuning_set_by_name(&w->tuning[0], argv[0], strtod(argv[1], NULL));
  } else if (strcasecmp(name, "tune_zone") == 0) {
    /* CGameContext::ConTuneZone */
    if (ddnet_console_parse_args(args, "i s f", argv) == 3) {
      int list = (int)strtol(argv[0], NULL, 10);
      if (list >= 0 && list < w->num_tune_zones)
        ddnet_tuning_set_by_name(&w->tuning[list], argv[1], strtod(argv[2], NULL));
    }
  } else if (strcasecmp(name, "switch_open") == 0) {
    /* CGameContext::ConSwitchOpen */
    if (ddnet_console_parse_args(args, "i", argv) == 1) {
      int number = (int)strtol(argv[0], NULL, 10);
      if (number >= 0 && number <= w->num_switchers - 1) {
        w->switch_initial[number] = false;
        /* a reset of the switches of a team leaves this one in another state now */
        memset(w->switches_touched, 1, sizeof(w->switches_touched));
      }
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

  if (!world_reserve_clients(w, 1))
    return false;
  /* Only the tune zones that exist in the map can ever be looked up. */
  w->num_tune_zones = col->highest_tune_zone + 1;
  w->tuning = calloc((size_t)w->num_tune_zones, sizeof(*w->tuning));
  w->tuning_values = calloc((size_t)w->num_tune_zones, sizeof(*w->tuning_values));
  /* CWorldCore::InitSwitchers */
  w->num_switchers = col->highest_switch_number > 0 ? col->highest_switch_number + 1 : 0;
  w->switch_initial = calloc((size_t)(w->num_switchers ? w->num_switchers : 1), sizeof(*w->switch_initial));
  if (!w->players || !w->characters || !w->tuning || !w->tuning_values || !w->switch_initial)
    goto fail;

  /* (no team has a row of its own yet: all switches are on, see world_switch_write()) */
  for (int s = 0; s < w->num_switchers; s++)
    w->switch_initial[s] = true;

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

  for (int i = 0; i < DDNET_PROJECTILE_DUE_SIZE; i++)
    w->projectile_due[i] = -1;

  /* create all entities from the game layer */
  if (!controller_create_all_entities(w))
    goto fail;
  w->first_static_laser = w->first_entity[DDNET_ENTTYPE_LASER];
  /* The pickups on conveyors. The entities were made in the order of the
   * pickups of the map, the list has the newest first. */
  {
    const collision_t *col = collision(w);
    int id = col->num_pickups;
    for (int index = w->first_entity[DDNET_ENTTYPE_PICKUP]; index != -1;
         index = entity_peek(w, index)->link.next) {
      id--;
      const entity_t *ent = entity(w, index);
      if (id < 0 || vne(ent->pos, col->pickups[id].pos) || ent->u.pickup.type != col->pickups[id].type ||
          ent->u.pickup.subtype != col->pickups[id].subtype) {
        w->pickups_generic = true;
        break;
      }
      if (!col->pickups[id].moves)
        continue;
      if (w->num_pickup_movers == DDNET_MAX_PICKUP_MOVERS) {
        w->pickups_generic = true;
        break;
      }
      w->pickup_movers[w->num_pickup_movers] = index;
      w->pickup_mover_ids[w->num_pickup_movers] = id;
      w->num_pickup_movers++;
    }
    if (id != 0)
      w->pickups_generic = true;
  }
  w->orbit_tele_out = w->tele_out;
  w->orbit_old_teleport_weapons = w->config.sv_old_teleport_weapons;
  ddnet_world_tuning_changed(w);
  /* (a first reset of the switches of a team sets their type) */
  memset(w->switches_touched, 1, sizeof(w->switches_touched));
  /* what changes from here on is kept track of for ddnet_world_copy() */
  world_touch_reset(w);
  return true;

fail:
  ddnet_world_free(w);
  return false;
}

void ddnet_world_free(ddnet_world_t *w) {
  free(w->tuning);
  free(w->tuning_values);
  free(w->switch_states);
  free(w->switch_initial);
  free(w->players);
  free(w->characters);
  free(w->entities);
  free(w->dragger_targets);
  free(w->gun_timers);
  free(w->projectile_memo);
  free(w->sight_memo);
  laser_grid_release(w->laser_grid);
  free(w->laser_tiles);
  free(w->touch_log);
  free(w->projectile_awake);
  free(w->projectile_parked);
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

#ifdef DDNET_PHYSICS_CHECK_COPY
/* for tests: how many copies only copied what changed, and how much that was */
long ddnet_copy_check_sparse, ddnet_copy_check_entries;
#endif

bool ddnet_world_copy(ddnet_world_t *dst, const ddnet_world_t *src) {
  if (dst == src)
    return true;

  /* keep the buffers of dst, and what says how it relates to src (only that:
   * the rest of it is overwritten, and a copy of all of it would double the
   * bytes a copy moves) */
  const struct {
    ddnet_tuning_t *tuning;
    ddnet_tuning_values_t *tuning_values;
    ddnet_switch_state_t *switch_states;
    bool *switch_initial;
    ddnet_player_t *players;
    ddnet_character_t *characters;
    ddnet_entity_t *entities;
    ddnet_dragger_targets_t *dragger_targets;
    ddnet_gun_timers_t *gun_timers;
    ddnet_projectile_memo_t *projectile_memo;
    ddnet_sight_memo_t *sight_memo;
    uint32_t *touch_log;
    ddnet_laser_grid_t *laser_grid;
    ddnet_laser_tile_t *laser_tiles;
    int *projectile_awake;
    ddnet_parked_projectile_t *projectile_parked;
    uint64_t lineage, lineage_base, origin_lineage;
    int entity_capacity, client_capacity, projectile_awake_capacity, projectile_parked_capacity;
    int num_clients, num_tune_zones, num_switchers, switch_teams, num_draggers, num_guns;
    int touch_count, origin_pos;
  } old = {
      dst->tuning,
      dst->tuning_values,
      dst->switch_states,
      dst->switch_initial,
      dst->players,
      dst->characters,
      dst->entities,
      dst->dragger_targets,
      dst->gun_timers,
      dst->projectile_memo,
      dst->sight_memo,
      dst->touch_log,
      dst->laser_grid,
      dst->laser_tiles,
      dst->projectile_awake,
      dst->projectile_parked,
      dst->lineage,
      dst->lineage_base,
      dst->origin_lineage,
      dst->entity_capacity,
      dst->client_capacity,
      dst->projectile_awake_capacity,
      dst->projectile_parked_capacity,
      dst->num_clients,
      dst->num_tune_zones,
      dst->num_switchers,
      dst->switch_teams,
      dst->num_draggers,
      dst->num_guns,
      dst->touch_count,
      dst->origin_pos,
  };
  *dst = *src;
  dst->tuning = old.tuning;
  dst->tuning_values = old.tuning_values;
  dst->switch_states = old.switch_states;
  dst->switch_initial = old.switch_initial;
  dst->players = old.players;
  dst->characters = old.characters;
  dst->entities = old.entities;
  dst->entity_capacity = old.entity_capacity;
  dst->dragger_targets = old.dragger_targets;
  dst->gun_timers = old.gun_timers;
  dst->projectile_memo = old.projectile_memo;
  dst->sight_memo = old.sight_memo;
  dst->touch_log = old.touch_log;
  dst->lineage_base = old.lineage_base;
  dst->lineage = old.lineage;
  dst->laser_grid = old.laser_grid;
  dst->laser_tiles = old.laser_tiles;
  dst->projectile_awake = old.projectile_awake;
  dst->projectile_awake_capacity = old.projectile_awake_capacity;
  dst->projectile_parked = old.projectile_parked;
  dst->projectile_parked_capacity = old.projectile_parked_capacity;

  if (dst->projectile_awake_capacity < src->num_projectile_awake) {
    int *awake = realloc(dst->projectile_awake, (size_t)src->projectile_awake_capacity * sizeof(*awake));
    if (!awake)
      goto fail;
    dst->projectile_awake = awake;
    dst->projectile_awake_capacity = src->projectile_awake_capacity;
  }
  if (src->num_projectile_awake)
    memcpy(dst->projectile_awake, src->projectile_awake,
           (size_t)src->num_projectile_awake * sizeof(*dst->projectile_awake));
  if (dst->projectile_parked_capacity < src->num_projectile_parked) {
    ddnet_parked_projectile_t *parked =
        realloc(dst->projectile_parked, (size_t)src->projectile_parked_capacity * sizeof(*parked));
    if (!parked)
      goto fail;
    dst->projectile_parked = parked;
    dst->projectile_parked_capacity = src->projectile_parked_capacity;
  }

  /* the same for all copies of a world: shared, not copied */
  if (src->laser_grid != dst->laser_grid) {
    if (src->laser_grid)
      __atomic_add_fetch(&src->laser_grid->refs, 1, __ATOMIC_RELAXED);
    laser_grid_release(dst->laser_grid);
    dst->laser_grid = src->laser_grid;
  }

  dst->client_capacity = old.client_capacity;
  if (!world_reserve_clients(dst, src->num_clients > 0 ? src->num_clients : 1))
    goto fail;
  /* Only the slots that were ever used need to be copied. Slots that only dst
   * used are emptied. */
  memcpy(dst->players, src->players, (size_t)src->num_clients * sizeof(*dst->players));
  memcpy(dst->characters, src->characters, (size_t)src->num_clients * sizeof(*dst->characters));
  if (old.num_clients > src->num_clients)
    memset(&dst->players[src->num_clients], 0,
           (size_t)(old.num_clients - src->num_clients) * sizeof(*dst->players));

  /* (room for the entities src has, not for as many as it has room for: a copy
   * that is only kept and copied from stays small, and one that is ticked grows) */
  if (dst->entity_capacity < src->num_entities) {
    entity_t *entities = realloc(dst->entities, (size_t)src->num_entities * sizeof(*entities));
    if (!entities)
      goto fail;
    dst->entities = entities;
    dst->entity_capacity = src->num_entities;
  }

  /* The tuning, the switchers and the tables of draggers and turrets are
   * large and change rarely. If one of the two worlds was copied from the
   * other and it is known what was written to in both since (touch_log),
   * only that is copied:
   * - dst was copied from src: what dst changed, and what src changed since;
   * - src was copied from dst and did not change: what dst changed since. */
  int src_from = 0, src_to = 0, dst_from = 0, dst_to = 0;
  bool sparse = false;
  if (old.tuning && old.num_tune_zones == src->num_tune_zones && old.num_switchers == src->num_switchers &&
      old.switch_teams == src->switch_teams &&
      old.num_draggers == src->num_draggers && old.num_guns == src->num_guns && src->lineage && old.lineage) {
    if (old.origin_lineage == src->lineage && old.origin_pos <= src->touch_count) {
      sparse = true;
      /* (and the entry before, see world_touch()) */
      src_from = maxi(old.origin_pos - 1, 0), src_to = src->touch_count;
      dst_to = old.touch_count;
    } else if (src->origin_lineage == old.lineage && src->touch_count == 0 &&
               src->origin_pos <= old.touch_count) {
      sparse = true;
      dst_from = maxi(src->origin_pos - 1, 0), dst_to = old.touch_count;
    }
  }
  if (sparse) {
#ifdef DDNET_PHYSICS_CHECK_COPY
    ddnet_copy_check_sparse++;
    ddnet_copy_check_entries += (src_to - src_from) + (dst_to - dst_from);
#endif
    bool parked_changed = false;
    /* many entities: all of them at once is faster than one by one */
    const bool all_entities = ((src_to - src_from) + (dst_to - dst_from)) * 8 > src->num_entities;
    if (all_entities && src->num_entities)
      memcpy(dst->entities, src->entities, (size_t)src->num_entities * sizeof(*dst->entities));
    for (int pass = 0; pass < 2; pass++) {
      const uint32_t *log = pass ? old.touch_log : src->touch_log;
      for (int i = pass ? dst_from : src_from, end = pass ? dst_to : src_to; i < end; i++) {
        const int index = (int)(log[i] & 0xffffff);
        switch (log[i] >> 24) {
        case TOUCH_SWITCHER:
          dst->switch_states[index] = src->switch_states[index];
          break;
        case TOUCH_DRAGGER_TARGETS:
          dst->dragger_targets[index] = src->dragger_targets[index];
          break;
        case TOUCH_GUN_TIMERS:
          dst->gun_timers[index] = src->gun_timers[index];
          break;
        case TOUCH_PARKED:
          parked_changed = true;
          break;
        case TOUCH_ENTITY:
          /* (one that only dst has is not part of it anymore) */
          if (!all_entities && index < src->num_entities)
            dst->entities[index] = src->entities[index];
          break;
        }
      }
    }
    /* the list of parked projectiles, if it was written to at all */
    if (parked_changed && src->num_projectile_parked)
      memcpy(dst->projectile_parked, src->projectile_parked,
             (size_t)src->num_projectile_parked * sizeof(*dst->projectile_parked));
#ifdef DDNET_PHYSICS_CHECK_COPY
    if (memcmp(dst->projectile_parked, src->projectile_parked,
               (size_t)src->num_projectile_parked * sizeof(*dst->projectile_parked)) ||
        memcmp(dst->tuning, src->tuning, (size_t)src->num_tune_zones * sizeof(*dst->tuning)) ||
        memcmp(dst->tuning_values, src->tuning_values,
               (size_t)src->num_tune_zones * sizeof(*dst->tuning_values)) ||
        memcmp(dst->switch_states, src->switch_states,
               (size_t)src->switch_teams * src->num_switchers * sizeof(*dst->switch_states)) ||
        memcmp(dst->switch_initial, src->switch_initial, (size_t)src->num_switchers) ||
        memcmp(dst->dragger_targets, src->dragger_targets,
               (size_t)src->num_draggers * sizeof(*dst->dragger_targets)) ||
        memcmp(dst->gun_timers, src->gun_timers, (size_t)src->num_guns * sizeof(*dst->gun_timers)) ||
        memcmp(dst->entities, src->entities, (size_t)src->num_entities * sizeof(*dst->entities))) {
      fprintf(stderr, "ddnet_world_copy: a copy of what changed is not a copy of everything\n");
      abort();
    }
#endif
  } else {
    if (src->num_entities)
      memcpy(dst->entities, src->entities, (size_t)src->num_entities * sizeof(*dst->entities));
    if (src->num_projectile_parked)
      memcpy(dst->projectile_parked, src->projectile_parked,
             (size_t)src->num_projectile_parked * sizeof(*dst->projectile_parked));
    if (!copy_buffer(&dst->tuning, old.num_tune_zones, src->tuning, src->num_tune_zones,
                     sizeof(*dst->tuning)) ||
        !copy_buffer(&dst->tuning_values, old.num_tune_zones, src->tuning_values, src->num_tune_zones,
                     sizeof(*dst->tuning_values)) ||
        !copy_buffer(&dst->switch_states, old.switch_teams * old.num_switchers, src->switch_states,
                     src->switch_teams * src->num_switchers, sizeof(*dst->switch_states)) ||
        !copy_buffer(&dst->switch_initial, old.num_switchers, src->switch_initial, src->num_switchers,
                     sizeof(*dst->switch_initial)) ||
        !copy_buffer(&dst->dragger_targets, old.num_draggers, src->dragger_targets, src->num_draggers,
                     sizeof(*dst->dragger_targets)) ||
        !copy_buffer(&dst->gun_timers, old.num_guns, src->gun_timers, src->num_guns,
                     sizeof(*dst->gun_timers))) {
      goto fail;
    }
  }
  /* dst is a copy of src as it is now, and its own log starts here */
  world_new_lineage(dst);
  dst->origin_lineage = src->lineage;
  dst->origin_pos = src->touch_count;
  return true;

fail:
  ddnet_world_free(dst);
  return false;
}
