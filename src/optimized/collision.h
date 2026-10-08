/* CCollision: all queries against the static tiles of the map. */
#ifndef DDNET_PHYSICS_SRC_OPTIMIZED_COLLISION_H
#define DDNET_PHYSICS_SRC_OPTIMIZED_COLLISION_H

#include "../common/vmath.h"

#include <ddnet_map_loader.h>
#include <ddnet_physics/collision.h>

/* How often something happens, for -DDDNET_PHYSICS_PROFILE builds. */
#ifdef DDNET_PHYSICS_PROFILE
enum { PROF_MAX_COUNTERS = 64 };
extern unsigned long long ddnet_prof_counters[PROF_MAX_COUNTERS];
extern const char *ddnet_prof_counter_names[PROF_MAX_COUNTERS];
#define PROF_COUNT(slot, name) (ddnet_prof_counter_names[slot] = name, ddnet_prof_counters[slot]++)
#else
#define PROF_COUNT(slot, name) ((void)0)
#endif

/* For what runs once in many ticks: kept out of the way of what runs on every tick. */
#if defined(__GNUC__) && !defined(DDNET_PHYSICS_NO_COLD)
#define DDNET_COLD __attribute__((cold, noinline))
#else
#define DDNET_COLD
#endif

/* veq() with both coordinates compared at once (the same comparison: a
 * number that is not one is equal to nothing, the two zeros are equal). */
#ifdef __SSE2__
#include <emmintrin.h>
static inline bool veq_fast(vec2 a, vec2 b) {
  const __m128 x = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&a));
  const __m128 y = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&b));
  return (_mm_movemask_ps(_mm_cmpeq_ps(x, y)) & 3) == 3;
}
#else
static inline bool veq_fast(vec2 a, vec2 b) { return veq(a, b); }
#endif

/* round_to_int() of DDNet, "(int)(f + (f > 0 ? 0.5f : -0.5f))", for all
 * floats: half with the sign of f is the same half (for both zeros the sum
 * is cut off to 0 either way), and it is taken without comparing. */
static inline int round_fast(float f) { return (int)(f + copysignf(0.5f, f)); }

/* For what runs on every tick: the compiler puts these next to each other,
 * away from the rest, which is easier on the caches for instructions. */
#if defined(__GNUC__) && !defined(DDNET_PHYSICS_AB)
#define DDNET_HOT __attribute__((hot))
#else
#define DDNET_HOT
#endif

/* For large pieces that run on few ticks, inside of functions that run on
 * every tick: kept out of them, so that what always runs is small. (Not
 * DDNET_COLD: when they run they should be fast.) */
#if defined(__GNUC__) && !defined(DDNET_PHYSICS_AB)
#define DDNET_NOINLINE __attribute__((noinline))
#else
#define DDNET_NOINLINE
#endif

/* For functions with an array on the stack that is only indexed below its size: no canary for the stack
 * protector that the compiler may put in by default (its check runs on every call). */
#if defined(__has_attribute)
#if __has_attribute(no_stack_protector)
#define DDNET_NO_CANARY __attribute__((no_stack_protector))
#endif
#endif
#ifndef DDNET_NO_CANARY
#define DDNET_NO_CANARY
#endif

typedef ddnet_collision_t collision_t;

/* Asks whether the switch with a number is active for whoever is moving. */
typedef bool (*switch_active_fn)(unsigned char number, void *user);

vec2 clamp_vel(int move_restriction, vec2 vel);

int collision_get_move_restrictions(const collision_t *col, switch_active_fn switch_active, void *user,
                                    vec2 pos, float distance, int override_center_tile_index);

int collision_get_tile(const collision_t *col, int x, int y);
int collision_get_front_tile(const collision_t *col, int x, int y);
int collision_is_solid(const collision_t *col, int x, int y);
bool collision_check_point(const collision_t *col, float x, float y);
bool collision_is_on_ground_on_map(const collision_t *col, vec2 pos, float size);
int collision_get_collision_at(const collision_t *col, float x, float y);
int collision_get_front_collision_at(const collision_t *col, float x, float y);

int collision_intersect_line(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                             vec2 *out_before_collision);
int collision_intersect_line_tele_hook(const collision_t *col, bool old_teleport_hook, vec2 pos0, vec2 pos1,
                                       vec2 *out_collision, vec2 *out_before_collision, int *tele_nr);
int collision_intersect_line_tele_weapon(const collision_t *col, bool old_teleport_weapons, vec2 pos0,
                                         vec2 pos1, vec2 *out_collision, vec2 *out_before_collision,
                                         int *tele_nr);
int collision_intersect_no_laser(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                                 vec2 *out_before_collision);
int collision_intersect_no_laser_no_walls(const collision_t *col, vec2 pos0, vec2 pos1, vec2 *out_collision,
                                          vec2 *out_before_collision);

bool collision_sight_blocked(const collision_t *col, int kind, vec2 pos0, vec2 pos1, ddnet_sight_t *sight,
                             ddnet_sight_memo_t *memo, int slot);
bool collision_tile_out_of_sight(const collision_t *col, int kind, vec2 from, int tile);
void collision_move_point(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, float elasticity,
                          int *bounces);
