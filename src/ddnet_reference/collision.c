/* Port of DDNet's src/game/collision.cpp, function by function. */
#include "collision.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

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
  int nx = clampi(round_to_int(x) / 32, 0, col->width - 1);
  int ny = clampi(round_to_int(y) / 32, 0, col->height - 1);

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

  if (!load_static_entities(col))
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

int collision_get_move_restrictions(const collision_t *col, switch_active_fn switch_active, void *user,
                                    vec2 pos, float distance, int override_center_tile_index) {
  static const vec2 DIRECTIONS[NUM_MR_DIRS] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
  assert(0.0f <= distance && distance <= 32.0f);
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

  int nx = clampi(x / 32, 0, col->width - 1);
  int ny = clampi(y / 32, 0, col->height - 1);
  const int index = ny * col->width + nx;

  if (col->game[index].index >= TILE_SOLID && col->game[index].index <= TILE_NOLASER)
    return col->game[index].index;
  return 0;
}

int collision_get_front_tile(const collision_t *col, int x, int y) {
  if (!col->front)
    return 0;
  int nx = clampi(x / 32, 0, col->width - 1);
  int ny = clampi(y / 32, 0, col->height - 1);
  if (col->front[ny * col->width + nx].index == TILE_DEATH ||
      col->front[ny * col->width + nx].index == TILE_NOLASER)
    return col->front[ny * col->width + nx].index;
  else
    return 0;
}

int collision_is_solid(const collision_t *col, int x, int y) {
  const int index = collision_get_tile(col, x, y);
  return index == TILE_SOLID || index == TILE_NOHOOK;
}

bool collision_check_point(const collision_t *col, float x, float y) {
  return collision_is_solid(col, round_to_int(x), round_to_int(y));
}

int collision_get_collision_at(const collision_t *col, float x, float y) {
  return collision_get_tile(col, round_to_int(x), round_to_int(y));
}

int collision_get_front_collision_at(const collision_t *col, float x, float y) {
  return collision_get_front_tile(col, round_to_int(x), round_to_int(y));
}

static int is_no_laser(const collision_t *col, int x, int y) {
  return collision_get_tile(col, x, y) == TILE_NOLASER;
}

static int is_front_no_laser(const collision_t *col, int x, int y) {
  return collision_get_front_tile(col, x, y) == TILE_NOLASER;
}

static int get_pure_map_index_xy(const collision_t *col, float x, float y) {
  int nx = clampi(round_to_int(x) / 32, 0, col->width - 1);
  int ny = clampi(round_to_int(y) / 32, 0, col->height - 1);
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
  if (absolutef(x) > absolutef(y)) {
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

int collision_intersect_line(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                             vec2 *out_before_collision) {
  float distance = vdistance(pos0, pos1);
  int end = (int)(distance + 1);
  vec2 last = pos0;
  for (int i = 0; i <= end; i++) {
    float a = i / (float)end;
    vec2 pos = vmix(pos0, pos1, a);
    /* Temporary position for checking collision */
    int ix = round_to_int(pos.x);
    int iy = round_to_int(pos.y);

    if (collision_check_point(col, ix, iy)) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = last;
      return collision_get_collision_at(col, ix, iy);
    }

    last = pos;
  }
  if (out_collision)
    *out_collision = pos1;
  if (out_before_collision)
    *out_before_collision = pos1;
  return 0;
}

int collision_intersect_line_tele_hook(const collision_t *col, bool old_teleport_hook, vec2 pos0, vec2 pos1,
                                       vec2 *out_collision, vec2 *out_before_collision, int *tele_nr) {
  float distance = vdistance(pos0, pos1);
  int end = (int)(distance + 1);
  vec2 last = pos0;
  int dx = 0, dy = 0; /* Offset for checking the "through" tile */
  through_offset(pos0, pos1, &dx, &dy);
  for (int i = 0; i <= end; i++) {
    float a = i / (float)end;
    vec2 pos = vmix(pos0, pos1, a);
    /* Temporary position for checking collision */
    int ix = round_to_int(pos.x);
    int iy = round_to_int(pos.y);

    int index = collision_get_pure_map_index(col, pos);
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
        *out_before_collision = last;
      return TILE_TELEINHOOK;
    }

    int hit = 0;
    if (collision_check_point(col, ix, iy)) {
      if (!is_through(col, ix, iy, dx, dy, pos0, pos1))
        hit = collision_get_collision_at(col, ix, iy);
    } else if (is_hook_blocker(col, ix, iy, pos0, pos1)) {
      hit = TILE_NOHOOK;
    }
    if (hit) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = last;
      return hit;
    }

    last = pos;
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
  vec2 last = pos0;
  for (int i = 0; i <= end; i++) {
    float a = i / (float)end;
    vec2 pos = vmix(pos0, pos1, a);
    /* Temporary position for checking collision */
    int ix = round_to_int(pos.x);
    int iy = round_to_int(pos.y);

    int index = collision_get_pure_map_index(col, pos);
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
        *out_before_collision = last;
      return TILE_TELEINWEAPON;
    }

    if (collision_check_point(col, ix, iy)) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = last;
      return collision_get_collision_at(col, ix, iy);
    }

    last = pos;
  }
  if (out_collision)
    *out_collision = pos1;
  if (out_before_collision)
    *out_before_collision = pos1;
  return 0;
}

