/* Port of DDNet's src/game/collision.cpp, function by function. */
#include "collision.h"

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ddnet_physics/types.h>

vec2 clamp_vel(int move_restriction, vec2 vel) {
  if (vel.x > 0 && (move_restriction & DDNET_CANTMOVE_RIGHT))
    vel.x = 0;
  if (vel.x < 0 && (move_restriction & DDNET_CANTMOVE_LEFT))
    vel.x = 0;
  if (vel.y > 0 && (move_restriction & DDNET_CANTMOVE_DOWN))
    vel.y = 0;
  if (vel.y < 0 && (move_restriction & DDNET_CANTMOVE_UP))
    vel.y = 0;
  return vel;
}

/* ---------------------------------------------------------------- loading */

static bool tele_list_push(ddnet_tele_list_t *list, vec2 pos) {
  /* Grows in powers of two. */
  if ((list->count & (list->count - 1)) == 0) {
    int capacity = list->count ? list->count * 2 : 1;
    vec2 *positions = realloc(list->positions, (size_t)capacity * sizeof(vec2));
    if (!positions)
      return false;
    list->positions = positions;
  }
  list->positions[list->count++] = pos;
  return true;
}

static void set_door_collision_at(collision_t *col, float x, float y, unsigned char type, unsigned char flags,
                                  unsigned char number) {
  if (!col->door)
    return;
  int nx = clampi(round_fast(x) >> 5, 0, col->width - 1);
  int ny = clampi(round_fast(y) >> 5, 0, col->height - 1);

  col->door[ny * col->width + nx].index = type;
  col->door[ny * col->width + nx].flags = flags;
  col->door[ny * col->width + nx].number = number;
}

/* CDoor::CDoor and CDoor::ResetCollision. A door is a line of stoppers that
 * only blocks while its switch is active. It never changes after the map is
 * loaded, so it is baked into the collision here instead of being an entity. */
static void create_door(collision_t *col, vec2 pos, float rotation, int length, int number) {
  vec2 direction = v2(sinf(rotation), cosf(rotation));

  if (collision_get_tile(col, pos.x, pos.y) || collision_get_front_tile(col, pos.x, pos.y))
    return;

  for (int i = 0; i < length - 1; i++) {
    vec2 current_pos = vadd(pos, vscale(direction, i));
    if (collision_check_point(col, current_pos.x, current_pos.y))
      break;
    else
      set_door_collision_at(col, current_pos.x, current_pos.y, TILE_STOPA, 0, number);
  }
}

/* The part of IGameController::OnEntity that does not need a world: spawn
 * points and doors. */
static bool load_static_entity(collision_t *col, int index, int x, int y, int layer, int number) {
  const vec2 pos = v2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);

  if (index >= ENTITY_SPAWN && index <= ENTITY_SPAWN_BLUE) {
    const int spawn_type = index - ENTITY_SPAWN;
    int count = col->num_spawn_points[spawn_type];
    if ((count & (count - 1)) == 0) {
      vec2 *points = realloc(col->spawn_points[spawn_type], (size_t)(count ? count * 2 : 1) * sizeof(vec2));
      if (!points)
        return false;
      col->spawn_points[spawn_type] = points;
    }
    col->spawn_points[spawn_type][count] = pos;
    col->num_spawn_points[spawn_type] = count + 1;
  } else if (index == ENTITY_DOOR) {
    int sides[8];
    sides[0] = collision_entity(col, x, y + 1, layer);
    sides[1] = collision_entity(col, x + 1, y + 1, layer);
    sides[2] = collision_entity(col, x + 1, y, layer);
    sides[3] = collision_entity(col, x + 1, y - 1, layer);
    sides[4] = collision_entity(col, x, y - 1, layer);
    sides[5] = collision_entity(col, x - 1, y - 1, layer);
    sides[6] = collision_entity(col, x - 1, y, layer);
    sides[7] = collision_entity(col, x - 1, y + 1, layer);
    for (int i = 0; i < 8; i++) {
      if (sides[i] >= ENTITY_LASER_SHORT && sides[i] <= ENTITY_LASER_LONG) {
        const float rotation = PI / 4 * i;
        const int length = 32 * 3 + 32 * (sides[i] - ENTITY_LASER_SHORT) * 3;
        /* (where it ends is worked out once the map is loaded, see doors_finish()) */
        if ((col->num_doors & (col->num_doors - 1)) == 0) {
          ddnet_map_door_t *doors =
              realloc(col->doors, (size_t)(col->num_doors ? col->num_doors * 2 : 1) * sizeof(*doors));
          if (!doors)
            return false;
          col->doors = doors;
        }
        col->doors[col->num_doors++] = (ddnet_map_door_t){.pos = pos,
                                                          .to = pos,
                                                          .direction = v2(sinf(rotation), cosf(rotation)),
                                                          .length = length,
                                                          .number = number};
        create_door(col, pos, rotation, length, number);
      }
    }
  }
  return true;
}

/* The order of CGameContext::CreateAllEntities. */
static bool load_static_entities(collision_t *col) {
  for (int y = 0; y < col->height; y++) {
    for (int x = 0; x < col->width; x++) {
      const int index = y * col->width + x;

      const int game_index = col->game[index].index;
      if (game_index >= ENTITY_OFFSET &&
          !load_static_entity(col, game_index - ENTITY_OFFSET, x, y, LAYER_GAME, 0))
        return false;

      if (col->front) {
        const int front_index = col->front[index].index;
        if (front_index >= ENTITY_OFFSET &&
            !load_static_entity(col, front_index - ENTITY_OFFSET, x, y, LAYER_FRONT, 0))
          return false;
      }

      if (col->switch_tiles) {
        const int switch_type = col->switch_tiles[index].type;
        if (switch_type >= ENTITY_OFFSET &&
            !load_static_entity(col, switch_type - ENTITY_OFFSET, x, y, LAYER_SWITCH,
                                col->switch_tiles[index].number))
          return false;
      }
    }
  }
  return true;
}

static bool load_layers(collision_t *col, const map_data_t *map) {
  const size_t count = (size_t)col->width * col->height;

  col->game = malloc(count * sizeof(*col->game));
  if (!col->game)
    return false;
  for (size_t i = 0; i < count; i++) {
    col->game[i].index = map->game_layer.data[i];
    col->game[i].flags = map->game_layer.flags ? map->game_layer.flags[i] : 0;
  }

  if (map->front_layer.data) {
    col->front = malloc(count * sizeof(*col->front));
    if (!col->front)
      return false;
    for (size_t i = 0; i < count; i++) {
      col->front[i].index = map->front_layer.data[i];
      col->front[i].flags = map->front_layer.flags ? map->front_layer.flags[i] : 0;
    }
  }

  if (map->tele_layer.type) {
    col->tele = malloc(count * sizeof(*col->tele));
    if (!col->tele)
      return false;
    for (size_t i = 0; i < count; i++) {
      col->tele[i].number = map->tele_layer.number[i];
      col->tele[i].type = map->tele_layer.type[i];
    }
  }

  if (map->speedup_layer.type) {
    col->speedup = malloc(count * sizeof(*col->speedup));
    if (!col->speedup)
      return false;
    for (size_t i = 0; i < count; i++) {
      col->speedup[i].force = map->speedup_layer.force[i];
      col->speedup[i].max_speed = map->speedup_layer.max_speed[i];
      col->speedup[i].type = map->speedup_layer.type[i];
      col->speedup[i].angle = map->speedup_layer.angle[i];
    }
  }

  if (map->switch_layer.type) {
    col->switch_tiles = malloc(count * sizeof(*col->switch_tiles));
    col->door = calloc(count, sizeof(*col->door));
    if (!col->switch_tiles || !col->door)
      return false;
    for (size_t i = 0; i < count; i++) {
      col->switch_tiles[i].number = map->switch_layer.number[i];
      col->switch_tiles[i].type = map->switch_layer.type[i];
      col->switch_tiles[i].flags = map->switch_layer.flags[i];
      col->switch_tiles[i].delay = map->switch_layer.delay[i];
    }
  }

  if (map->tune_layer.type) {
    col->tune = malloc(count * sizeof(*col->tune));
    if (!col->tune)
      return false;
    for (size_t i = 0; i < count; i++) {
      col->tune[i].number = map->tune_layer.number[i];
      col->tune[i].type = map->tune_layer.type[i];
    }
  }
  return true;
}

/* ---------------------------------------------------------- precomputation */

/* The pickup an entity tile stands for (IGameController::OnEntity), -1 if none. */
int collision_pickup_type(int index, int *subtype) {
  *subtype = 0;
  if (index == ENTITY_ARMOR_1)
    return DDNET_POWERUP_ARMOR;
  if (index == ENTITY_ARMOR_SHOTGUN)
    return DDNET_POWERUP_ARMOR_SHOTGUN;
  if (index == ENTITY_ARMOR_GRENADE)
    return DDNET_POWERUP_ARMOR_GRENADE;
  if (index == ENTITY_ARMOR_NINJA)
    return DDNET_POWERUP_ARMOR_NINJA;
  if (index == ENTITY_ARMOR_LASER)
    return DDNET_POWERUP_ARMOR_LASER;
  if (index == ENTITY_HEALTH_1)
    return DDNET_POWERUP_FREEZE;
  if (index == ENTITY_WEAPON_SHOTGUN) {
    *subtype = DDNET_WEAPON_SHOTGUN;
    return DDNET_POWERUP_WEAPON;
  }
  if (index == ENTITY_WEAPON_GRENADE) {
    *subtype = DDNET_WEAPON_GRENADE;
    return DDNET_POWERUP_WEAPON;
  }
  if (index == ENTITY_WEAPON_LASER) {
    *subtype = DDNET_WEAPON_LASER;
    return DDNET_POWERUP_WEAPON;
  }
  if (index == ENTITY_POWERUP_NINJA) {
    *subtype = DDNET_WEAPON_NINJA;
    return DDNET_POWERUP_NINJA;
  }
  return -1;
}

static bool tile_exists_slow(const collision_t *col, int index);

/* dist[i] = distance in tiles (chessboard metric, capped at 255) to the nearest
 * tile with the flag. Two passes over the map are enough for this metric. */
static uint8_t *distance_field(const collision_t *col, int flag) {
  const int w = col->width, h = col->height;
  uint8_t *dist = malloc((size_t)w * h);
  if (!dist)
    return NULL;
  for (int i = 0; i < w * h; i++)
    dist[i] = (col->tile_flags[i] & flag) ? 0 : 255;
#define RELAX(nx, ny)                                                                                        \
  if ((nx) >= 0 && (nx) < w && (ny) >= 0 && (ny) < h && dist[(ny) * w + (nx)] + 1 < dist[y * w + x])         \
    dist[y * w + x] = dist[(ny) * w + (nx)] + 1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      RELAX(x - 1, y) RELAX(x - 1, y - 1) RELAX(x, y - 1) RELAX(x + 1, y - 1)
    }
  for (int y = h - 1; y >= 0; y--)
    for (int x = w - 1; x >= 0; x--) {
      RELAX(x + 1, y) RELAX(x + 1, y + 1) RELAX(x, y + 1) RELAX(x - 1, y + 1)
    }
#undef RELAX
  return dist;
}

/* table[y * (w + 1) + x] = number of tiles with the flag in the rows above y and the columns left of x */
static uint16_t *summed_area_table(const collision_t *col, int flag) {
  const int w = col->width, h = col->height;
  uint16_t *table = calloc((size_t)(w + 1) * (h + 1), sizeof(*table));
  if (!table)
    return NULL;
  /* modulo 65536, see rect_count() */
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      table[(y + 1) * (w + 1) + x + 1] =
          (uint16_t)(((col->tile_flags[y * w + x] & flag) ? 1 : 0) + table[y * (w + 1) + x + 1] +
                     table[(y + 1) * (w + 1) + x] - table[y * (w + 1) + x]);
  return table;
}

/* How many tiles of the columns x0 to x1 - 1 and the rows y0 to y1 - 1 are
 * counted in a table, for up to 255 columns and rows: fewer than 65536 tiles,
 * so that the count comes out right although the table wraps around. */
static inline __attribute__((always_inline)) int rect_count(const collision_t *col, const uint16_t *table,
                                                            int x0, int y0, int x1, int y1) {
  const int stride = col->width + 1;
  return (uint16_t)(table[y1 * stride + x1] - table[y0 * stride + x1] - table[y1 * stride + x0] +
                    table[y0 * stride + x0]);
}

/* True if none of the tiles that positions in the rectangle are on is counted
 * in the table. The rectangle is taken a bit larger than given, which covers
 * the rounding of positions to tiles. */
static inline bool rect_is_free(const collision_t *col, const uint16_t *table, float min_x, float min_y,
                                float max_x, float max_y) {
  /* positions outside of the map are on the border tiles */
  const int x0 = clampi(((int)(min_x - 2.0f)) >> 5, 0, col->width - 1);
  const int y0 = clampi((int)(min_y - 2.0f) >> 5, 0, col->height - 1);
  const int x1 = clampi((int)(max_x + 2.0f) >> 5, 0, col->width - 1) + 1;
  const int y1 = clampi((int)(max_y + 2.0f) >> 5, 0, col->height - 1) + 1;
  /* (too large to tell) */
  if (x1 - x0 > 255 || y1 - y0 > 255)
    return false;
  return rect_count(col, table, x0, y0, x1, y1) == 0;
}

/* True if a projectile that flies from one position to the other in a tick
 * cannot hit anything solid or pass a teleporter. */
bool collision_rect_is_free_for_projectiles(const collision_t *col, vec2 from, vec2 to) {
  /* also false for positions that are not numbers */
  if (!(fabsf(from.x) < 1e6f && fabsf(from.y) < 1e6f && fabsf(to.x) < 1e6f && fabsf(to.y) < 1e6f))
    return false;
  return rect_is_free(col, col->sat_projectile, minf(from.x, to.x), minf(from.y, to.y), maxf(from.x, to.x),
                      maxf(from.y, to.y));
}

/* True if a projectile that flies from one position to the other in a tick
 * cannot pass a teleporter. */
bool collision_rect_is_free_of_teleporters(const collision_t *col, vec2 from, vec2 to) {
  if (!(fabsf(from.x) < 1e6f && fabsf(from.y) < 1e6f && fabsf(to.x) < 1e6f && fabsf(to.y) < 1e6f))
    return false;
  return rect_is_free(col, col->sat_tele, minf(from.x, to.x), minf(from.y, to.y), maxf(from.x, to.x),
                      maxf(from.y, to.y));
}

static bool is_stopper(int index) { return index == TILE_STOP || index == TILE_STOPS || index == TILE_STOPA; }

/* The solid tiles are needed to place the doors, everything else needs the doors. */
static bool precompute_solid(collision_t *col) {
  const int w = col->width, h = col->height;
  col->tile_flags = calloc((size_t)w * h, sizeof(*col->tile_flags));
  if (!col->tile_flags)
    return false;
  for (int i = 0; i < w * h; i++) {
    const int game = col->game[i].index;
    if (game == TILE_SOLID || game == TILE_NOHOOK)
      col->tile_flags[i] = DDNET_TILEFLAG_SOLID;
  }
  return true;
}

