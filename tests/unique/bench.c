/* Ticks per second of a Unique backend with random inputs, and world copies
 * per second.
 *
 *   bench <map> <race|fastcap|fastcap_no_weapons> [seconds] [setting value]...
 *
 * One world, a new random input on every tick (the same as tests/bench.c
 * makes, so that the two benchmarks can be compared). A frozen tee (a heart freezes in race) is killed when the kill delay
 * allows: input is ignored while frozen. The gun and the hammer are not fired
 * (see no_useless_fire()). The checksum at the end depends on every position
 * along the way and must be the same for both backends. */
#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

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

/* No fire while the gun or the hammer is in hand or about to be (they hit
 * nothing), and on a tick that fires no weapon is asked for. (A tee without a
 * character fires to respawn.) */
static inline void no_useless_fire(ddnet_input_t *input, ddnet_world_t *world) {
  /* (ddnet_world_character() without the call) */
  const ddnet_character_t *chr = &world->character;
  if (!world->player.has_character || !chr->alive)
    return;
  const int active = chr->core.active_weapon, queued = chr->queued_weapon;
  const int useless = (active == DDNET_WEAPON_GUN) | (queued == DDNET_WEAPON_GUN) | (active == DDNET_WEAPON_HAMMER) |
                      (queued == DDNET_WEAPON_HAMMER);
  input->fire &= useless - 1;
  input->wanted_weapon &= -(input->fire == 0);
}

int main(int argc, char **argv) {
  if (argc < 3 || (argc > 4 && (argc - 4) % 2 != 0)) {
    fprintf(stderr, "usage: %s <map> <race|fastcap|fastcap_no_weapons> [seconds] [setting value]...\n", argv[0]);
    return 2;
  }
  const char *map_path = argv[1];
  const double seconds = argc > 3 ? atof(argv[3]) : 2.0;

  ddnet_gamemode_t gamemode;
  if (strcmp(argv[2], "race") == 0)
    gamemode = DDNET_MODE_UNIQUE_RACE;
  else if (strcmp(argv[2], "fastcap") == 0)
    gamemode = DDNET_MODE_UNIQUE_FASTCAP;
  else if (strcmp(argv[2], "fastcap_no_weapons") == 0)
    gamemode = DDNET_MODE_UNIQUE_FASTCAP_NO_WEAPONS;
  else {
    fprintf(stderr, "bench: unknown game mode '%s'\n", argv[2]);
    return 2;
  }
  ddnet_config_t config = ddnet_config_default(gamemode);
  for (int i = 4; i + 1 < argc; i += 2) {
    if (!ddnet_config_set(&config, argv[i], atoi(argv[i + 1]))) {
      fprintf(stderr, "bench: unknown setting '%s'\n", argv[i]);
      return 2;
    }
  }

  map_data_t map = load_map(map_path);
  ddnet_collision_t collision;
  if (!map.game_layer.data || !ddnet_collision_init(&collision, &map)) {
    fprintf(stderr, "bench: cannot load map '%s'\n", map_path);
    return 2;
  }
  free_map_data(&map);

  ddnet_world_t start, world = {0}, copy = {0};
  if (!ddnet_world_init(&start, &collision, &config))
    return 2;
  /* (the run starts at the spawn) */
  for (int t = 0; t < 50; t++)
    ddnet_world_tick(&start);

  enum { TICKS = 50000, CHECKSUM_ITERATIONS = 2 };
  long ticks = 0, copies = 0;
  uint32_t checksum = 0;
  double best = 0, timed = 0, copy_time = 0;
  const double begin = now();
  for (int iteration = 0; iteration < CHECKSUM_ITERATIONS || now() - begin < seconds; iteration++) {
    uint64_t seed = 0x9E3779B97F4A7C15ull * (uint64_t)(iteration + 1);
    ddnet_world_copy(&world, &start);
    const double t0 = now();
    for (int t = 0; t < TICKS; t++) {
      random_input(&world.player.input, &seed);
      no_useless_fire(&world.player.input, &world);
      ddnet_world_tick(&world);
      const ddnet_character_t *chr = &world.character;
      if (world.player.has_character && chr->alive) {
        if (iteration < CHECKSUM_ITERATIONS)
          checksum = checksum * 31 + (uint32_t)(int)chr->core.pos.x + (uint32_t)(int)chr->core.pos.y * 7919u;
        if (chr->freeze_time > 0)
          ddnet_player_kill(&world);
      }
    }
    const double duration = now() - t0;
    if (TICKS / duration > best)
      best = TICKS / duration;
    timed += duration;
    ticks += TICKS;

    /* copies of the world as it is after the run (BENCH_NO_COPIES=1: none, for profiling the ticks) */
    if (getenv("BENCH_NO_COPIES"))
      continue;
    enum { COPIES = 200000 };
    const double c0 = now();
    for (int c = 0; c < COPIES; c++)
      ddnet_world_copy(&copy, (c & 1) ? &start : &world);
    copy_time += now() - c0;
    copies += COPIES;
  }

  printf("%-34s %-18s %6.3f M ticks/s (best %6.3f)  %6.2f M copies/s  checksum %08x  ticks %ld\n", map_path,
         argv[2], ticks / timed / 1e6, best / 1e6, copies ? copies / copy_time / 1e6 : 0.0, checksum, ticks);
  ddnet_world_free(&copy);
  ddnet_world_free(&world);
  ddnet_world_free(&start);
  ddnet_collision_free(&collision);
  return 0;
}