static int get_index_xy(const collision_t *col, int nx, int ny) {
  return col->game[ny * col->width + nx].index;
}

static int get_front_index_xy(const collision_t *col, int nx, int ny) {
  if (!col->front)
    return 0;
  return col->front[ny * col->width + nx].index;
}

int collision_intersect_no_laser(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                                 vec2 *out_before_collision) {
  float distance = vdistance(pos0, pos1);
  vec2 last = pos0;

  const int distance_rounded = (int)ceilf(distance);
  for (int i = 0; i < distance_rounded; i++) {
    float a = i / distance;
    vec2 pos = vmix(pos0, pos1, a);
    int nx = clampi(round_to_int(pos.x) / 32, 0, col->width - 1);
    int ny = clampi(round_to_int(pos.y) / 32, 0, col->height - 1);
    if (get_index_xy(col, nx, ny) == TILE_SOLID || get_index_xy(col, nx, ny) == TILE_NOHOOK ||
        get_index_xy(col, nx, ny) == TILE_NOLASER || get_front_index_xy(col, nx, ny) == TILE_NOLASER) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = last;
      if (get_front_index_xy(col, nx, ny) == TILE_NOLASER)
        return collision_get_front_collision_at(col, pos.x, pos.y);
      else
        return collision_get_collision_at(col, pos.x, pos.y);
    }
    last = pos;
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
  vec2 last = pos0;

  const int distance_rounded = (int)ceilf(distance);
  for (int i = 0; i < distance_rounded; i++) {
    float a = (float)i / distance;
    vec2 pos = vmix(pos0, pos1, a);
    if (is_no_laser(col, round_to_int(pos.x), round_to_int(pos.y)) ||
        is_front_no_laser(col, round_to_int(pos.x), round_to_int(pos.y))) {
      if (out_collision)
        *out_collision = pos;
      if (out_before_collision)
        *out_before_collision = last;
      if (is_no_laser(col, round_to_int(pos.x), round_to_int(pos.y)))
        return collision_get_collision_at(col, pos.x, pos.y);
      else
        return collision_get_front_collision_at(col, pos.x, pos.y);
    }
    last = pos;
  }
  if (out_collision)
    *out_collision = pos1;
  if (out_before_collision)
    *out_before_collision = pos1;
  return 0;
}

/* ---------------------------------------------------------------- moving */

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

bool collision_test_box(const collision_t *col, vec2 pos, vec2 size) {
  size = vscale(size, 0.5f);
  if (collision_check_point(col, pos.x - size.x, pos.y - size.y))
    return true;
  if (collision_check_point(col, pos.x + size.x, pos.y - size.y))
    return true;
  if (collision_check_point(col, pos.x - size.x, pos.y + size.y))
    return true;
  if (collision_check_point(col, pos.x + size.x, pos.y + size.y))
    return true;
  return false;
}