/* The tiles the state checks of CCharacter::HandleTiles look for (see character_handle_tile()). */
static bool is_state_tile(int tile) {
  switch (tile) {
  case TILE_DFREEZE:
  case TILE_DUNFREEZE:
  case TILE_LFREEZE:
  case TILE_LUNFREEZE:
  case TILE_EHOOK_ENABLE:
  case TILE_EHOOK_DISABLE:
  case TILE_HIT_ENABLE:
  case TILE_HIT_DISABLE:
  case TILE_NPC_ENABLE:
  case TILE_NPC_DISABLE:
  case TILE_NPH_ENABLE:
  case TILE_NPH_DISABLE:
  case TILE_UNLIMITED_JUMPS_ENABLE:
  case TILE_UNLIMITED_JUMPS_DISABLE:
  case TILE_WALLJUMP:
  case TILE_JETPACK_ENABLE:
  case TILE_JETPACK_DISABLE:
  case TILE_REFILL_JUMPS:
  case TILE_TELE_GUN_ENABLE:
  case TILE_TELE_GUN_DISABLE:
  case TILE_TELE_GRENADE_ENABLE:
  case TILE_TELE_GRENADE_DISABLE:
  case TILE_TELE_LASER_ENABLE:
  case TILE_TELE_LASER_DISABLE:
    return true;
  default:
    return false;
  }
}

static bool is_race_tile(int tile) {
  return tile == TILE_START || tile == TILE_FINISH || tile == TILE_UNLOCK_TEAM || tile == TILE_SOLO_ENABLE ||
         tile == TILE_SOLO_DISABLE;
}

/* tile_handlers, and DDNET_TILEFLAG_NEAR_RACE */
/* padded_flags, from the finished tile_flags */
static bool precompute_padded(collision_t *col) {
  const int w = col->width, h = col->height, pad = DDNET_TILE_PAD;
  const int pw = w + 2 * pad, ph = h + 2 * pad;
  col->padded_flags = malloc((size_t)pw * ph * sizeof(*col->padded_flags));
  if (!col->padded_flags)
    return false;
  for (int y = 0; y < ph; y++)
    for (int x = 0; x < pw; x++) {
      const int mx = x - pad, my = y - pad;
      int flags = col->tile_flags[clampi(my, 0, h - 1) * w + clampi(mx, 0, w - 1)];
      if (mx < 0 || my < 0 || mx >= w || my >= h)
        flags &= ~(DDNET_TILEFLAG_QUIET | DDNET_TILEFLAG_INERT);
      col->padded_flags[(size_t)y * pw + x] = (uint16_t)flags;
    }
  col->padded_width = pw;
  col->padded_origin = col->padded_flags + (size_t)pad * pw + pad;
  return true;
}

static bool precompute_tile_handlers(collision_t *col) {
  const int w = col->width, h = col->height;
  col->tile_handlers = calloc((size_t)w * h, 1);
  col->layer_tiles = calloc((size_t)w * h, 1);
  if (!col->tile_handlers || !col->layer_tiles)
    return false;
  for (int i = 0; i < w * h; i++) {
    const int tile = collision_get_tile_index(col, i), ftile = collision_get_front_tile_index(col, i);
    int handlers = 0;
    if (collision_is_time_checkpoint(col, i) > -1 || collision_is_front_time_checkpoint(col, i) > -1 ||
        collision_is_tele_checkpoint(col, i))
      handlers |= DDNET_TILEHANDLER_CHECKPOINT;
    if (is_race_tile(tile) || is_race_tile(ftile))
      handlers |= DDNET_TILEHANDLER_RACE;
    {
      const bool in_game = tile == TILE_FREEZE || tile == TILE_UNFREEZE || is_state_tile(tile);
      const bool in_front = ftile == TILE_FREEZE || ftile == TILE_UNFREEZE || is_state_tile(ftile);
      if (in_game && in_front && tile != ftile)
        handlers |= DDNET_TILEHANDLER_LAYERS_TWO;
      else
        col->layer_tiles[i] = (uint8_t)(in_game ? tile : in_front ? ftile : 0);
    }
    if (collision_get_switch_type(col, i) != 0)
      handlers |= DDNET_TILEHANDLER_SWITCH;
    if (collision_is_teleport(col, i) || collision_is_evil_teleport(col, i) ||
        collision_is_check_teleport(col, i) || collision_is_check_evil_teleport(col, i))
      handlers |= DDNET_TILEHANDLER_TELE;
    col->tile_handlers[i] = (uint8_t)handlers;
  }
  /* the sensors of a tee for start and finish lines are a third of a tile from its middle */
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      bool near = false;
      for (int dy = -1; dy <= 1 && !near; dy++)
        for (int dx = -1; dx <= 1 && !near; dx++) {
          const int n = clampi(y + dy, 0, h - 1) * w + clampi(x + dx, 0, w - 1);
          const int tile = collision_get_tile_index(col, n), ftile = collision_get_front_tile_index(col, n);
          near = tile == TILE_START || ftile == TILE_START || tile == TILE_FINISH || ftile == TILE_FINISH;
        }
      if (near)
        col->tile_flags[y * w + x] |= DDNET_TILEFLAG_NEAR_RACE;
    }
  return true;
}

static bool precompute(collision_t *col) {
  const int w = col->width, h = col->height;

  for (int i = 0; i < w * h; i++) {
    int flags = col->tile_flags[i];
    const int game = col->game[i].index;
    const int front = col->front ? col->front[i].index : 0;
    if (game == TILE_THROUGH_ALL || game == TILE_THROUGH_DIR || front == TILE_THROUGH_ALL ||
        front == TILE_THROUGH_DIR)
      flags |= DDNET_TILEFLAG_HOOK;
    if (col->tele && col->tele[i].type == TILE_TELEINHOOK)
      flags |= DDNET_TILEFLAG_HOOK;
    if (col->tele && col->tele[i].type == TILE_TELEINWEAPON)
      flags |= DDNET_TILEFLAG_WEAPON;
    if (col->tele && col->tele[i].type == TILE_TELEIN)
      flags |= DDNET_TILEFLAG_TELEIN;
    if (tile_exists_slow(col, i))
      flags |= DDNET_TILEFLAG_EXISTS;
    if (game == TILE_NOLASER || front == TILE_NOLASER)
      flags |= DDNET_TILEFLAG_NOLASER;
    const int layers[3] = {game, front, collision_get_switch_type(col, i)};
    for (int l = 0; l < 3; l++)
      if (layers[l] == TILE_FREEZE || layers[l] == TILE_DFREEZE || layers[l] == TILE_LFREEZE ||
          layers[l] == TILE_DEATH)
        flags |= DDNET_TILEFLAG_FREEZE;
    col->tile_flags[i] = (uint16_t)flags;
  }

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int flags = 0;
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          /* positions outside of the map are looked up on the border tiles */
          const int n = clampi(y + dy, 0, h - 1) * w + clampi(x + dx, 0, w - 1);
          if (is_stopper(col->game[n].index) || (col->front && is_stopper(col->front[n].index)) ||
              (col->door && col->door[n].index))
            flags |= DDNET_TILEFLAG_NEAR_STOPPER;
          if (col->game[n].index == TILE_DEATH || (col->front && col->front[n].index == TILE_DEATH))
            flags |= DDNET_TILEFLAG_NEAR_DEATH;
        }
      }
      col->tile_flags[y * w + x] |= (uint16_t)flags;
    }
  }

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int around = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
          around |= col->tile_flags[clampi(y + dy, 0, h - 1) * w + clampi(x + dx, 0, w - 1)];
      if (around & DDNET_TILEFLAG_SOLID)
        col->tile_flags[y * w + x] |= DDNET_TILEFLAG_NEAR_SOLID;
      if (!(col->tile_flags[y * w + x] & (DDNET_TILEFLAG_EXISTS | DDNET_TILEFLAG_FREEZE |
                                          DDNET_TILEFLAG_NEAR_STOPPER | DDNET_TILEFLAG_NEAR_DEATH)) &&
          !(col->tune && col->tune[y * w + x].type))
        col->tile_flags[y * w + x] |= DDNET_TILEFLAG_INERT;
      /* (something solid may be around: it is not what the tile handling looks at) */
      if (!(around & (DDNET_TILEFLAG_EXISTS | DDNET_TILEFLAG_FREEZE)) &&
          !(col->tile_flags[y * w + x] & (DDNET_TILEFLAG_NEAR_STOPPER | DDNET_TILEFLAG_NEAR_DEATH)) &&
          !(col->tune && col->tune[y * w + x].type))
        col->tile_flags[y * w + x] |= DDNET_TILEFLAG_QUIET;
    }
  }

  /* pickups */
  col->tile_pickups = malloc((size_t)w * h * sizeof(*col->tile_pickups));
  int capacity = 0;
  if (!col->tile_pickups)
    return false;
  for (int i = 0; i < w * h; i++) {
    col->tile_pickups[i] = -1;
    const int entities[3] = {col->game[i].index, col->front ? col->front[i].index : 0,
                             col->switch_tiles ? col->switch_tiles[i].type : 0};
    static const int LAYERS[3] = {LAYER_GAME, LAYER_FRONT, LAYER_SWITCH};
    for (int l = 0; l < 3; l++) {
      int subtype;
      const int type =
          entities[l] >= ENTITY_OFFSET ? collision_pickup_type(entities[l] - ENTITY_OFFSET, &subtype) : -1;
      if (type == -1)
        continue;
      if (col->num_pickups == capacity) {
        capacity = capacity ? capacity * 2 : 64;
        ddnet_map_pickup_t *pickups = realloc(col->pickups, (size_t)capacity * sizeof(*pickups));
        if (!pickups)
          return false;
        col->pickups = pickups;
      }
      ddnet_map_pickup_t *pickup = &col->pickups[col->num_pickups];
      pickup->pos = v2(i % w * 32.0f + 16.0f, i / w * 32.0f + 16.0f);
      pickup->tile = i;
      pickup->type = type;
      pickup->subtype = subtype;
      pickup->layer = LAYERS[l];
      pickup->number = l == 2 ? col->switch_tiles[i].number : 0;
      pickup->moves = false;
      if (col->tile_pickups[i] < 0)
        col->tile_pickups[i] = col->num_pickups;
      col->num_pickups++;
      /* a pickup on a conveyor moves */
      if (col->game[i].index == TILE_CP || col->game[i].index == TILE_CP_F) {
        col->pickups_move = true;
        col->pickups[col->num_pickups - 1].moves = true;
      }
      for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
          if (i % w + dx >= 0 && i % w + dx < w && i / w + dy >= 0 && i / w + dy < h) {
            uint16_t *flags = &col->tile_flags[(i / w + dy) * w + i % w + dx];
            *flags = (uint16_t)((*flags | DDNET_TILEFLAG_NEAR_PICKUP) & ~DDNET_TILEFLAG_QUIET);
          }
    }
  }

  for (int i = 0; i < 256; i++)
    col->move_fraction[i] = 1.0f / (float)(i + 1);
  col->line_fractions = malloc((size_t)(DDNET_LINE_FRACTIONS + 1) * (DDNET_LINE_FRACTIONS + 2) / 2 *
                               sizeof(*col->line_fractions));
  if (!col->line_fractions)
    return false;
  for (int end = 1; end <= DDNET_LINE_FRACTIONS; end++)
    for (int i = 0; i <= end; i++)
      col->line_fractions[end * (end + 1) / 2 + i] = i / (float)end;
  /* (and not so large that adding a half to a coordinate would round) */
  col->inner_width = minf((col->width - 1) * 32.0f, 4e6f);
  col->inner_height = minf((col->height - 1) * 32.0f, 4e6f);
  col->units_width = col->width * 32.0f;
  col->units_height = col->height * 32.0f;

  col->sat_solid = summed_area_table(col, DDNET_TILEFLAG_SOLID);
  col->sat_hook = summed_area_table(col, DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_HOOK);
  col->sat_projectile =
      summed_area_table(col, DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_WEAPON | DDNET_TILEFLAG_TELEIN);
  col->sat_tele = summed_area_table(col, DDNET_TILEFLAG_WEAPON | DDNET_TILEFLAG_TELEIN);
  for (int i = 0; i < w * h; i++)
    if (col->tile_flags[i] & (DDNET_TILEFLAG_WEAPON | DDNET_TILEFLAG_TELEIN))
      col->num_projectile_teleporters++;
  col->sat_laser = summed_area_table(col, DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_NOLASER);
  col->sat_nolaser = summed_area_table(col, DDNET_TILEFLAG_NOLASER);
  col->sat_exists = summed_area_table(col, DDNET_TILEFLAG_EXISTS);
  if (!col->sat_solid || !col->sat_hook || !col->sat_projectile || !col->sat_laser || !col->sat_nolaser ||
      !col->sat_tele || !col->sat_exists)
    return false;

  col->dist_solid = distance_field(col, DDNET_TILEFLAG_SOLID);
  col->dist_hook = distance_field(col, DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_HOOK);
  col->dist_weapon = distance_field(col, DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_WEAPON);
  col->dist_exists = distance_field(col, DDNET_TILEFLAG_EXISTS);
  if (!precompute_tile_handlers(col))
    return false;
  col->dist_projectile =
      distance_field(col, DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_WEAPON | DDNET_TILEFLAG_TELEIN);
  col->dist_laser = distance_field(col, DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_NOLASER);
  col->dist_nolaser = distance_field(col, DDNET_TILEFLAG_NOLASER);
  return col->dist_solid && col->dist_hook && col->dist_weapon && col->dist_exists && col->dist_projectile &&
         col->dist_laser && col->dist_nolaser;
}

/* Tile index of a position that is rounded like CheckPoint() does it. */
static inline int tile_at(const collision_t *col, int ix, int iy) {
  /* On the map, which is nearly always, nothing has to be clamped (as
   * unsigned numbers, negative ones are too large as well). */
  const unsigned tx = (unsigned)ix >> 5, ty = (unsigned)iy >> 5;
  if (tx < (unsigned)col->width && ty < (unsigned)col->height)
    return (int)(ty * (unsigned)col->width + tx);
  /* x / 32 and x >> 5 only differ for negative x, which is clamped to 0 either way */
  int nx = clampi(ix >> 5, 0, col->width - 1);
  int ny = clampi(iy >> 5, 0, col->height - 1);
  return ny * col->width + nx;
}

/* Tile index of a position that is truncated like GetMapIndex() does it. */
static inline int tile_at_pos(const collision_t *col, vec2 pos) {
  return tile_at(col, (int)pos.x, (int)pos.y);
}

/* How many of the following samples of a line, which are at most one unit
 * apart, can be skipped when the sample at hand is on a tile with this
 * distance to the nearest tile of interest. */
static inline int samples_to_skip(int dist) { return dist >= 2 ? 32 * (dist - 1) : 1; }

/* CCollision::Init */
/* CDoor::CDoor: where the doors end, "IntersectNoLaser(Pos, Pos + normalize(Direction) * Length, &m_To)" */
static void doors_finish(collision_t *col) {
  for (int i = 0; i < col->num_doors; i++) {
    ddnet_map_door_t *door = &col->doors[i];
    const vec2 to = vadd(door->pos, vscale(vnormalize(door->direction), (float)door->length));
    collision_intersect_no_laser(col, door->pos, to, &door->to, NULL);
  }
}

