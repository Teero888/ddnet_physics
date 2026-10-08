/* Ticks per second with random inputs.
 *
 *   bench <map> [players] [seconds]
 *   bench --tas [players] [seconds]
 *
 * One world, a new random input for every player on every tick. A frozen tee
 * is killed right away: random inputs walk a tee into freeze where it would
 * sit for most of the run, and input is ignored while frozen, so respawning
 * keeps the benchmark on moving tees. Weapons that cannot do anything are not
 * fired (see no_useless_fire()). The checksum at the end depends on every position
 * along the way and must be the same for both backends.
 *
 * With --tas the inputs are not random but a run through Aip-Gores (from the
 * start to the finish, without freezing once; tests/tas_aip_gores.h), replayed
 * again and again from the spawn; every player plays it. It only works on that
 * map, so the map is tests/Aip-Gores.map of the source tree (BENCH_TAS_MAP). Its
 * first 26 inputs are empty: the tee spawns above the start and lands there in
 * that time. */
#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tas_aip_gores.h"

/* (tests/CMakeLists.txt gives the path in the source tree; a build without it
 * runs from the top of the source tree) */
#ifndef BENCH_TAS_MAP
#define BENCH_TAS_MAP "tests/Aip-Gores.map"
#endif

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

/* One 64 bit random number per input, and a part of its bits for every
 * field: the benchmark is to measure the physics, not the generator. */
static inline uint64_t fast_rand(uint64_t *seed) {
  uint64_t x = *seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *seed = x;
  return x * 0x2545F4914F6CDD1Dull;
}

/* a number from 0 to count - 1 out of 16 random bits */
static inline int pick(uint64_t bits, int count) { return (int)(((bits & 0xffff) * (uint32_t)count) >> 16); }

static inline void random_input(ddnet_input_t *input, uint64_t *seed) {
  const uint64_t r = fast_rand(seed);
  input->direction = pick(r, 3) - 1;
  input->jump = (int)(r >> 16) & 1;
  input->fire = (int)(r >> 17) & 1;
  input->hook = (int)(r >> 18) & 1;
  input->wanted_weapon = pick(r >> 19, DDNET_NUM_WEAPONS);
  input->target_x = pick(r >> 32, 2001) - 1000;
  input->target_y = pick(r >> 48, 2001) - 1000;
}

/* Weapons that cannot do anything are not fired: the gun never (without a
 * telegun its bullets change nothing that the checksum sees), and the hammer
 * not when the tee is alone (there is nobody to hit). No fire while the tee
 * has such a weapon in hand or about to be, and on a tick that fires another
 * weapon no weapon is asked for, so that nothing switches before it fires. (A
 * tee without a character fires to respawn, as before.) */
static inline void no_useless_fire(ddnet_input_t *input, const ddnet_world_t *world, int c, int players) {
  const ddnet_character_t *chr = &world->characters[c];
  if (!world->players[c].has_character || !chr->alive)
    return;
  const int active = chr->core.active_weapon, queued = chr->queued_weapon;
  /* (without branches: which weapon is in hand is random, and the benchmark is not to measure itself) */
  const int useless = (active == DDNET_WEAPON_GUN) | (queued == DDNET_WEAPON_GUN) |
                      ((players == 1) & ((active == DDNET_WEAPON_HAMMER) | (queued == DDNET_WEAPON_HAMMER)));
  input->fire &= useless - 1;
  input->wanted_weapon &= -(input->fire == 0);
}

int main(int argc, char **argv) {
  /* (--tas takes the place of the map) */
  const bool tas = argc > 1 && strcmp(argv[1], "--tas") == 0;
  if (argc < 2) {
    fprintf(stderr, "usage: %s <map> [players] [seconds]\n       %s --tas [players] [seconds]\n", argv[0],
            argv[0]);
    return 2;
  }
  const char *map_path = tas ? BENCH_TAS_MAP : argv[1];
  const int players = argc > 2 ? atoi(argv[2]) : 1;
  const double seconds = argc > 3 ? atof(argv[3]) : 2.0;

  map_data_t map = load_map(map_path);
  ddnet_collision_t collision;
  if (!ddnet_collision_init(&collision, &map)) {
    fprintf(stderr, "bench: cannot load map '%s'\n", map_path);
    return 2;
  }
  free_map_data(&map);

  ddnet_config_t config = ddnet_config_default(DDNET_MODE_DDRACE);
  ddnet_world_t start, world = {0};
  if (!ddnet_world_init(&start, &collision, &config))
    return 2;
  for (int i = 0; i < players; i++)
    ddnet_player_join(&start, i);
  /* (the run starts at the spawn) */
  for (int t = 0; t < (tas ? 0 : 50); t++)
    ddnet_world_tick(&start);

  enum { RANDOM_TICKS = 50000, CHECKSUM_ITERATIONS = 2 };
  /* with --tas an iteration is one replay of the run */
  const int ticks_per_iteration = tas ? (int)(sizeof(tas_aip_gores) / sizeof(*tas_aip_gores)) : RANDOM_TICKS;
  long ticks = 0, kills = 0;
  uint32_t checksum = 0;
  double best = 0, timed = 0;
  const double begin = now();
  for (int iteration = 0; iteration < CHECKSUM_ITERATIONS || now() - begin < seconds; iteration++) {
    uint64_t seed = 0x9E3779B97F4A7C15ull * (uint64_t)(iteration + 1);
    ddnet_world_copy(&world, &start);
    const double t0 = now();
    for (int t = 0; t < ticks_per_iteration; t++) {
      if (tas) {
        for (int c = 0; c < players; c++)
          world.players[c].input = tas_aip_gores[t];
      } else {
        for (int c = 0; c < players; c++) {
          random_input(&world.players[c].input, &seed);
          no_useless_fire(&world.players[c].input, &world, c, players);
        }
      }
      ddnet_world_tick(&world);
      for (int c = 0; c < players; c++) {
        /* (ddnet_world_character() without the call) */
        const ddnet_character_t *chr = &world.characters[c];
        if (world.players[c].has_character && chr->alive) {
          if (iteration < CHECKSUM_ITERATIONS)
            checksum =
                checksum * 31 + (uint32_t)(int)chr->core.pos.x + (uint32_t)(int)chr->core.pos.y * 7919u;
          if (chr->freeze_time > 0) {
            ddnet_player_kill(&world, c);
            kills++;
          }
        }
      }
    }
    const double duration = now() - t0;
    if (ticks_per_iteration / duration > best)
      best = ticks_per_iteration / duration;
    timed += duration;
    ticks += ticks_per_iteration;
  }

  /* (the copies of the start world between iterations are not timed) */
  printf("%-28s %d player%s  %6.3f M ticks/s (best %6.3f)  checksum %08x  ticks %ld%s\n",
         tas ? "Aip-Gores" : map_path, players, players == 1 ? " " : "s", ticks / timed / 1e6, best / 1e6,
         checksum, ticks, tas ? "  tas" : "");
  if (tas && players == 1 && kills)
    fprintf(stderr, "bench: the tee froze %ld times, the run of --tas is one through Aip-Gores\n", kills);
#ifdef DDNET_PHYSICS_PROFILE
  extern void ddnet_prof_report(long ticks);
  ddnet_prof_report(ticks);
#endif
  ddnet_world_free(&world);
  ddnet_world_free(&start);
  ddnet_collision_free(&collision);
  return 0;
}