bool collision_is_on_ground(const collision_t *col, vec2 pos, float size) {
  if (collision_check_point(col, pos.x + size / 2, pos.y + size / 2 + 5))
    return true;
  if (collision_check_point(col, pos.x - size / 2, pos.y + size / 2 + 5))
    return true;

  return false;
}

void collision_move_box(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, vec2 size, vec2 elasticity,
                        bool *grounded) {
  /* do the move */
  vec2 pos = *inout_pos;
  vec2 vel = *inout_vel;

  float distance = vlength(vel);
  int max = (int)distance;

  if (distance > 0.00001f) {
    float fraction = 1.0f / (float)(max + 1);
    float elasticity_x = clampf(elasticity.x, -1.0f, 1.0f);
    float elasticity_y = clampf(elasticity.y, -1.0f, 1.0f);

    for (int i = 0; i <= max; i++) {
      /* Early break as optimization to stop checking for collisions for
       * large distances after the obstacles we have already hit reduced
       * our speed to exactly 0. */
      if (veq(vel, v2(0, 0)))
        break;

      vec2 new_pos = vadd(pos, vscale(vel, fraction)); /* TODO: this row is not nice */

      /* Fraction can be very small and thus the calculation has no effect, no
       * reason to continue calculating. */
      if (veq(new_pos, pos))
        break;

      if (collision_test_box(col, v2(new_pos.x, new_pos.y), size)) {
        int hits = 0;

        if (collision_test_box(col, v2(pos.x, new_pos.y), size)) {
          if (grounded && elasticity_y > 0 && vel.y > 0)
            *grounded = true;
          new_pos.y = pos.y;
          vel.y *= -elasticity_y;
          hits++;
        }

        if (collision_test_box(col, v2(new_pos.x, pos.y), size)) {
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
      }

      pos = new_pos;
    }
  }

  *inout_pos = pos;
  *inout_vel = vel;
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

bool collision_tile_exists(const collision_t *col, int index) {
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

int collision_get_map_index(const collision_t *col, vec2 pos) {
  int nx = clampi((int)pos.x / 32, 0, col->width - 1);
  int ny = clampi((int)pos.y / 32, 0, col->height - 1);
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
  return it;
}

bool collision_map_indices_next(map_indices_t *it, int *index) {
  const collision_t *col = it->col;
  if (!it->d) {
    /* Not moving: only the tile the tee is on, once. */
    if (it->i != 0)
      return false;
    it->i = 1;
    int nx = clampi((int)it->pos.x / 32, 0, col->width - 1);
    int ny = clampi((int)it->pos.y / 32, 0, col->height - 1);
    *index = ny * col->width + nx;
    return collision_tile_exists(col, *index);
  }

  while (it->i < it->end) {
    float a = it->i / it->d;
    it->i++;
    vec2 tmp = vmix(it->prev_pos, it->pos, a);
    int nx = clampi((int)tmp.x / 32, 0, col->width - 1);
    int ny = clampi((int)tmp.y / 32, 0, col->height - 1);
    int tile = ny * col->width + nx;
    if (collision_tile_exists(col, tile) && it->last_index != tile) {
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
    int nx = clampi((int)pos.x / 32, 0, col->width - 1);
    int ny = clampi((int)pos.y / 32, 0, col->height - 1);

    if ((col->tele) || (col->speedup && col->speedup[ny * col->width + nx].force > 0))
      return ny * col->width + nx;
  }

  const int distance_rounded = (int)ceilf(distance);
  for (int i = 0; i < distance_rounded; i++) {
    float a = (float)i / distance;
    vec2 tmp = vmix(prev_pos, pos, a);
    int nx = clampi((int)tmp.x / 32, 0, col->width - 1);
    int ny = clampi((int)tmp.y / 32, 0, col->height - 1);
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
  int nx = clampi(x / 32, 0, col->width - 1);
  int ny = clampi(y / 32, 0, col->height - 1);
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