bool ddnet_collision_init(ddnet_collision_t *col, const map_data_t *map) {
  memset(col, 0, sizeof(*col));
  if (!map || !map->game_layer.data || map->width <= 0 || map->height <= 0)
    return false;

  col->width = map->width;
  col->height = map->height;
  if (!load_layers(col, map))
    goto fail;

  const int count = col->width * col->height;

  if (col->switch_tiles) {
    for (int i = 0; i < count; i++) {
      if (col->switch_tiles[i].number > col->highest_switch_number)
        col->highest_switch_number = col->switch_tiles[i].number;

      col->door[i].number = col->switch_tiles[i].number;

      const unsigned char index = col->switch_tiles[i].type;
      if (index <= TILE_NPH_ENABLE) {
        if ((index >= TILE_JUMP && index <= TILE_SUBTRACT_TIME) || index == TILE_ALLOW_TELE_GUN ||
            index == TILE_ALLOW_BLUE_TELE_GUN) {
          col->switch_tiles[i].type = index;
        } else {
          col->switch_tiles[i].type = 0;
        }
      }
    }
  }

  if (col->tele) {
    for (int i = 0; i < count; i++) {
      const unsigned char number = col->tele[i].number;
      const unsigned char type = col->tele[i].type;
      if (number && type) {
        const vec2 tele_pos = v2(i % col->width * 32.0f + 16.0f, i / col->width * 32.0f + 16.0f);
        ddnet_tele_list_t *list;
        if (type == TILE_TELEIN)
          list = &col->tele_ins[number - 1];
        else if (type == TILE_TELEOUT)
          list = &col->tele_outs[number - 1];
        else if (type == TILE_TELECHECKOUT)
          list = &col->tele_check_outs[number - 1];
        else
          list = &col->tele_others[number - 1];
        if (!tele_list_push(list, tele_pos))
          goto fail;
      }
    }
  }

  if (col->tune) {
    for (int i = 0; i < count; i++)
      if (col->tune[i].type && col->tune[i].number > col->highest_tune_zone)
        col->highest_tune_zone = col->tune[i].number;
  }

  if (!precompute_solid(col) || !load_static_entities(col))
    goto fail;
  if (!precompute(col))
    goto fail;
  if (!precompute_padded(col))
    goto fail;

  if (map->num_settings > 0) {
    col->settings = calloc((size_t)map->num_settings, sizeof(*col->settings));
    if (!col->settings)
      goto fail;
    for (int i = 0; i < map->num_settings; i++) {
      const char *setting = map->settings[i] ? map->settings[i] : "";
      col->settings[i] = malloc(strlen(setting) + 1);
      if (!col->settings[i])
        goto fail;
      strcpy(col->settings[i], setting);
      col->num_settings = i + 1;
    }
  }
  doors_finish(col);
  return true;

fail:
  ddnet_collision_free(col);
  return false;
}

void ddnet_collision_free(ddnet_collision_t *col) {
  free(col->doors);
  free(col->game);
  free(col->front);
  free(col->tele);
  free(col->speedup);
  free(col->switch_tiles);
  free(col->tune);
  free(col->door);
  free(col->tile_flags);
  free(col->pickups);
  free(col->tile_pickups);
  free(col->dist_solid);
  free(col->dist_hook);
  free(col->dist_weapon);
  free(col->dist_projectile);
  free(col->dist_laser);
  free(col->dist_nolaser);
  free(col->sat_solid);
  free(col->sat_hook);
  free(col->sat_projectile);
  free(col->sat_laser);
  free(col->sat_tele);
  free(col->sat_nolaser);
  free(col->sat_exists);
  free(col->line_fractions);
  free(col->dist_exists);
  free(col->tile_handlers);
  free(col->layer_tiles);
  free(col->padded_flags);
  for (int i = 0; i < DDNET_MAX_TELE_NUMBER; i++) {
    free(col->tele_ins[i].positions);
    free(col->tele_outs[i].positions);
    free(col->tele_check_outs[i].positions);
    free(col->tele_others[i].positions);
  }
  for (int i = 0; i < 3; i++)
    free(col->spawn_points[i]);
  for (int i = 0; i < col->num_settings; i++)
    free(col->settings[i]);
  free(col->settings);
  memset(col, 0, sizeof(*col));
}

/* ------------------------------------------------------- move restrictions */

enum { MR_DIR_HERE = 0, MR_DIR_RIGHT, MR_DIR_DOWN, MR_DIR_LEFT, MR_DIR_UP, NUM_MR_DIRS };

static int get_move_restrictions_raw(int tile, int flags) {
  flags = flags & (TILEFLAG_XFLIP | TILEFLAG_YFLIP | TILEFLAG_ROTATE);
  switch (tile) {
  case TILE_STOP:
    switch (flags) {
    case ROTATION_0:
      return DDNET_CANTMOVE_DOWN;
    case ROTATION_90:
      return DDNET_CANTMOVE_LEFT;
    case ROTATION_180:
      return DDNET_CANTMOVE_UP;
    case ROTATION_270:
      return DDNET_CANTMOVE_RIGHT;

    case TILEFLAG_YFLIP ^ ROTATION_0:
      return DDNET_CANTMOVE_UP;
    case TILEFLAG_YFLIP ^ ROTATION_90:
      return DDNET_CANTMOVE_RIGHT;
    case TILEFLAG_YFLIP ^ ROTATION_180:
      return DDNET_CANTMOVE_DOWN;
    case TILEFLAG_YFLIP ^ ROTATION_270:
      return DDNET_CANTMOVE_LEFT;
    }
    break;
  case TILE_STOPS:
    switch (flags) {
    case ROTATION_0:
    case ROTATION_180:
    case TILEFLAG_YFLIP ^ ROTATION_0:
    case TILEFLAG_YFLIP ^ ROTATION_180:
      return DDNET_CANTMOVE_DOWN | DDNET_CANTMOVE_UP;
    case ROTATION_90:
    case ROTATION_270:
    case TILEFLAG_YFLIP ^ ROTATION_90:
    case TILEFLAG_YFLIP ^ ROTATION_270:
      return DDNET_CANTMOVE_LEFT | DDNET_CANTMOVE_RIGHT;
    }
    break;
  case TILE_STOPA:
    return DDNET_CANTMOVE_LEFT | DDNET_CANTMOVE_RIGHT | DDNET_CANTMOVE_UP | DDNET_CANTMOVE_DOWN;
  }
  return 0;
}

static int get_move_restrictions_mask(int direction) {
  switch (direction) {
  case MR_DIR_HERE:
    return 0;
  case MR_DIR_RIGHT:
    return DDNET_CANTMOVE_RIGHT;
  case MR_DIR_DOWN:
    return DDNET_CANTMOVE_DOWN;
  case MR_DIR_LEFT:
    return DDNET_CANTMOVE_LEFT;
  case MR_DIR_UP:
    return DDNET_CANTMOVE_UP;
  default:
    assert(!"invalid direction");
    return 0;
  }
}

static int get_move_restrictions(int direction, int tile, int flags) {
  int result = get_move_restrictions_raw(tile, flags);
  /* Generally, stoppers only have an effect if they block us from moving
   * *onto* them. The one exception is one-way blockers, they can also
   * block us from moving if we're on top of them. */
  if (direction == MR_DIR_HERE && tile == TILE_STOP)
    return result;
  return result & get_move_restrictions_mask(direction);
}

static void get_door_tile(const collision_t *col, int index, ddnet_door_tile_t *door_tile) {
  if (!col->door || index < 0 || !col->door[index].index) {
    door_tile->index = 0;
    door_tile->flags = 0;
    door_tile->number = 0;
    return;
  }
  *door_tile = col->door[index];
}

DDNET_NOINLINE int collision_get_move_restrictions(const collision_t *col, switch_active_fn switch_active,
                                                   void *user, vec2 pos, float distance,
                                                   int override_center_tile_index) {
  static const vec2 DIRECTIONS[NUM_MR_DIRS] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
  assert(0.0f <= distance && distance <= 32.0f);

  /* The five positions that are looked at are on the tile of the tee or next
   * to it, or the tile is given. Nothing to do if there are no stoppers. */
  if (!(col->tile_flags[collision_get_pure_map_index(col, pos)] & DDNET_TILEFLAG_NEAR_STOPPER) &&
      (override_center_tile_index < 0 ||
       !(col->tile_flags[override_center_tile_index] & DDNET_TILEFLAG_NEAR_STOPPER)))
    return 0;

  int restrictions = 0;
  for (int d = 0; d < NUM_MR_DIRS; d++) {
    vec2 mod_pos = vadd(pos, vscale(DIRECTIONS[d], distance));
    int mod_map_index = collision_get_pure_map_index(col, mod_pos);
    if (d == MR_DIR_HERE && override_center_tile_index >= 0)
      mod_map_index = override_center_tile_index;
    for (int front = 0; front < 2; front++) {
      int tile;
      int flags;
      if (!front) {
        tile = collision_get_tile_index(col, mod_map_index);
        flags = collision_get_tile_flags(col, mod_map_index);
      } else {
        tile = collision_get_front_tile_index(col, mod_map_index);
        flags = collision_get_front_tile_flags(col, mod_map_index);
      }
      restrictions |= get_move_restrictions(d, tile, flags);
    }
    if (switch_active) {
      ddnet_door_tile_t door_tile;
      get_door_tile(col, mod_map_index, &door_tile);
      if ((int)door_tile.number <= col->highest_switch_number && switch_active(door_tile.number, user))
        restrictions |= get_move_restrictions(d, door_tile.index, door_tile.flags);
    }
  }
  return restrictions;
}

/* ------------------------------------------------------------ solid tiles */

int collision_get_tile(const collision_t *col, int x, int y) {
  if (!col->game)
    return 0;

  int nx = clampi(x >> 5, 0, col->width - 1);
  int ny = clampi(y >> 5, 0, col->height - 1);
  const int index = ny * col->width + nx;

  if (col->game[index].index >= TILE_SOLID && col->game[index].index <= TILE_NOLASER)
    return col->game[index].index;
  return 0;
}

int collision_get_front_tile(const collision_t *col, int x, int y) {
  if (!col->front)
    return 0;
  int nx = clampi(x >> 5, 0, col->width - 1);
  int ny = clampi(y >> 5, 0, col->height - 1);
  if (col->front[ny * col->width + nx].index == TILE_DEATH ||
      col->front[ny * col->width + nx].index == TILE_NOLASER)
    return col->front[ny * col->width + nx].index;
  else
    return 0;
}

DDNET_HOT int collision_is_solid(const collision_t *col, int x, int y) {
  return col->tile_flags[tile_at(col, x, y)] & DDNET_TILEFLAG_SOLID;
}

DDNET_HOT bool collision_check_point(const collision_t *col, float x, float y) {
  return collision_is_solid(col, round_fast(x), round_fast(y));
}

int collision_get_collision_at(const collision_t *col, float x, float y) {
  return collision_get_tile(col, round_fast(x), round_fast(y));
}

int collision_get_front_collision_at(const collision_t *col, float x, float y) {
  return collision_get_front_tile(col, round_fast(x), round_fast(y));
}

static int is_no_laser(const collision_t *col, int x, int y) {
  return collision_get_tile(col, x, y) == TILE_NOLASER;
}

static int get_pure_map_index_xy(const collision_t *col, float x, float y) {
  int nx = clampi(round_fast(x) >> 5, 0, col->width - 1);
  int ny = clampi(round_fast(y) >> 5, 0, col->height - 1);
  return ny * col->width + nx;
}

int collision_get_pure_map_index(const collision_t *col, vec2 pos) {
  return get_pure_map_index_xy(col, pos.x, pos.y);
}

/* ---------------------------------------------------------- line tracing */

/* Offset for checking the "through" tile behind a hookthrough tile. */
static void through_offset(vec2 pos0, vec2 pos1, int *offset_x, int *offset_y) {
  float x = pos0.x - pos1.x;
  float y = pos0.y - pos1.y;
  if (fabsf(x) > fabsf(y)) {
    if (x < 0) {
      *offset_x = -32;
      *offset_y = 0;
    } else {
      *offset_x = 32;
      *offset_y = 0;
    }
  } else {
    if (y < 0) {
      *offset_x = 0;
      *offset_y = -32;
    } else {
      *offset_x = 0;
      *offset_y = 32;
    }
  }
}

static bool is_through(const collision_t *col, int x, int y, int offset_x, int offset_y, vec2 pos0,
                       vec2 pos1) {
  const int index = get_pure_map_index_xy(col, x, y);
  if (col->front &&
      (col->front[index].index == TILE_THROUGH_ALL || col->front[index].index == TILE_THROUGH_CUT))
    return true;
  if (col->front && col->front[index].index == TILE_THROUGH_DIR &&
      ((col->front[index].flags == ROTATION_0 && pos0.y > pos1.y) ||
       (col->front[index].flags == ROTATION_90 && pos0.x < pos1.x) ||
       (col->front[index].flags == ROTATION_180 && pos0.y < pos1.y) ||
       (col->front[index].flags == ROTATION_270 && pos0.x > pos1.x)))
    return true;
  const int offset_index = get_pure_map_index_xy(col, x + offset_x, y + offset_y);
  return col->game[offset_index].index == TILE_THROUGH ||
         (col->front && col->front[offset_index].index == TILE_THROUGH);
}

static bool is_hook_blocker(const collision_t *col, int x, int y, vec2 pos0, vec2 pos1) {
  const int index = get_pure_map_index_xy(col, x, y);
  if (col->game[index].index == TILE_THROUGH_ALL ||
      (col->front && col->front[index].index == TILE_THROUGH_ALL))
    return true;
  if (col->game[index].index == TILE_THROUGH_DIR &&
      ((col->game[index].flags == ROTATION_0 && pos0.y < pos1.y) ||
       (col->game[index].flags == ROTATION_90 && pos0.x > pos1.x) ||
       (col->game[index].flags == ROTATION_180 && pos0.y > pos1.y) ||
       (col->game[index].flags == ROTATION_270 && pos0.x < pos1.x)))
    return true;
  if (col->front && col->front[index].index == TILE_THROUGH_DIR &&
      ((col->front[index].flags == ROTATION_0 && pos0.y < pos1.y) ||
       (col->front[index].flags == ROTATION_90 && pos0.x > pos1.x) ||
       (col->front[index].flags == ROTATION_180 && pos0.y > pos1.y) ||
       (col->front[index].flags == ROTATION_270 && pos0.x < pos1.x)))
    return true;
  return false;
}

/* The lines below are sampled like DDNet samples them, about once per unit,
 * and every sample that is looked at is computed exactly like DDNet computes
 * it. What is skipped are samples that provably see nothing:
 *
 * - A tile knows how far away the nearest tile of interest is, and consecutive
 *   samples are at most one unit apart.
 * - The samples of a line that are on one tile are consecutive: the
 *   coordinates of the samples never decrease or never increase along the
 *   line, and neither does rounding them. So if two samples are on the same
 *   uninteresting tile, everything between them can be skipped. */

/* A line whose sample i is mix(pos0, pos1, i / (float)end), on the tile of its
 * rounded coordinates. */
typedef struct line_t {
  const collision_t *col;
  vec2 pos0, pos1;
  int end; /* the last sample */
  /* sample i is at i / denom of the way, with one unit or less between two samples */
  float denom;
  /* samples per unit of x and y, to guess where the line leaves a tile */
  float per_x, per_y;
  /* i / denom for every i, where denom is a whole number small enough (line_fractions), or NULL */
  const float *fractions;
  /* the last sample that was worked out, for the caller to take instead of working it out again */
  int last;
  vec2 last_pos;
} line_t;

static inline line_t line_begin_samples(const collision_t *col, vec2 pos0, vec2 pos1, int end, float denom) {
  line_t line = {col, pos0, pos1, end, denom, 0.0f, 0.0f, NULL, -1, {0.0f, 0.0f}};
  if (denom == (float)end && end >= 1 && end <= DDNET_LINE_FRACTIONS)
    line.fractions = col->line_fractions + end * (end + 1) / 2;
  const float dx = pos1.x - pos0.x, dy = pos1.y - pos0.y;
  if (dx != 0)
    line.per_x = denom / dx;
  if (dy != 0)
    line.per_y = denom / dy;
  return line;
}

/* The samples 0 to end at i / (float)end. */
static inline line_t line_begin(const collision_t *col, vec2 pos0, vec2 pos1, int end) {
  return line_begin_samples(col, pos0, pos1, end, (float)end);
}

/* i / denom: the part of the way to sample i (i from 0 to end) */
static inline float line_fraction(const line_t *line, int i) {
  return line->fractions ? line->fractions[i] : i / line->denom;
}

static inline vec2 line_sample(const line_t *line, int i) {
  PROF_COUNT(1, "line samples (search)");
  return vmix(line->pos0, line->pos1, line_fraction(line, i));
}