void collision_move_box(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, vec2 size, vec2 elasticity,
                        bool *grounded);
/* The same for a caller that rounds the position to whole numbers afterwards and uses nothing else of it. */
void collision_move_box_rounded(const collision_t *col, vec2 *inout_pos, vec2 *inout_vel, vec2 size,
                                vec2 elasticity, bool *grounded);
bool collision_test_box(const collision_t *col, vec2 pos, vec2 size);
bool collision_is_on_ground(const collision_t *col, vec2 pos, float size);

/* Tile index of a position (rounded), unconditionally. */
int collision_get_pure_map_index(const collision_t *col, vec2 pos);
/* Tile index of a position (truncated), or -1 if nothing on that tile matters. */
int collision_get_map_index(const collision_t *col, vec2 pos);
bool collision_tile_exists(const collision_t *col, int index);

/* GetMapIndices(): the tiles that matter on the way from prev_pos to pos, in
 * order. DDNet collects them into a vector first, here they are produced one
 * by one, which is equivalent because they only depend on static map data. */
typedef struct map_indices_t {
  const collision_t *col;
  vec2 prev_pos;
  vec2 pos;
  float d;
  int end;
  int i;
  int last_index;
  /* the last sample that was looked at and its tile, which the search for the next tile leaves for the loop
   */
  int sample, sample_tile;
} map_indices_t;

map_indices_t collision_map_indices(const collision_t *col, vec2 prev_pos, vec2 pos);
bool collision_map_indices_next(map_indices_t *it, int *index);

int collision_get_tile_index(const collision_t *col, int index);
int collision_get_front_tile_index(const collision_t *col, int index);
int collision_get_tile_flags(const collision_t *col, int index);
int collision_get_front_tile_flags(const collision_t *col, int index);
/* First tile with a teleporter or speedup on the way from prev_pos to pos, or -1. */
int collision_get_index(const collision_t *col, vec2 prev_pos, vec2 pos);

int collision_is_teleport(const collision_t *col, int index);
int collision_is_evil_teleport(const collision_t *col, int index);
bool collision_is_check_teleport(const collision_t *col, int index);
bool collision_is_check_evil_teleport(const collision_t *col, int index);
int collision_is_teleport_weapon(const collision_t *col, int index);
int collision_is_teleport_hook(const collision_t *col, int index);
int collision_is_tele_checkpoint(const collision_t *col, int index);
bool collision_is_speedup(const collision_t *col, int index);
int collision_is_tune(const collision_t *col, int index);
void collision_get_speedup(const collision_t *col, int index, vec2 *dir, int *force, int *max_speed,
                           int *type);
int collision_get_switch_type(const collision_t *col, int index);
int collision_get_switch_number(const collision_t *col, int index);
int collision_get_switch_delay(const collision_t *col, int index);
int collision_is_time_checkpoint(const collision_t *col, int index);
int collision_is_front_time_checkpoint(const collision_t *col, int index);
int collision_mover_speed(const collision_t *col, int x, int y, vec2 *speed);

/* Entity number (tile index - ENTITY_OFFSET) at a tile position of a layer. */
int collision_entity(const collision_t *col, int x, int y, int layer);
int collision_pickup_type(int index, int *subtype);
/* Tile index of a position, rounded down like GetMapIndex() does it. */
static inline int collision_tile_at_pos(const collision_t *col, vec2 pos) {
  /* (on the map nothing has to be clamped; as unsigned numbers, negative ones are too large as well) */
  const unsigned tx = (unsigned)(int)pos.x >> 5, ty = (unsigned)(int)pos.y >> 5;
  if (tx < (unsigned)col->width && ty < (unsigned)col->height)
    return (int)(ty * (unsigned)col->width + tx);
  /* x / 32 and x >> 5 only differ for negative x, which is clamped to 0 either way */
  return clampi((int)pos.y >> 5, 0, col->height - 1) * col->width +
         clampi((int)pos.x >> 5, 0, col->width - 1);
}
/* The flags of the tile a position is on (positions outside of the map are on
 * the tiles at its border), with DDNET_TILEFLAG_QUIET only for a position on
 * the map. Everything that DDNet looks up for a tee at a position is less
 * than a tile away, so on this tile or one of its neighbors: that is what
 * the flags for things nearby are about. */
static inline int collision_flags_at(const collision_t *col, vec2 pos) {
  const int flags = col->tile_flags[collision_tile_at_pos(col, pos)];
  const bool on_map = pos.x >= 0 && pos.y >= 0 && pos.x < col->width * 32.0f && pos.y < col->height * 32.0f;
  return on_map ? flags : flags & ~(DDNET_TILEFLAG_QUIET | DDNET_TILEFLAG_INERT);
}

/* True if the position is on the map and on a tile where nothing is around, see DDNET_TILEFLAG_QUIET. */
static inline bool collision_is_quiet(const collision_t *col, vec2 pos) {
  return (collision_flags_at(col, pos) & DDNET_TILEFLAG_QUIET) != 0;
}
bool collision_rect_is_free_for_projectiles(const collision_t *col, vec2 from, vec2 to);
bool collision_rect_is_free_of_teleporters(const collision_t *col, vec2 from, vec2 to);

#endif