/* (remembered: the caller looks at the first sample on the next tile next, see line_leave_tile()) */
static inline int line_tile(line_t *line, int i) {
  const vec2 pos = line_sample(line, i);
  line->last = i;
  line->last_pos = pos;
  return tile_at(line->col, round_fast(pos.x), round_fast(pos.y));
}

/* Sample i of the main loop of a trace, which the search for the next tile may have worked out already */
static inline vec2 line_main_sample(const line_t *line, int i) {
  return line->last == i ? line->last_pos : vmix(line->pos0, line->pos1, line_fraction(line, i));
}

/* The first sample after sample i that is not on the same tile as sample i,
 * which is at pos. Only the result of the search is a guess that needs no
 * checking: everything is verified with the real samples. */
static inline int line_leave_tile(line_t *line, int i, vec2 pos, int tile) {
  /* where the line would cross the border of the tile, in samples from pos0 */
  float guess = (float)(line->end + 1);
  if (line->per_x != 0) {
    const int tx = round_fast(pos.x) / 32;
    const float border = line->per_x > 0 ? tx * 32.0f + 31.5f : tx * 32.0f - 0.5f;
    guess = (border - line->pos0.x) * line->per_x;
  }
  if (line->per_y != 0) {
    const int ty = round_fast(pos.y) / 32;
    const float border = line->per_y > 0 ? ty * 32.0f + 31.5f : ty * 32.0f - 0.5f;
    const float guess_y = (border - line->pos0.y) * line->per_y;
    if (guess_y < guess)
      guess = guess_y;
  }
  int next = i + 1;
  if (guess > (float)next)
    next = guess < (float)(line->end + 1) ? (int)guess : line->end + 1;

  /* the last sample before next has to be on the tile ... */
  while (next - 1 > i && line_tile(line, next - 1) != tile)
    next--;
  /* ... and next must not be */
  while (next <= line->end && line_tile(line, next) == tile)
    next++;
  return next;
}

/* The sample to look at after sample i, which is on a tile of no interest. */
static inline int line_skip(line_t *line, int i, vec2 pos, int tile, const uint8_t *dist) {
  const int d = dist[tile];
  if (d >= 2)
    return i + 32 * (d - 1);
  return line_leave_tile(line, i, pos, tile);
}

int collision_intersect_line(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                             vec2 *out_before_collision) {
  float distance = vdistance(pos0, pos1);
  int end = (int)(distance + 1);
  /* all samples are between the two ends: nothing to hit if nothing is there */
  if (distance < 1e6f && rect_is_free(col, col->sat_solid, minf(pos0.x, pos1.x), minf(pos0.y, pos1.y),
                                      maxf(pos0.x, pos1.x), maxf(pos0.y, pos1.y))) {
    if (out_collision)
      *out_collision = pos1;
    if (out_before_collision)
      *out_before_collision = pos1;
    return 0;
  }
  line_t line = line_begin(col, pos0, pos1, end);
  for (int i = 0; i <= end;) {
    const vec2 pos = line_main_sample(&line, i);
    /* Temporary position for checking collision */
    int ix = round_fast(pos.x);
    int iy = round_fast(pos.y);
    const int tile = tile_at(col, ix, iy);

    if (col->tile_flags[tile] & DDNET_TILEFLAG_SOLID) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = i == 0 ? pos0 : vmix(pos0, pos1, line_fraction(&line, i - 1));
      return collision_get_collision_at(col, ix, iy);
    }

    i = line_skip(&line, i, pos, tile, col->dist_solid);
  }
  if (out_collision)
    *out_collision = pos1;
  if (out_before_collision)
    *out_before_collision = pos1;
  return 0;
}

DDNET_HOT int collision_intersect_line_tele_hook(const collision_t *col, bool old_teleport_hook, vec2 pos0,
                                                 vec2 pos1, vec2 *out_collision, vec2 *out_before_collision,
                                                 int *tele_nr) {
  PROF_COUNT(5, "hook lines");
  /* All samples are between the two ends: nothing to hit if nothing is there.
   * This comes first: most lines end here, and need neither their length nor
   * the offset below. (The coordinates have to be such that their tiles can
   * be told.) */
  if (!old_teleport_hook && fabsf(pos0.x) < 1e6f && fabsf(pos0.y) < 1e6f && fabsf(pos1.x) < 1e6f &&
      fabsf(pos1.y) < 1e6f &&
      rect_is_free(col, col->sat_hook, minf(pos0.x, pos1.x), minf(pos0.y, pos1.y), maxf(pos0.x, pos1.x),
                   maxf(pos0.y, pos1.y))) {
    if (tele_nr)
      *tele_nr = 0;
    if (out_collision)
      *out_collision = pos1;
    if (out_before_collision)
      *out_before_collision = pos1;
    return 0;
  }
  float distance = vdistance(pos0, pos1);
  int end = (int)(distance + 1);
  int dx = 0, dy = 0; /* Offset for checking the "through" tile */
  through_offset(pos0, pos1, &dx, &dy);
  const int relevant =
      DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_HOOK | (old_teleport_hook ? DDNET_TILEFLAG_TELEIN : 0);
  line_t line = line_begin(col, pos0, pos1, end);
  PROF_COUNT(6, "hook lines traced");
  for (int i = 0; i <= end;) {
    PROF_COUNT(7, "hook line samples (main)");
    const vec2 pos = line_main_sample(&line, i);
    /* Temporary position for checking collision */
    int ix = round_fast(pos.x);
    int iy = round_fast(pos.y);

    /* the same tile as GetPureMapIndex(Pos) */
    const int index = tile_at(col, ix, iy);
    const int flags = col->tile_flags[index];
    if (!(flags & relevant)) {
      if (tele_nr)
        *tele_nr = 0;
      /* with sv_old_teleport_hook the normal teleporters matter too, which the distances do not include */
      i = old_teleport_hook ? i + 1 : line_skip(&line, i, pos, index, col->dist_hook);
      continue;
    }

    if (tele_nr) {
      if (old_teleport_hook)
        *tele_nr = collision_is_teleport(col, index);
      else
        *tele_nr = collision_is_teleport_hook(col, index);
    }
    if (tele_nr && *tele_nr) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = i == 0 ? pos0 : vmix(pos0, pos1, line_fraction(&line, i - 1));
      return TILE_TELEINHOOK;
    }

    int hit = 0;
    if (flags & DDNET_TILEFLAG_SOLID) {
      if (!is_through(col, ix, iy, dx, dy, pos0, pos1))
        hit = collision_get_collision_at(col, ix, iy);
    } else if (is_hook_blocker(col, ix, iy, pos0, pos1)) {
      hit = TILE_NOHOOK;
    }
    if (hit) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = i == 0 ? pos0 : vmix(pos0, pos1, line_fraction(&line, i - 1));
      return hit;
    }

    i++;
  }
  if (out_collision)
    *out_collision = pos1;
  if (out_before_collision)
    *out_before_collision = pos1;
  return 0;
}

int collision_intersect_line_tele_weapon(const collision_t *col, bool old_teleport_weapons, vec2 pos0,
                                         vec2 pos1, vec2 *out_collision, vec2 *out_before_collision,
                                         int *tele_nr) {
  float distance = vdistance(pos0, pos1);
  int end = (int)(distance + 1);
  const int relevant =
      DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_WEAPON | (old_teleport_weapons ? DDNET_TILEFLAG_TELEIN : 0);
  line_t line = line_begin(col, pos0, pos1, end);
  for (int i = 0; i <= end;) {
    const vec2 pos = line_main_sample(&line, i);
    /* Temporary position for checking collision */
    int ix = round_fast(pos.x);
    int iy = round_fast(pos.y);

    /* the same tile as GetPureMapIndex(Pos) */
    const int index = tile_at(col, ix, iy);
    const int flags = col->tile_flags[index];
    if (!(flags & relevant)) {
      if (tele_nr)
        *tele_nr = 0;
      i = old_teleport_weapons ? i + 1 : line_skip(&line, i, pos, index, col->dist_weapon);
      continue;
    }

    if (tele_nr) {
      if (old_teleport_weapons)
        *tele_nr = collision_is_teleport(col, index);
      else
        *tele_nr = collision_is_teleport_weapon(col, index);
    }
    if (tele_nr && *tele_nr) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = i == 0 ? pos0 : vmix(pos0, pos1, line_fraction(&line, i - 1));
      return TILE_TELEINWEAPON;
    }

    if (flags & DDNET_TILEFLAG_SOLID) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = i == 0 ? pos0 : vmix(pos0, pos1, line_fraction(&line, i - 1));
      return collision_get_collision_at(col, ix, iy);
    }

    i++;
  }
  if (out_collision)
    *out_collision = pos1;
  if (out_before_collision)
    *out_before_collision = pos1;
  return 0;
}

static int get_front_index_xy(const collision_t *col, int nx, int ny) {
  if (!col->front)
    return 0;
  return col->front[ny * col->width + nx].index;
}

int collision_intersect_no_laser(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                                 vec2 *out_before_collision) {
  float distance = vdistance(pos0, pos1);
  /* all samples are between the two ends: nothing to hit if nothing is there */
  if (distance < 1e6f && rect_is_free(col, col->sat_laser, minf(pos0.x, pos1.x), minf(pos0.y, pos1.y),
                                      maxf(pos0.x, pos1.x), maxf(pos0.y, pos1.y))) {
    if (out_collision)
      *out_collision = pos1;
    if (out_before_collision)
      *out_before_collision = pos1;
    return 0;
  }

  const int distance_rounded = (int)ceilf(distance);
  line_t line = line_begin_samples(col, pos0, pos1, distance_rounded - 1, distance);
  for (int i = 0; i < distance_rounded;) {
    float a = i / distance;
    vec2 pos = vmix(pos0, pos1, a);
    int nx = clampi(round_fast(pos.x) >> 5, 0, col->width - 1);
    int ny = clampi(round_fast(pos.y) >> 5, 0, col->height - 1);
    const int tile = ny * col->width + nx;
    /* solid, unhookable or stopping lasers in the game or front layer */
    if (col->tile_flags[tile] & (DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_NOLASER)) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = i == 0 ? pos0 : vmix(pos0, pos1, (i - 1) / distance);
      if (get_front_index_xy(col, nx, ny) == TILE_NOLASER)
        return collision_get_front_collision_at(col, pos.x, pos.y);
      else
        return collision_get_collision_at(col, pos.x, pos.y);
    }
    i = line_skip(&line, i, pos, tile, col->dist_laser);
  }
  if (out_collision)
    *out_collision = pos1;
  if (out_before_collision)
    *out_before_collision = pos1;
  return 0;
}

int collision_intersect_no_laser_no_walls(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                                          vec2 *out_before_collision) {
  float distance = vdistance(pos0, pos1);
  /* all samples are between the two ends: nothing to hit if nothing is there */
  if (distance < 1e6f && rect_is_free(col, col->sat_nolaser, minf(pos0.x, pos1.x), minf(pos0.y, pos1.y),
                                      maxf(pos0.x, pos1.x), maxf(pos0.y, pos1.y))) {
    if (out_collision)
      *out_collision = pos1;
    if (out_before_collision)
      *out_before_collision = pos1;
    return 0;
  }

  const int distance_rounded = (int)ceilf(distance);
  line_t line = line_begin_samples(col, pos0, pos1, distance_rounded - 1, distance);
  for (int i = 0; i < distance_rounded;) {
    float a = (float)i / distance;
    vec2 pos = vmix(pos0, pos1, a);
    const int tile = tile_at(col, round_fast(pos.x), round_fast(pos.y));
    if (!(col->tile_flags[tile] & DDNET_TILEFLAG_NOLASER)) {
      i = line_skip(&line, i, pos, tile, col->dist_nolaser);
      continue;
    }
    {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = i == 0 ? pos0 : vmix(pos0, pos1, (float)(i - 1) / distance);
      if (is_no_laser(col, round_fast(pos.x), round_fast(pos.y)))
        return collision_get_collision_at(col, pos.x, pos.y);
      else
        return collision_get_front_collision_at(col, pos.x, pos.y);
    }
  }
  if (out_collision)
    *out_collision = pos1;
  if (out_before_collision)
    *out_before_collision = pos1;
  return 0;
}

/* ---------------------------------------------------------------- moving */

/* True if a tile is out of sight from a position altogether because of a
 * blocking tile: a trace from there to any position on the tile has a sample
 * on the blocking tile. The samples of a trace are a unit apart or less, so it
 * is enough that its line is inside of the blocking tile for more than that:
 * that it comes closer than 14 units to the middle of it, which makes more
 * than two units inside of a circle that is well within the tile, and that
 * the circle is on the way and not beyond the end. The lines to all positions
 * on a tile are between the lines to its corners, and what comes that close to
 * a point for the corners does for all of them. */
static bool sight_tile_hidden(const collision_t *col, vec2 from, int tile, int blocker, int blocking) {
  const double cx = blocker % col->width * 32.0 + 16.0 - from.x,
               cy = blocker / col->width * 32.0 + 16.0 - from.y;
  const double to_blocker = sqrt(cx * cx + cy * cy);
  /* not from inside of it or right next to it */
  if (!(to_blocker > 32.0))
    return false;
  const double tile_x = tile % col->width * 32.0 - from.x, tile_y = tile / col->width * 32.0 - from.y;
  bool hidden = true;
  for (int corner = 0; corner < 4 && hidden; corner++) {
    const double kx = tile_x + (corner & 1 ? 32.0 : 0.0), ky = tile_y + (corner & 2 ? 32.0 : 0.0);
    const double length_squared = kx * kx + ky * ky;
    const double cross = cx * ky - cy * kx, dot = cx * kx + cy * ky;
    /* the line to the corner goes by the middle of the blocking tile closely, in front, and every
     * position on the tile is behind the circle by some units (a tile is 46 across) */
    hidden = dot > 0.0 && cross * cross <= 14.0 * 14.0 * length_squared &&
             sqrt(length_squared) - 46.0 > to_blocker + 14.5 + 4.0;
  }
  if (hidden)
    return true;

  /* A wall: the blocking tiles in a row or a column with the one that is known.
   * A line that crosses the line through their middles, between the first and
   * the last middle, is inside of them for 14 units before and after. That it
   * does is true for all positions on the tile if it is for its corners. The
   * tile has to be two rows (columns) or more beyond the wall, and the other
   * end not in it. */
  const int bx = blocker % col->width, by = blocker / col->width;
  const int tx = tile % col->width, ty = tile / col->width;
  for (int vertical = 0; vertical < 2; vertical++) {
    /* along: the coordinate along the wall, across: the one across it */
    const int tile_across = vertical ? tx : ty, wall_across = vertical ? bx : by;
    if (tile_across - wall_across < 2 && wall_across - tile_across < 2)
      continue;
    const double wall = wall_across * 32.0 + 16.0;
    const double from_across = vertical ? from.x : from.y, from_along = vertical ? from.y : from.x;
    /* the two ends on different sides of the wall, the near one out of it */
    if (!((from_across < wall - 18.0 && tile_across > wall_across) ||
          (from_across > wall + 18.0 && tile_across < wall_across)))
      continue;
    int first = vertical ? by : bx, last = first;
    const int limit = vertical ? col->height : col->width;
    while (
        first > 0 && last - first < 64 &&
        (col->tile_flags[vertical ? (first - 1) * col->width + bx : by * col->width + first - 1] & blocking))
      first--;
    while (last < limit - 1 && last - first < 128 &&
           (col->tile_flags[vertical ? (last + 1) * col->width + bx : by * col->width + last + 1] & blocking))
      last++;
    const double lowest = first * 32.0 + 16.0, highest = last * 32.0 + 16.0;
    bool crossed = true;
    for (int corner = 0; corner < 4 && crossed; corner++) {
      const double k_across = (vertical ? tx : ty) * 32.0 + (corner & 1 ? 32.0 : 0.0);
      const double k_along = (vertical ? ty : tx) * 32.0 + (corner & 2 ? 32.0 : 0.0);
      /* where the line to the corner crosses the middle of the wall */
      const double t = (wall - from_across) / (k_across - from_across);
      const double at = from_along + t * (k_along - from_along);
      crossed = t > 0.0 && t < 1.0 && at >= lowest && at <= highest;
    }
    if (crossed)
      return true;
  }
  return false;
}

/* Whether something is in the way between two positions: what one of the line
 * traces returns, as true or false. kind 0: collision_intersect_line, 1:
 * collision_intersect_no_laser, 2: collision_intersect_no_laser_no_walls.
 *
 * What was in the way last time usually still is. sight remembers that tile:
 * if one of the samples of the trace is on it, the trace stops there or before
 * it, with something in the way either way. And if that tile hides all of the
 * tile the other position is on, there is nothing to trace. */
bool collision_sight_blocked(const collision_t *col, int kind, vec2 pos0, vec2 pos1, ddnet_sight_t *sight,
                             ddnet_sight_memo_t *memo, int slot) {
  static const int FLAGS[3] = {DDNET_TILEFLAG_SOLID, DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_NOLASER,
                               DDNET_TILEFLAG_NOLASER};
  PROF_COUNT(30, "sight checks");
  /* the tile of the other position, if it is on the map (and positions near it are what they seem) */
  const bool on_map = pos1.x >= 0 && pos1.y >= 0 && pos1.x < col->width * 32.0f &&
                      pos1.y < col->height * 32.0f && fabsf(pos0.x) < 1e6f && fabsf(pos0.y) < 1e6f;
  const int there = on_map ? ((int)pos1.y >> 5) * col->width + ((int)pos1.x >> 5) : -1;
  bool blocker_known = sight->blocker > 0 && (col->tile_flags[sight->blocker - 1] & FLAGS[kind]);

  /* is all of that tile known to be out of sight, or can that be shown now? */
  ddnet_sight_memo_entry_t *known = NULL;
  int state = 0;
  if (memo && on_map) {
    /* What is known about a tile for the entities of the map is next to
     * each other (slot is a number of the one that looks): they are asked
     * one after the other about the same tile. */
    const uint32_t hash = (uint32_t)(there * 4 + kind) * 0x9E3779B1u;
    known = &memo->entries[((hash >> 12) + (uint32_t)slot) % DDNET_SIGHT_MEMO_SIZE];
    if (known->state && known->tile == there * 4 + kind && memcmp(&known->from, &pos0, sizeof(pos0)) == 0)
      state = known->state;
    if (state == 1) {
      PROF_COUNT(31, "sight: tile known to be hidden");
      return true;
    }
    if (blocker_known && state != sight->blocker + 1) {
      state = sight_tile_hidden(col, pos0, there, sight->blocker - 1, FLAGS[kind]) ? 1 : sight->blocker + 1;
      known->from = pos0;
      known->tile = there * 4 + kind;
      known->state = state;
      if (state == 1) {
        PROF_COUNT(26, "sight: tile proven hidden");
        return true;
      }
    }
  }

  bool blocked = false;
  const float distance = vdistance(pos0, pos1);
  if (blocker_known && distance > 0 && distance < 1e6f) {
    const int tile = sight->blocker - 1;
    /* the samples: 0 to end at i / denom of the way */
    int end;
    float denom;
    if (kind == 0) {
      end = (int)(distance + 1);
      denom = (float)end;
    } else {
      end = (int)ceilf(distance) - 1;
      denom = distance;
    }
    /* the one that is the closest to the middle of the tile, and the ones around it */
    const vec2 center = v2(tile % col->width * 32.0f + 15.5f, tile / col->width * 32.0f + 15.5f);
    const vec2 way = vsub(pos1, pos0);
    const float along = clampf(vdot(vsub(center, pos0), way) / vdot(way, way), 0.0f, 1.0f);
    const int nearest = (int)(along * denom);
    for (int i = maxi(nearest - 2, 0); i <= mini(nearest + 3, end) && !blocked; i++) {
      const vec2 pos = vmix(pos0, pos1, i / denom);
      blocked = tile_at(col, round_fast(pos.x), round_fast(pos.y)) == tile;
    }
  }

  if (!blocked) {
    PROF_COUNT(27, "sight: full traces");
    vec2 at;
    int hit;
    if (kind == 0)
      hit = collision_intersect_line(col, pos0, pos1, &at, NULL);
    else if (kind == 1)
      hit = collision_intersect_no_laser(col, pos0, pos1, &at, NULL);
    else
      hit = collision_intersect_no_laser_no_walls(col, pos0, pos1, &at, NULL);
    sight->blocker = hit ? tile_at(col, round_fast(at.x), round_fast(at.y)) + 1 : 0;
    blocked = hit != 0;
    /* with the tile that is in the way now */
    if (blocked && known && state != sight->blocker + 1) {
      known->from = pos0;
      known->tile = there * 4 + kind;
      known->state =
          sight_tile_hidden(col, pos0, there, sight->blocker - 1, FLAGS[kind]) ? 1 : sight->blocker + 1;
    }
  }
  return blocked;
}

/* True if it can be shown that collision_sight_blocked() is true from a
 * position to every position on a tile of the map: something is in the way to
 * the middle or a corner of the tile, and that tile hides all of it. */
bool collision_tile_out_of_sight(const collision_t *col, int kind, vec2 from, int tile) {
  static const int FLAGS[3] = {DDNET_TILEFLAG_SOLID, DDNET_TILEFLAG_SOLID | DDNET_TILEFLAG_NOLASER,
                               DDNET_TILEFLAG_NOLASER};
  if (!(fabsf(from.x) < 1e6f && fabsf(from.y) < 1e6f))
    return false;
  const float x = tile % col->width * 32.0f, y = tile / col->width * 32.0f;
  const vec2 to[5] = {v2(x + 16.0f, y + 16.0f), v2(x + 1.0f, y + 1.0f), v2(x + 31.0f, y + 1.0f),
                      v2(x + 1.0f, y + 31.0f), v2(x + 31.0f, y + 31.0f)};
  for (int i = 0; i < 1; i++) {
    vec2 at;
    int hit;
    if (kind == 0)
      hit = collision_intersect_line(col, from, to[i], &at, NULL);
    else if (kind == 1)
      hit = collision_intersect_no_laser(col, from, to[i], &at, NULL);
    else
      hit = collision_intersect_no_laser_no_walls(col, from, to[i], &at, NULL);
    /* in sight */
    if (!hit)
      return false;
    const int blocker = tile_at(col, round_fast(at.x), round_fast(at.y));
    if ((col->tile_flags[blocker] & FLAGS[kind]) && sight_tile_hidden(col, from, tile, blocker, FLAGS[kind]))
      return true;
  }
  return false;
}

void collision_move_point(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, float elasticity,
                          int *bounces) {
  if (bounces)
    *bounces = 0;

  vec2 pos = *inout_pos;
  vec2 vel = *inout_vel;
  if (collision_check_point(col, pos.x + vel.x, pos.y + vel.y)) {
    int affected = 0;
    if (collision_check_point(col, pos.x + vel.x, pos.y)) {
      inout_vel->x *= -elasticity;
      if (bounces)
        (*bounces)++;
      affected++;
    }

    if (collision_check_point(col, pos.x, pos.y + vel.y)) {
      inout_vel->y *= -elasticity;
      if (bounces)
        (*bounces)++;
      affected++;
    }

    if (affected == 0) {
      inout_vel->x *= -elasticity;
      inout_vel->y *= -elasticity;
    }
  } else {
    *inout_pos = vadd(pos, vel);
  }
}

DDNET_HOT bool collision_test_box(const collision_t *col, vec2 pos, vec2 size) {
  PROF_COUNT(0, "test_box");
  size = vscale(size, 0.5f);
  const uint16_t *flags = col->tile_flags;
  /* A tee, well inside of the map: the corners are 14 from the middle, which
   * can be taken off after rounding as well (both are exact for coordinates
   * like these), and they are on the map. */
  if (size.x == 14.0f && size.y == 14.0f && pos.x > 32.0f && pos.y > 32.0f && pos.x < col->inner_width &&
      pos.y < col->inner_height) {
    const int x = (int)(pos.x + 0.5f), y = (int)(pos.y + 0.5f);
    const int left = (x - 14) >> 5, right = (x + 14) >> 5;
    const int top = ((y - 14) >> 5) * col->width, bottom = ((y + 14) >> 5) * col->width;
    return (flags[top + left] | flags[top + right] | flags[bottom + left] | flags[bottom + right]) &
           DDNET_TILEFLAG_SOLID;
  }
  /* the four corners are on at most two columns and two rows of tiles */
  const int x0 = clampi(round_fast(pos.x - size.x) >> 5, 0, col->width - 1);
  const int x1 = clampi(round_fast(pos.x + size.x) >> 5, 0, col->width - 1);
  const int y0 = clampi(round_fast(pos.y - size.y) >> 5, 0, col->height - 1) * col->width;
  const int y1 = clampi(round_fast(pos.y + size.y) >> 5, 0, col->height - 1) * col->width;
  return (flags[y0 + x0] | flags[y0 + x1] | flags[y1 + x0] | flags[y1 + x1]) & DDNET_TILEFLAG_SOLID;
}

/* collision_is_on_ground() for a position on the map: the two points are
 * at most a tile beyond it, in the padding (padded_origin). */
bool collision_is_on_ground_on_map(const collision_t *col, vec2 pos, float size) {
  /* round_to_int(f) is (int)(f + 0.5f) for f > 0, which pos.x + 14 and
   * pos.y + 19 are. pos.x - 14 may be below 0 by less than 14: then both
   * round to a number from -14 to 0, which is on column -1 or 0, and the
   * padding repeats column 0 in column -1. */
  const int y = (int)(pos.y + size / 2 + 5 + 0.5f) >> 5;
  const int right = (int)(pos.x + size / 2 + 0.5f) >> 5, left = (int)(pos.x - size / 2 + 0.5f) >> 5;
  const uint16_t *row = col->padded_origin + (ptrdiff_t)y * col->padded_width;
  return ((row[right] | row[left]) & DDNET_TILEFLAG_SOLID) != 0;
}

DDNET_HOT bool collision_is_on_ground(const collision_t *col, vec2 pos, float size) {
  if (collision_check_point(col, pos.x + size / 2, pos.y + size / 2 + 5))
    return true;
  if (collision_check_point(col, pos.x - size / 2, pos.y + size / 2 + 5))
    return true;

  return false;
}

/* pos + way, for a caller that rounds the position to whole numbers afterwards
 * (round_to_int) and uses nothing else of it. DDNet goes the way in a number
 * of steps, adding a part of it to the position every time, which rounds on
 * every addition and does not come out at exactly this. But it comes out
 * close: every addition is off by half a unit in the last place of the largest
 * position at most, and the parts add up to the way within a few units in its
 * last place. If the sum here is further than all of that (taken twice, to be
 * safe) from where rounding to a whole number changes, both round to the same
 * number. Returns false if it is not, or if that cannot be said. steps is the
 * number of steps or more. The result is less than 0.2 off. */
static inline bool rounds_like_steps(vec2 pos, vec2 way, float steps, vec2 *out) {
#ifdef __SSE2__
  /* both coordinates at once */
  const __m128 sign = _mm_set1_ps(-0.0f);
  const __m128 p = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&pos));
  const __m128 v = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&way));
  const __m128 direct = _mm_add_ps(p, v);
  /* nothing on the way is larger than this */
  const __m128 largest =
      _mm_add_ps(_mm_add_ps(_mm_andnot_ps(sign, p), _mm_andnot_ps(sign, v)), _mm_set1_ps(1.0f));
  const __m128 error =
      _mm_add_ps(_mm_mul_ps(largest, _mm_set1_ps((steps + 4.0f) * 1.2e-7f)), _mm_set1_ps(2e-5f));
  /* rounding changes halfway between two whole numbers, for negative ones too */
  const __m128 size = _mm_andnot_ps(sign, direct);
  const __m128 fraction = _mm_sub_ps(size, _mm_cvtepi32_ps(_mm_cvttps_epi32(size)));
  const __m128 distance = _mm_andnot_ps(sign, _mm_sub_ps(fraction, _mm_set1_ps(0.5f)));
  const __m128 fine =
      _mm_and_ps(_mm_and_ps(_mm_cmplt_ps(largest, _mm_set1_ps(1e6f)), _mm_cmpgt_ps(distance, error)),
                 _mm_cmplt_ps(error, _mm_set1_ps(0.2f)));
  if ((_mm_movemask_ps(fine) & 3) != 3)
    return false;
  _mm_storel_epi64((__m128i *)out, _mm_castps_si128(direct));
  return true;
#else
  const vec2 direct = v2(pos.x + way.x, pos.y + way.y);
  const float largest_x = absolutef(pos.x) + absolutef(way.x) + 1.0f;
  const float largest_y = absolutef(pos.y) + absolutef(way.y) + 1.0f;
  const float error_x = (steps + 4.0f) * 1.2e-7f * largest_x + 2e-5f;
  const float error_y = (steps + 4.0f) * 1.2e-7f * largest_y + 2e-5f;
  if (!(largest_x < 1e6f && largest_y < 1e6f && error_x < 0.2f && error_y < 0.2f))
    return false;
  float fraction_x = absolutef(direct.x), fraction_y = absolutef(direct.y);
  fraction_x -= (float)(int)fraction_x;
  fraction_y -= (float)(int)fraction_y;
  if (!(absolutef(fraction_x - 0.5f) > error_x && absolutef(fraction_y - 0.5f) > error_y))
    return false;
  *out = direct;
  return true;
#endif
}

/* p + d added count times, rounded after every addition, as a loop that adds
 * a step to a position does it, without the loop. While p and the sums stay
 * in one binade [low, high) (where floats are ulp apart), every step adds the
 * same multiple of ulp to p, which is on that grid: d rounded to it, the same
 * for every p unless d is exactly halfway between two multiples (a tie, which
 * the last bit of the sum decides, step by step). So the steps that stay in a
 * binade are taken at once, and the one that leaves it on its own. (For a
 * negative p everything is mirrored, which rounds the same.) */
static inline float add_steps(float p, float d, int count) {
  const float sign = p < 0.0f ? -1.0f : 1.0f;
  p *= sign;
  d *= sign;
  while (count > 0) {
    /* (also not a number, and a step that is not small against the position, for which the differences
     * below would not be exact) */
    if (!(p >= 1.0f && p < 0x1p100f && fabsf(d) < p * 0.5f))
      break;
    uint32_t bits;
    memcpy(&bits, &p, sizeof(bits));
    const uint32_t exponent = bits & 0x7f800000u, above = exponent + 0x00800000u,
                   spacing = exponent - (23u << 23);
    float low, high, ulp;
    memcpy(&low, &exponent, sizeof(low));
    memcpy(&high, &above, sizeof(high));
    memcpy(&ulp, &spacing, sizeof(ulp));
    const float sum = p + d;
    /* exact: |d| is less than half of p (Fast2Sum) */
    const float delta = sum - p, error = d - delta;
    if (delta == 0.0f)
      return sign * p; /* no step changes p anymore */
    if (!(sum >= low && sum < high) || fabsf(error) == ulp * 0.5f) {
      p = sum;
      count--;
      continue;
    }
    /* Step k (from 0) goes from p + k * delta and adds delta as long as its sum and its result stay in the
     * binade: k * |delta| < room, for the room between p and the edge the steps go to, less the larger of d
     * and delta. n = room / |delta| - 1 steps do: the room is off by half a unit in the last place of p at
     * most (high - p and p - low are exact), which is not more than half of |delta|, and the division adds
     * less than another half. */
    const float reach = d > 0.0f ? (d > delta ? d : delta) : (d < delta ? d : delta);
    const float room = d > 0.0f ? (high - p) - reach : (p - low) + reach;
    const float fit = room / fabsf(delta) - 1.0f;
    if (!(fit >= 1.0f)) {
      p = sum;
      count--;
      continue;
    }
    const int n = fit < (float)count ? (int)fit : count;
    p = (float)((double)p + (double)n * (double)delta); /* (exact: on the grid, in the binade) */
    count -= n;
  }
  for (; count > 0; count--)
    p = p + d;
  return sign * p;
}

/* add_steps() for both coordinates, which a loop that adds a step to a position adds up on their own (DDNet
 * stops when a step changes neither: the steps after it would not change them either). A few steps are
 * quicker one by one. Checked against the loop for 200 million random cases (positions near the edges of
 * binades, steps that are short binary fractions and so have ties) and 290 million moves of a tee: the same
 * bits for all of them. */
static inline vec2 add_steps2(vec2 p, vec2 d, int count) {
  if (count < 20) {
    for (; count > 0; count--)
      p = vadd(p, d);
    return p;
  }
  return v2(add_steps(p.x, d.x, count), add_steps(p.y, d.y, count));
}

/* pos + count * step in place of adding step count times, see above */
static inline bool steps_at_once(vec2 pos, vec2 step, int count, vec2 *out) {
  return rounds_like_steps(pos, vscale(step, (float)count), (float)count, out);
}

/* rounded: the caller rounds the position to whole numbers afterwards and uses nothing else of it */
/* The part of move_box() that does it step by step. */
/* collision_test_box() for a tee whose middle is known to be above 32 and
 * below inner_width and inner_height (see there) */
static inline bool test_box_inside(const collision_t *col, vec2 pos) {
  const uint16_t *flags = col->tile_flags;
  const int x = (int)(pos.x + 0.5f), y = (int)(pos.y + 0.5f);
  const int left = (x - 14) >> 5, right = (x + 14) >> 5;
  const int top = ((y - 14) >> 5) * col->width, bottom = ((y + 14) >> 5) * col->width;
  return (flags[top + left] | flags[top + right] | flags[bottom + left] | flags[bottom + right]) &
         DDNET_TILEFLAG_SOLID;
}

/* The loop of CCollision::MoveBox with its tests, for move_box_steps().
 * inside: the box is a tee that is well inside of the map wherever the loop
 * takes it, so that its tests need not look whether it is. */
static inline __attribute__((always_inline)) void move_box_loop(const collision_t *col, vec2 *inout_pos,
                                                                vec2 *inout_vel, vec2 size, vec2 elasticity,
                                                                bool *grounded, bool rounded, int max,
                                                                float fraction, bool inside, bool simple) {
#define TEST_BOX(p) (inside ? test_box_inside(col, p) : collision_test_box(col, p, size))
  vec2 pos = *inout_pos;
  vec2 vel = *inout_vel;
  float elasticity_x = clampf(elasticity.x, -1.0f, 1.0f);
  float elasticity_y = clampf(elasticity.y, -1.0f, 1.0f);

  /* positions of the box that are known to be free, none so far */
  vec2 free_min = v2(1.0f, 1.0f), free_max = v2(0.0f, 0.0f);
  const float map_max_x = (col->width - 2) * 32.0f, map_max_y = (col->height - 2) * 32.0f;

  /* the step only changes when a collision changes the velocity */
  vec2 step = vscale(vel, fraction);
  /* whether to look if the rest of the way is free, once after every collision */
  bool look_ahead = inside && !simple;
  for (int i = 0; i <= max; i++) {
    /* Early break as optimization to stop checking for collisions for
     * large distances after the obstacles we have already hit reduced
     * our speed to exactly 0. */
    if (veq_fast(vel, v2(0, 0)))
      break;

    vec2 new_pos = vadd(pos, step); /* TODO: this row is not nice */

    /* Fraction can be very small and thus the calculation has no effect, no
     * reason to continue calculating. */
    if (veq_fast(new_pos, pos))
      break;

    /* Still on the tiles that were found to be free: keep going without
     * tests. A step that does not change the position ends the loop in
     * DDNet; going on has the same result, see above. (simple: not for a few
     * steps with a cheap test, where this costs more than it saves) */
    if (!simple && new_pos.x > free_min.x && new_pos.x < free_max.x && new_pos.y > free_min.y &&
        new_pos.y < free_max.y) {
      /* Each coordinate only goes one way, so if the last position is on
       * these tiles, all of them are. */
      vec2 at_once;
      if (rounded && steps_at_once(new_pos, step, max - i, &at_once) && at_once.x > free_min.x + 0.25f &&
          at_once.x < free_max.x - 0.25f && at_once.y > free_min.y + 0.25f &&
          at_once.y < free_max.y - 0.25f) {
        pos = at_once;
        goto done;
      }
      const vec2 last_pos = add_steps2(new_pos, step, max - i);
      if (last_pos.x > free_min.x && last_pos.x < free_max.x && last_pos.y > free_min.y &&
          last_pos.y < free_max.y) {
        pos = last_pos;
        goto done;
      }
      do {
        pos = new_pos;
        if (++i > max)
          goto done;
        new_pos = vadd(pos, step);
      } while (new_pos.x > free_min.x && new_pos.x < free_max.x && new_pos.y > free_min.y &&
               new_pos.y < free_max.y);
      /* DDNet looks at this step, unless it does not move */
      if (veq_fast(new_pos, pos))
        goto done;
    }

    PROF_COUNT(33, "move_box loop: tests");
    if (!TEST_BOX(v2(new_pos.x, new_pos.y))) {
      if (simple)
        goto moved;
      if (look_ahead) {
        /* The steps after this one: each coordinate goes one way (the
         * velocity only changes in a collision), so they are between this
         * position and the last one, which is about new_pos + count * step,
         * off by the roundings of count additions at most (see
         * rounds_like_steps()). If nothing solid is where a corner of the box
         * can get to on that way (rounded to a whole number, which is within
         * half a unit), none of them collides: the rest of the loop just adds
         * the step. (The box is a tee well inside of the map, see there.) */
        look_ahead = false;
        const int count = max - i;
        const vec2 last = v2(new_pos.x + step.x * (float)count, new_pos.y + step.y * (float)count);
        const float largest = maxf(maxf(absolutef(new_pos.x), absolutef(last.x)),
                                   maxf(absolutef(new_pos.y), absolutef(last.y))) +
                              1.0f;
        const float error = ((float)count + 4.0f) * 1.2e-7f * largest + 2e-5f;
        const float out = 14.0f + 0.75f;
        const float min_x = minf(new_pos.x, last.x) - out, max_x = maxf(new_pos.x, last.x) + out;
        const float min_y = minf(new_pos.y, last.y) - out, max_y = maxf(new_pos.y, last.y) + out;
        if (error < 0.2f && min_x > 0.0f && min_y > 0.0f && max_x < col->units_width &&
            max_y < col->units_height) {
          const int x0 = (int)min_x >> 5, x1 = ((int)max_x >> 5) + 1;
          const int y0 = (int)min_y >> 5, y1 = ((int)max_y >> 5) + 1;
          if (x1 - x0 <= 64 && y1 - y0 <= 64 && rect_count(col, col->sat_solid, x0, y0, x1, y1) == 0) {
            PROF_COUNT(32, "move_box: the rest free");
            if (!(rounded && steps_at_once(new_pos, step, count, &pos)))
              pos = add_steps2(new_pos, step, count);
            goto done;
          }
        }
      }
      /* The corners of the box are on up to four tiles, which are free. So
       * are all positions that keep every corner on its tile. The margin is
       * for the rounding of the corner coordinates. */
      free_min = v2(1.0f, 1.0f);
      free_max = v2(0.0f, 0.0f);
      const float half_x = size.x * 0.5f, half_y = size.y * 0.5f;
      if (new_pos.x - half_x > 64.0f && new_pos.y - half_y > 64.0f && new_pos.x + half_x < map_max_x &&
          new_pos.y + half_y < map_max_y && map_max_x < 60000.0f && map_max_y < 60000.0f) {
        const int x0 = round_fast(new_pos.x - half_x) / 32, x1 = round_fast(new_pos.x + half_x) / 32;
        const int y0 = round_fast(new_pos.y - half_y) / 32, y1 = round_fast(new_pos.y + half_y) / 32;
        free_min = v2(x0 * 32.0f - 0.5f + half_x + 0.05f, y0 * 32.0f - 0.5f + half_y + 0.05f);
        free_max = v2(x1 * 32.0f + 31.5f - half_x - 0.05f, y1 * 32.0f + 31.5f - half_y - 0.05f);
      }
    } else {
      PROF_COUNT(34, "move_box loop: collisions");
      free_min = v2(1.0f, 1.0f);
      free_max = v2(0.0f, 0.0f);

      int hits = 0;

      if (TEST_BOX(v2(pos.x, new_pos.y))) {
        if (grounded && elasticity_y > 0 && vel.y > 0)
          *grounded = true;
        new_pos.y = pos.y;
        vel.y *= -elasticity_y;
        hits++;
      }

      if (TEST_BOX(v2(new_pos.x, pos.y))) {
        new_pos.x = pos.x;
        vel.x *= -elasticity_x;
        hits++;
      }

      /* neither of the tests got a collision.
       * this is a real _corner case_! */
      if (hits == 0) {
        if (grounded && elasticity_y > 0 && vel.y > 0)
          *grounded = true;
        new_pos.y = pos.y;
        vel.y *= -elasticity_y;
        new_pos.x = pos.x;
        vel.x *= -elasticity_x;
      }
      step = vscale(vel, fraction);
      look_ahead = inside && !simple;
    }

  moved:
    pos = new_pos;
  }
done:
  *inout_pos = pos;
  *inout_vel = vel;
#undef TEST_BOX
}

/* Whether no step of a move of a tee from pos by vel in steps steps (move_box()) gets its box onto a solid
 * tile: then the move adds up the steps and nothing else. Each step is within error of the line from pos to
 * pos + vel (see rounds_like_steps()), and a corner of the box is rounded to a whole number, which is half a
 * unit away at most; so a box half a tile and a bit larger, swept along the line, covers every tile a corner
 * gets on. That is looked at column by column of tiles: the part of the line whose box reaches into a
 * column, and the rows that part covers. False if it cannot tell (far, near the border of the map, or a
 * rounding error that is too large). */
static bool move_sweep_free(const collision_t *col, vec2 pos, vec2 vel, int steps) {
  const float largest = fabsf(pos.x) + fabsf(pos.y) + fabsf(vel.x) + fabsf(vel.y) + 1.0f;
  const float error = ((float)steps + 4.0f) * 1.2e-7f * largest + 2e-5f;
  /* 14 for the box, 0.5 for the rounding of a corner, the error, and some more for the roundings below */
  const float out = 14.0f + 0.5f + error + 0.25f;
  const vec2 end = v2(pos.x + vel.x, pos.y + vel.y);
  const float min_x = minf(pos.x, end.x) - out, max_x = maxf(pos.x, end.x) + out;
  const float min_y = minf(pos.y, end.y) - out, max_y = maxf(pos.y, end.y) + out;
  if (!(error < 0.2f && min_x >= 32.0f && min_y >= 32.0f && max_x < col->inner_width &&
        max_y < col->inner_height))
    return false;
  const int x0 = (int)min_x >> 5, x1 = (int)max_x >> 5;
  const int y0 = (int)min_y >> 5, y1 = (int)max_y >> 5;
  /* (a move that ends in something solid collides, which is most that come here: not worth the rest) */
  if (x1 - x0 > 16 || y1 - y0 > 64 || test_box_inside(col, end))
    return false;
  /* a line that hardly moves sideways: all of it in every column */
  if (!(fabsf(vel.x) > 1.0f))
    return rect_count(col, col->sat_solid, x0, y0, x1 + 1, y1 + 1) == 0;
  const float per_x = 1.0f / vel.x;
  for (int x = x0; x <= x1; x++) {
    /* the part of the line (0 to 1) whose box reaches into column x */
    float a = ((float)x * 32.0f - out - pos.x) * per_x, b = ((float)x * 32.0f + 32.0f + out - pos.x) * per_x;
    float from = maxf(minf(a, b), 0.0f), to = minf(maxf(a, b), 1.0f);
    if (!(from <= to))
      from = 0.0f, to = 1.0f;
    const float ya = pos.y + vel.y * from, yb = pos.y + vel.y * to;
    const int top = clampi((int)(minf(ya, yb) - out) >> 5, y0, y1);
    const int bottom = clampi((int)(maxf(ya, yb) + out) >> 5, y0, y1);
    if (rect_count(col, col->sat_solid, x, top, x + 1, bottom + 1))
      return false;
  }
  return true;
}

/* Whether every one of count steps of d from p (p > 0, added up with a rounding after each, see
 * add_steps()) is exactly p + k * *delta: the steps stay in the binade of p and are not ties. Then the k-th
 * position is known without adding up, as (float)(p + k * (double)*delta). */
static inline bool steps_linear(float p, float d, int count, float *delta) {
  if (!(p >= 1.0f && p < 0x1p100f && fabsf(d) < p * 0.5f))
    return false;
  uint32_t bits;
  memcpy(&bits, &p, sizeof(bits));
  const uint32_t exponent = bits & 0x7f800000u, above = exponent + 0x00800000u,
                 spacing = exponent - (23u << 23);
  float low, high, ulp;
  memcpy(&low, &exponent, sizeof(low));
  memcpy(&high, &above, sizeof(high));
  memcpy(&ulp, &spacing, sizeof(ulp));
  const float sum = p + d;
  *delta = sum - p; /* (exact) */
  if (*delta == 0.0f)
    return true;
  if (!(sum >= low && sum < high) || fabsf(d - *delta) == ulp * 0.5f)
    return false;
  const float reach = d > 0.0f ? (d > *delta ? d : *delta) : (d < *delta ? d : *delta);
  const float room = d > 0.0f ? (high - p) - reach : (p - low) + reach;
  return (float)count * fabsf(*delta) < room; /* (see add_steps()) */
}

static inline float step_at(float p, float delta, int k) {
#ifdef __FMA__
  /* (exact, so the one rounding of the fused operation changes nothing) */
  return __builtin_fmaf((float)k, delta, p);
#else
  return (float)((double)p + (double)k * (double)delta);
#endif
}

/* The first k from 1 on for which the rounded position after k steps (p + k * delta, as steps_linear()
 * says), (int)(position + 0.5f) like a box test rounds it, is at edge or more (increasing) or below edge;
 * count + 1 if none up to count is. The division gives it, and the positions around it confirm it. -1 if they
 * do not. */
static inline int step_onto_edge(float p, float delta, int count, int edge, bool increasing) {
  if (delta == 0.0f || (increasing != (delta > 0.0f)))
    return count + 1;
  const double at = ((double)edge - 0.5 - (double)p) / (double)delta;
  int k = increasing ? (int)ceil(at) : (int)floor(at) + 1;
  if (k < 1)
    k = 1;
  if (k > count)
    k = count + 1;
#define PAST(k)                                                                                              \
  (increasing ? (int)(step_at(p, delta, k) + 0.5f) >= edge : (int)(step_at(p, delta, k) + 0.5f) < edge)
  if ((k <= count && !PAST(k)) || (k > 1 && PAST(k - 1)))
    return -1;
#undef PAST
  return k;
}

/* move_box_loop() for a tee well inside of the map that collides once, with a floor, a ceiling or a wall
 * that does not bounce, and with nothing else: standing, walking, landing, pushing against a wall. The
 * positions of the steps are known exactly (steps_linear()). For a floor: nothing is solid on the rows the
 * box is on now, from where its corners are at the first position sideways to where they are at the last.
 * The steps go down, so the box stays on these rows until the step whose rounded y takes its bottom onto the
 * next row (step_onto_edge()); up to that step nothing collides. That step does what DDNet does if it is what
 * it usually is: the box collides there, and above the previous position too (DDNet tests that first), which
 * keeps the y of the previous step and sets the vertical velocity to 0; the box on its old rows at the new x
 * (tested second) is free. The rest of the steps only go sideways, on those rows. A ceiling is the same
 * upwards, and a wall the same sideways, except that DDNet's first test, above or below the previous
 * position, is free there, and the second one collides. False, with nothing changed, if it is not like that.
 */
static bool move_slide_y(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, float elasticity_y,
                         int max, float fraction) {
  const vec2 pos = *inout_pos, vel = *inout_vel;
  if (!(vel.y != 0.0f) || elasticity_y != 0.0f)
    return false;
  const vec2 step = vscale(vel, fraction);
  const int steps = max + 1;
  float dx, dy;
  if (!steps_linear(pos.x, step.x, steps, &dx) || !steps_linear(pos.y, step.y, steps, &dy))
    return false;
  const float end_x = step_at(pos.x, dx, steps);
  const int y = (int)(pos.y + 0.5f);
  const int top = (y - 14) >> 5, bottom = (y + 14) >> 5;
  const int left = ((int)(minf(pos.x, end_x) + 0.5f) - 14) >> 5,
            right = ((int)(maxf(pos.x, end_x) + 0.5f) + 14) >> 5;
  if (rect_count(col, col->sat_solid, left, top, right + 1, bottom + 1))
    return false;
  const bool down = vel.y > 0.0f;
  const int k = step_onto_edge(pos.y, dy, steps, down ? (bottom + 1) * 32 - 14 : top * 32 + 14, down);
  if (k < 0)
    return false;
  if (k > steps) {
    *inout_pos = v2(end_x, step_at(pos.y, dy, steps));
    return true;
  }
  const float new_y = step_at(pos.y, dy, k);
  if (!test_box_inside(col, v2(step_at(pos.x, dx, k), new_y)) ||
      !test_box_inside(col, v2(step_at(pos.x, dx, k - 1), new_y)))
    return false;
  *inout_vel = v2(vel.x, vel.y * -elasticity_y);
  *inout_pos = v2(end_x, step_at(pos.y, dy, k - 1));
  return true;
}

static bool move_slide_x(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, float elasticity_x,
                         int max, float fraction) {
  const vec2 pos = *inout_pos, vel = *inout_vel;
  if (!(vel.x != 0.0f) || elasticity_x != 0.0f)
    return false;
  const vec2 step = vscale(vel, fraction);
  const int steps = max + 1;
  float dx, dy;
  if (!steps_linear(pos.x, step.x, steps, &dx) || !steps_linear(pos.y, step.y, steps, &dy))
    return false;
  const float end_y = step_at(pos.y, dy, steps);
  const int x = (int)(pos.x + 0.5f);
  const int left = (x - 14) >> 5, right = (x + 14) >> 5;
  const int top = ((int)(minf(pos.y, end_y) + 0.5f) - 14) >> 5,
            bottom = ((int)(maxf(pos.y, end_y) + 0.5f) + 14) >> 5;
  if (rect_count(col, col->sat_solid, left, top, right + 1, bottom + 1))
    return false;
  const bool forward = vel.x > 0.0f;
  const int k = step_onto_edge(pos.x, dx, steps, forward ? (right + 1) * 32 - 14 : left * 32 + 14, forward);
  if (k < 0)
    return false;
  if (k > steps) {
    *inout_pos = v2(step_at(pos.x, dx, steps), end_y);
    return true;
  }
  /* DDNet's first test (the previous x, the new y) is on these columns, free */
  const float new_x = step_at(pos.x, dx, k);
  if (!test_box_inside(col, v2(new_x, step_at(pos.y, dy, k))) ||
      !test_box_inside(col, v2(new_x, step_at(pos.y, dy, k - 1))))
    return false;
  *inout_vel = v2(vel.x * -elasticity_x, vel.y);
  *inout_pos = v2(step_at(pos.x, dx, k - 1), end_y);
  return true;
}

/* move_box_loop() for a tee well inside of the map whose position is rounded to whole numbers right after
 * the move, with nothing else looking at it (rounded): what the move comes to after the rounding, for a move
 * that collides with nothing, or once, with a floor, a ceiling or a wall that does not bounce. The positions
 * of the steps are known exactly (steps_linear(), the last one end) and only go one way, so the columns and
 * rows the corners of the box get to are those between the ones at the start and at the end.
 *
 * - Nothing solid on those: nothing collides, and the move ends at end.
 * - The box gets onto another row (down onto the next one, or up), nothing solid on its rows now where it
 *   gets to sideways, and the row it gets onto all solid there: the step that gets it onto that row
 *   collides there and above the previous position, wherever that is sideways (DDNet tests that first),
 *   and not on the old rows at the new position (tested second). That keeps the y of the previous step and
 *   sets the vertical velocity to -0 (+0 upwards). A step is shorter than a unit, so that y is less than
 *   a unit and a half before the edge of the row and rounds to the last whole number before it, whichever
 *   step it was. The rest of the steps only go sideways, on the old rows: the move ends at end.x.
 * - A wall the same sideways, where DDNet's first test, the previous x at the new y, is free (on the old
 *   columns, where nothing is solid on the rows the box gets to), and the second one collides.
 *
 * False, with nothing changed, for anything else. The position it comes to is not DDNet's before the
 * rounding, only after; the velocity is DDNet's. */
static bool move_rounded_once(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, vec2 elasticity,
                              int max, float fraction) {
  const vec2 pos = *inout_pos, vel = *inout_vel;
  const vec2 step = vscale(vel, fraction);
  const int steps = max + 1;
  float dx, dy;
  if (!steps_linear(pos.x, step.x, steps, &dx) || !steps_linear(pos.y, step.y, steps, &dy))
    return false;
  const vec2 end = v2(step_at(pos.x, dx, steps), step_at(pos.y, dy, steps));
  const int x = (int)(pos.x + 0.5f), y = (int)(pos.y + 0.5f), ex = (int)(end.x + 0.5f),
            ey = (int)(end.y + 0.5f);
  const int left = (x - 14) >> 5, right = (x + 14) >> 5, top = (y - 14) >> 5, bottom = (y + 14) >> 5;
  const int way_left = (mini(x, ex) - 14) >> 5, way_right = (maxi(x, ex) + 14) >> 5;
  const int way_top = (mini(y, ey) - 14) >> 5, way_bottom = (maxi(y, ey) + 14) >> 5;
  if (way_right - way_left > 64 || way_bottom - way_top > 64)
    return false;
  if (rect_count(col, col->sat_solid, way_left, way_top, way_right + 1, way_bottom + 1) == 0) {
    *inout_pos = end;
    return true;
  }
  if ((way_top < top || way_bottom > bottom) && clampf(elasticity.y, -1.0f, 1.0f) == 0.0f) {
    const bool down = vel.y > 0.0f;
    const int row = down ? bottom + 1 : top - 1;
    if (rect_count(col, col->sat_solid, way_left, top, way_right + 1, bottom + 1) == 0 &&
        rect_count(col, col->sat_solid, way_left, row, way_right + 1, row + 1) == way_right - way_left + 1) {
      *inout_vel = v2(vel.x, vel.y * -clampf(elasticity.y, -1.0f, 1.0f));
      *inout_pos = v2(end.x, down ? (float)((bottom + 1) * 32 - 15) : (float)(top * 32 + 14));
      return true;
    }
  }
  if ((way_left < left || way_right > right) && clampf(elasticity.x, -1.0f, 1.0f) == 0.0f) {
    const bool forward = vel.x > 0.0f;
    const int column = forward ? right + 1 : left - 1;
    if (rect_count(col, col->sat_solid, left, way_top, right + 1, way_bottom + 1) == 0 &&
        rect_count(col, col->sat_solid, column, way_top, column + 1, way_bottom + 1) ==
            way_bottom - way_top + 1) {
      *inout_vel = v2(vel.x * -clampf(elasticity.x, -1.0f, 1.0f), vel.y);
      *inout_pos = v2(forward ? (float)((right + 1) * 32 - 15) : (float)(left * 32 + 14), end.y);
      return true;
    }
  }
  return false;
}

DDNET_NOINLINE static void move_box_steps(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, vec2 size,
                                          vec2 elasticity, bool *grounded, bool rounded) {
  vec2 pos = *inout_pos;
  vec2 vel = *inout_vel;

  float distance = vlength(vel);
  int max = (int)distance;
  PROF_COUNT(2, "move_box");
  for (int k = 0; k <= max; k++)
    PROF_COUNT(3, "move_box iterations (max+1)");

  if (distance > 0.00001f) {
    /* max is negative for a distance that does not fit into an int */
    float fraction = (unsigned)max < 256 ? col->move_fraction[max] : 1.0f / (float)(max + 1);

    /* No position of the loop is further from where it starts than the
     * length of the velocity in either direction (a step is never longer than
     * the one before, there are max + 1 of them, and a collision only takes a
     * step back). With all of them well inside of the map, the tests of a tee
     * need not look at that each time. */
    const bool inside = size.x == 28.0f && size.y == 28.0f && pos.x - distance > 34.0f &&
                        pos.y - distance > 34.0f && pos.x + distance < col->inner_width - 2.0f &&
                        pos.y + distance < col->inner_height - 2.0f;
    if (rounded && inside && move_rounded_once(col, inout_pos, inout_vel, elasticity, max, fraction)) {
      PROF_COUNT(36, "move_box: rounded at once");
      return;
    }

    /* Nothing solid within reach of the box on its whole way: the loop below
     * without its tests. The position still has to be added up step by step
     * to get the same rounding. */
    const float reach = distance + maxf(size.x, size.y) * 0.5f + 2.0f;
    if (reach < 255.0f * 32.0f) {
      const int tile = clampi((int)pos.y >> 5, 0, col->height - 1) * col->width +
                       clampi((int)pos.x >> 5, 0, col->width - 1);
      if (col->dist_solid[tile] > (int)(reach / 32.0f) + 1 ||
          (max >= 15 && size.x == 28.0f && size.y == 28.0f && move_sweep_free(col, pos, vel, max + 1))) {
        PROF_COUNT(4, "move_box all free");
        /* DDNet stops when a step does not change the position anymore. The
         * steps after that would not change it either. */
        const vec2 step = vscale(vel, fraction);
        if (!(rounded && steps_at_once(pos, step, max + 1, &pos))) {
          PROF_COUNT(28, "move_box all free: step by step");
          pos = add_steps2(pos, step, max + 1);
        }
        *inout_pos = pos;
        return;
      }
    }

    /* (a short move adds up its steps quicker than these work out where it collides) */
    if (inside && max >= 12 &&
        (move_slide_y(col, inout_pos, inout_vel, clampf(elasticity.y, -1.0f, 1.0f), max, fraction) ||
         move_slide_x(col, inout_pos, inout_vel, clampf(elasticity.x, -1.0f, 1.0f), max, fraction))) {
      PROF_COUNT(35, "move_box: one collision at once");
      return;
    }
    if (inside && max <= 32)
      move_box_loop(col, inout_pos, inout_vel, size, elasticity, grounded, rounded, max, fraction, true,
                    true);
    else if (inside)
      move_box_loop(col, inout_pos, inout_vel, size, elasticity, grounded, rounded, max, fraction, true,
                    false);
    else
      move_box_loop(col, inout_pos, inout_vel, size, elasticity, grounded, rounded, max, fraction, false,
                    false);
    return;
  }

  *inout_pos = pos;
  *inout_vel = vel;
}

DDNET_HOT static void move_box(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, vec2 size,
                               vec2 elasticity, bool *grounded, bool rounded) {
  /* do the move */
  vec2 pos = *inout_pos;
  vec2 vel = *inout_vel;

  if (rounded) {
    /* The common case without the length of the way, which takes a root:
     * the sum rounding like the steps do, and nothing solid on any tile a
     * corner of the box gets on. The positions on the way are between the
     * two ends, and a corner is rounded to a whole number: it is not further
     * out than this. The way has not more steps than travel + 2. */
    const float travel = fabsf(vel.x) + fabsf(vel.y);
    vec2 end;
    if (travel < 4096.0f && rounds_like_steps(pos, vel, travel + 2.0f, &end)) {
      const float out_x = size.x * 0.5f + 0.75f, out_y = size.y * 0.5f + 0.75f;
      const float min_x = minf(pos.x, end.x) - out_x, max_x = maxf(pos.x, end.x) + out_x;
      const float min_y = minf(pos.y, end.y) - out_y, max_y = maxf(pos.y, end.y) + out_y;
      /* on the map, where a tile is its coordinates divided by 32 */
      if (min_x >= 32.0f && min_y >= 32.0f && max_x < (col->width - 1) * 32.0f &&
          max_y < (col->height - 1) * 32.0f) {
        const int x0 = (int)min_x >> 5, x1 = ((int)max_x >> 5) + 1;
        const int y0 = (int)min_y >> 5, y1 = ((int)max_y >> 5) + 1;
        if (rect_count(col, col->sat_solid, x0, y0, x1, y1) == 0) {
          PROF_COUNT(29, "move_box: sum at once");
          *inout_pos = end;
          return;
        }
      }
    }
  }
  move_box_steps(col, inout_pos, inout_vel, size, elasticity, grounded, rounded);
}

DDNET_HOT void collision_move_box(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, vec2 size,
                                  vec2 elasticity, bool *grounded) {
  move_box(col, inout_pos, inout_vel, size, elasticity, grounded, false);
}

DDNET_HOT void collision_move_box_rounded(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, vec2 size,
                                          vec2 elasticity, bool *grounded) {
#ifdef DDNET_PHYSICS_NO_ROUNDED_MOVE /* to measure what it is worth */
  move_box(col, inout_pos, inout_vel, size, elasticity, grounded, false);
  return;
#endif
  move_box(col, inout_pos, inout_vel, size, elasticity, grounded, true);
}

/* ------------------------------------------------------------ tile lookup */

static bool tile_exists_next(const collision_t *col, int index) {
  if (index < 0)
    return false;
  const ddnet_tile_t *game = col->game;
  const ddnet_tile_t *front = col->front;
  const ddnet_door_tile_t *door = col->door;
  const int count = col->width * col->height;
  int left = (index - 1 > 0) ? index - 1 : index;
  int right = (index + 1 < count) ? index + 1 : index;
  int below = (index + col->width < count) ? index + col->width : index;
  int above = (index - col->width > 0) ? index - col->width : index;

  if ((game[right].index == TILE_STOP && game[right].flags == ROTATION_270) ||
      (game[left].index == TILE_STOP && game[left].flags == ROTATION_90))
    return true;
  if ((game[below].index == TILE_STOP && game[below].flags == ROTATION_0) ||
      (game[above].index == TILE_STOP && game[above].flags == ROTATION_180))
    return true;
  if (game[right].index == TILE_STOPA || game[left].index == TILE_STOPA ||
      ((game[right].index == TILE_STOPS || game[left].index == TILE_STOPS)))
    return true;
  /* The flag terms of these checks are always true, they are kept as written upstream. */
  if (game[below].index == TILE_STOPA || game[above].index == TILE_STOPA ||
      ((game[below].index == TILE_STOPS || game[above].index == TILE_STOPS) &&
       (game[below].flags | ROTATION_180 | ROTATION_0)))
    return true;
  if (front) {
    if (front[right].index == TILE_STOPA || front[left].index == TILE_STOPA ||
        ((front[right].index == TILE_STOPS || front[left].index == TILE_STOPS)))
      return true;
    if (front[below].index == TILE_STOPA || front[above].index == TILE_STOPA ||
        ((front[below].index == TILE_STOPS || front[above].index == TILE_STOPS) &&
         (front[below].flags | ROTATION_180 | ROTATION_0)))
      return true;
    if ((front[right].index == TILE_STOP && front[right].flags == ROTATION_270) ||
        (front[left].index == TILE_STOP && front[left].flags == ROTATION_90))
      return true;
    if ((front[below].index == TILE_STOP && front[below].flags == ROTATION_0) ||
        (front[above].index == TILE_STOP && front[above].flags == ROTATION_180))
      return true;
  }
  if (door) {
    if (door[right].index == TILE_STOPA || door[left].index == TILE_STOPA ||
        ((door[right].index == TILE_STOPS || door[left].index == TILE_STOPS)))
      return true;
    if (door[below].index == TILE_STOPA || door[above].index == TILE_STOPA ||
        ((door[below].index == TILE_STOPS || door[above].index == TILE_STOPS) &&
         (door[below].flags | ROTATION_180 | ROTATION_0)))
      return true;
    if ((door[right].index == TILE_STOP && door[right].flags == ROTATION_270) ||
        (door[left].index == TILE_STOP && door[left].flags == ROTATION_90))
      return true;
    if ((door[below].index == TILE_STOP && door[below].flags == ROTATION_0) ||
        (door[above].index == TILE_STOP && door[above].flags == ROTATION_180))
      return true;
  }
  return false;
}

static bool tile_exists_slow(const collision_t *col, int index) {
  if (index < 0)
    return false;

  const ddnet_tile_t *game = col->game;
  const ddnet_tile_t *front = col->front;
  const ddnet_tele_tile_t *tele = col->tele;
  if ((game[index].index >= TILE_FREEZE && game[index].index <= TILE_TELE_LASER_DISABLE) ||
      (game[index].index >= TILE_LFREEZE && game[index].index <= TILE_LUNFREEZE))
    return true;
  if (front && ((front[index].index >= TILE_FREEZE && front[index].index <= TILE_TELE_LASER_DISABLE) ||
                (front[index].index >= TILE_LFREEZE && front[index].index <= TILE_LUNFREEZE)))
    return true;
  if (tele && (tele[index].type == TILE_TELEIN || tele[index].type == TILE_TELEINEVIL ||
               tele[index].type == TILE_TELECHECKINEVIL || tele[index].type == TILE_TELECHECK ||
               tele[index].type == TILE_TELECHECKIN))
    return true;
  if (col->speedup && col->speedup[index].force > 0)
    return true;
  if (col->door && col->door[index].index)
    return true;
  if (col->switch_tiles && col->switch_tiles[index].type)
    return true;
  if (col->tune && col->tune[index].type)
    return true;
  return tile_exists_next(col, index);
}

DDNET_HOT bool collision_tile_exists(const collision_t *col, int index) {
  if (index < 0)
    return false;
  return col->tile_flags[index] & DDNET_TILEFLAG_EXISTS;
}

int collision_get_map_index(const collision_t *col, vec2 pos) {
  int nx = clampi((int)pos.x >> 5, 0, col->width - 1);
  int ny = clampi((int)pos.y >> 5, 0, col->height - 1);
  int index = ny * col->width + nx;

  if (collision_tile_exists(col, index))
    return index;
  else
    return -1;
}

map_indices_t collision_map_indices(const collision_t *col, vec2 prev_pos, vec2 pos) {
  map_indices_t it;
  it.col = col;
  it.prev_pos = prev_pos;
  it.pos = pos;
  it.d = vdistance(prev_pos, pos);
  it.end = (int)(it.d + 1);
  it.i = 0;
  it.last_index = 0;
  it.sample = -1;
  it.sample_tile = 0;
  return it;
}

/* The tile of sample i of GetMapIndices(): one sample per unit, truncated. (Remembered: the loop looks at
 * the first sample on the next tile next, which the search for that tile has looked at already.) */
static inline int map_indices_tile(map_indices_t *it, int i) {
  if (it->sample == i)
    return it->sample_tile;
  const collision_t *col = it->col;
  float a = i / it->d;
  vec2 tmp = vmix(it->prev_pos, it->pos, a);
  int nx = clampi((int)tmp.x >> 5, 0, col->width - 1);
  int ny = clampi((int)tmp.y >> 5, 0, col->height - 1);
  it->sample = i;
  it->sample_tile = ny * col->width + nx;
  return it->sample_tile;
}

/* The first sample after sample i that is not on the same tile, at most
 * it->end. The samples on one tile are consecutive, see the lines above. */
static inline int map_indices_leave_tile(map_indices_t *it, int i, int tile) {
  const collision_t *col = it->col;
  /* where the way would cross the border of the tile, in units from the start */
  float guess = (float)it->end;
  const float dx = it->pos.x - it->prev_pos.x, dy = it->pos.y - it->prev_pos.y;
  if (dx != 0) {
    const int tx = tile % col->width;
    const float border = dx > 0 ? (tx + 1) * 32.0f : tx * 32.0f;
    guess = (border - it->prev_pos.x) * it->d / dx;
  }
  if (dy != 0) {
    const int ty = tile / col->width;
    const float border = dy > 0 ? (ty + 1) * 32.0f : ty * 32.0f;
    const float guess_y = (border - it->prev_pos.y) * it->d / dy;
    if (guess_y < guess)
      guess = guess_y;
  }
  int next = i + 1;
  if (guess > (float)next)
    next = guess < (float)it->end ? (int)guess : it->end;

  while (next - 1 > i && map_indices_tile(it, next - 1) != tile)
    next--;
  while (next < it->end && map_indices_tile(it, next) == tile)
    next++;
  return next;
}

bool collision_map_indices_next(map_indices_t *it, int *index) {
  const collision_t *col = it->col;
  if (!it->d) {
    /* Not moving: only the tile the tee is on, once. */
    if (it->i != 0)
      return false;
    it->i = 1;
    int nx = clampi((int)it->pos.x >> 5, 0, col->width - 1);
    int ny = clampi((int)it->pos.y >> 5, 0, col->height - 1);
    *index = ny * col->width + nx;
    return collision_tile_exists(col, *index);
  }

  while (it->i < it->end) {
    const int tile = map_indices_tile(it, it->i);
    if (!(col->tile_flags[tile] & DDNET_TILEFLAG_EXISTS)) {
      /* samples are one unit apart: skip the ones that cannot be on a tile that exists */
      const int dist = col->dist_exists[tile];
      it->i = dist >= 2 ? it->i + 32 * (dist - 1) : map_indices_leave_tile(it, it->i, tile);
      continue;
    }
    /* a tile is only reported once, and the way does not come back to it */
    it->i = map_indices_leave_tile(it, it->i, tile);
    if (it->last_index != tile) {
      it->last_index = tile;
      *index = tile;
      return true;
    }
  }
  return false;
}

int collision_get_tile_index(const collision_t *col, int index) {
  if (index < 0)
    return 0;
  return col->game[index].index;
}

int collision_get_front_tile_index(const collision_t *col, int index) {
  if (index < 0 || !col->front)
    return 0;
  return col->front[index].index;
}

int collision_get_tile_flags(const collision_t *col, int index) {
  if (index < 0)
    return 0;
  return col->game[index].flags;
}

int collision_get_front_tile_flags(const collision_t *col, int index) {
  if (index < 0 || !col->front)
    return 0;
  return col->front[index].flags;
}

int collision_get_index(const collision_t *col, vec2 prev_pos, vec2 pos) {
  float distance = vdistance(prev_pos, pos);

  if (!distance) {
    int nx = clampi((int)pos.x >> 5, 0, col->width - 1);
    int ny = clampi((int)pos.y >> 5, 0, col->height - 1);

    if ((col->tele) || (col->speedup && col->speedup[ny * col->width + nx].force > 0))
      return ny * col->width + nx;
  }

  const int distance_rounded = (int)ceilf(distance);
  for (int i = 0; i < distance_rounded; i++) {
    float a = (float)i / distance;
    vec2 tmp = vmix(prev_pos, pos, a);
    int nx = clampi((int)tmp.x >> 5, 0, col->width - 1);
    int ny = clampi((int)tmp.y >> 5, 0, col->height - 1);
    if ((col->tele) || (col->speedup && col->speedup[ny * col->width + nx].force > 0))
      return ny * col->width + nx;
  }

  return -1;
}

int collision_entity(const collision_t *col, int x, int y, int layer) {
  if (x < 0 || x >= col->width || y < 0 || y >= col->height)
    return 0;

  const int index = y * col->width + x;
  switch (layer) {
  case LAYER_GAME:
    return col->game[index].index - ENTITY_OFFSET;
  case LAYER_FRONT:
    return col->front[index].index - ENTITY_OFFSET;
  case LAYER_SWITCH:
    return col->switch_tiles[index].type - ENTITY_OFFSET;
  case LAYER_TELE:
    return col->tele[index].type - ENTITY_OFFSET;
  case LAYER_SPEEDUP:
    return col->speedup[index].type - ENTITY_OFFSET;
  case LAYER_TUNE:
    return col->tune[index].type - ENTITY_OFFSET;
  default:
    assert(!"invalid layer");
    return 0;
  }
}

/* ---------------------------------------------------------- special tiles */

int collision_is_teleport(const collision_t *col, int index) {
  if (index < 0 || !col->tele)
    return 0;
  if (col->tele[index].type == TILE_TELEIN)
    return col->tele[index].number;
  return 0;
}

int collision_is_evil_teleport(const collision_t *col, int index) {
  if (index < 0)
    return 0;
  if (!col->tele)
    return 0;
  if (col->tele[index].type == TILE_TELEINEVIL)
    return col->tele[index].number;
  return 0;
}

bool collision_is_check_teleport(const collision_t *col, int index) {
  if (index < 0 || !col->tele)
    return false;
  return col->tele[index].type == TILE_TELECHECKIN;
}

bool collision_is_check_evil_teleport(const collision_t *col, int index) {
  if (index < 0 || !col->tele)
    return false;
  return col->tele[index].type == TILE_TELECHECKINEVIL;
}

int collision_is_tele_checkpoint(const collision_t *col, int index) {
  if (index < 0)
    return 0;
  if (!col->tele)
    return 0;
  if (col->tele[index].type == TILE_TELECHECK)
    return col->tele[index].number;
  return 0;
}

int collision_is_teleport_weapon(const collision_t *col, int index) {
  if (index < 0 || !col->tele)
    return 0;
  if (col->tele[index].type == TILE_TELEINWEAPON)
    return col->tele[index].number;
  return 0;
}

int collision_is_teleport_hook(const collision_t *col, int index) {
  if (index < 0 || !col->tele)
    return 0;
  if (col->tele[index].type == TILE_TELEINHOOK)
    return col->tele[index].number;
  return 0;
}

bool collision_is_speedup(const collision_t *col, int index) {
  assert(index >= 0);
  return col->speedup && col->speedup[index].force > 0;
}

int collision_is_tune(const collision_t *col, int index) {
  if (index < 0 || !col->tune)
    return 0;
  if (col->tune[index].type)
    return col->tune[index].number;
  return 0;
}

void collision_get_speedup(const collision_t *col, int index, vec2 *dir, int *force, int *max_speed,
                           int *type) {
  if (index < 0 || !col->speedup)
    return;
  float angle = col->speedup[index].angle * (PI / 180.0f);
  *force = col->speedup[index].force;
  *type = col->speedup[index].type;
  *dir = vdirection(angle);
  if (max_speed)
    *max_speed = col->speedup[index].max_speed;
}

int collision_get_switch_type(const collision_t *col, int index) {
  if (index < 0 || !col->switch_tiles)
    return 0;
  if (col->switch_tiles[index].type > 0)
    return col->switch_tiles[index].type;
  return 0;
}

int collision_get_switch_number(const collision_t *col, int index) {
  if (index < 0 || !col->switch_tiles)
    return 0;
  if (col->switch_tiles[index].type > 0 && col->switch_tiles[index].number > 0 &&
      col->switch_tiles[index].number <= col->highest_switch_number)
    return col->switch_tiles[index].number;
  return 0;
}

int collision_get_switch_delay(const collision_t *col, int index) {
  if (index < 0 || !col->switch_tiles)
    return 0;
  if (col->switch_tiles[index].type > 0)
    return col->switch_tiles[index].delay;
  return 0;
}

int collision_is_time_checkpoint(const collision_t *col, int index) {
  if (index < 0)
    return -1;
  int z = col->game[index].index;
  if (z >= TILE_TIME_CHECKPOINT_FIRST && z <= TILE_TIME_CHECKPOINT_LAST)
    return z - TILE_TIME_CHECKPOINT_FIRST;
  return -1;
}

int collision_is_front_time_checkpoint(const collision_t *col, int index) {
  if (index < 0 || !col->front)
    return -1;
  int z = col->front[index].index;
  if (z >= TILE_TIME_CHECKPOINT_FIRST && z <= TILE_TIME_CHECKPOINT_LAST)
    return z - TILE_TIME_CHECKPOINT_FIRST;
  return -1;
}

/* Conveyor ("mover") tiles push the map entities that stand on them. */
int collision_mover_speed(const collision_t *col, int x, int y, vec2 *speed) {
  int nx = clampi(x >> 5, 0, col->width - 1);
  int ny = clampi(y >> 5, 0, col->height - 1);
  int index = col->game[ny * col->width + nx].index;

  if (index != TILE_CP && index != TILE_CP_F)
    return 0;

  vec2 target;
  switch (col->game[ny * col->width + nx].flags) {
  case ROTATION_0:
    target.x = 0.0f;
    target.y = -4.0f;
    break;
  case ROTATION_90:
    target.x = 4.0f;
    target.y = 0.0f;
    break;
  case ROTATION_180:
    target.x = 0.0f;
    target.y = 4.0f;
    break;
  case ROTATION_270:
    target.x = -4.0f;
    target.y = 0.0f;
    break;
  default:
    target = v2(0.0f, 0.0f);
    break;
  }
  if (index == TILE_CP_F)
    target = vscale(target, 4.0f);
  *speed = target;
  return index;
}

/* ------------------------------------------------------------- public API */

int ddnet_collision_get_tile(const ddnet_collision_t *col, int x, int y) {
  return collision_get_tile(col, x, y);
}

int ddnet_collision_get_front_tile(const ddnet_collision_t *col, int x, int y) {
  return collision_get_front_tile(col, x, y);
}

bool ddnet_collision_check_point(const ddnet_collision_t *col, float x, float y) {
  return collision_check_point(col, x, y);
}

bool ddnet_collision_test_box(const ddnet_collision_t *col, ddnet_vec2_t pos, ddnet_vec2_t size) {
  return collision_test_box(col, pos, size);
}

bool ddnet_collision_is_on_ground(const ddnet_collision_t *col, ddnet_vec2_t pos, float size) {
  return collision_is_on_ground(col, pos, size);
}

int ddnet_collision_intersect_line(const ddnet_collision_t *col, ddnet_vec2_t pos0, ddnet_vec2_t pos1,
                                   ddnet_vec2_t *out_collision, ddnet_vec2_t *out_before_collision) {
  return collision_intersect_line(col, pos0, pos1, out_collision, out_before_collision);
}

void ddnet_collision_move_point(const ddnet_collision_t *col, ddnet_vec2_t *inout_pos,
                                ddnet_vec2_t *inout_vel, float elasticity, int *bounces) {
  collision_move_point(col, inout_pos, inout_vel, elasticity, bounces);
}

void ddnet_collision_move_box(const ddnet_collision_t *col, ddnet_vec2_t *inout_pos, ddnet_vec2_t *inout_vel,
                              ddnet_vec2_t size, ddnet_vec2_t elasticity, bool *grounded) {
  collision_move_box(col, inout_pos, inout_vel, size, elasticity, grounded);
}

int ddnet_collision_get_pure_map_index(const ddnet_collision_t *col, float x, float y) {
  return get_pure_map_index_xy(col, x, y);
}
