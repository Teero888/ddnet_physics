#include "../data.h"
#include "../utils.h"
#include "ddnet_map_loader.h"
#include <ddnet_physics/collision.h>
#include <ddnet_physics/gamecore.h>
#include <math.h>
#include <omp.h>
#include <stdio.h>

#define ITERATIONS 20
#define NUM_RUNS 50
#define BAR_WIDTH 50
#define TICKS_PER_ITERATION 50000
#define NUM_CHARACTERS 1

typedef struct {
  double mean;
  double stddev;
  double min;
  double max;
} SStats;

// ----- FAST PRNG (xorshift32) -----
static inline unsigned int fast_rand_u32(unsigned int *state) {
  unsigned int x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static inline int fast_rand_range(unsigned int *state, int min, int max) {
  // inclusive range [min, max]
  return min + (fast_rand_u32(state) % (max - min + 1));
}

// ----- STATS -----
static SStats calculate_stats(double *values, int count) {
  SStats stats = {0};
  double sum = 0;
  for (int i = 0; i < count; i++)
    sum += values[i];
  stats.mean = sum / count;

  double variance = 0;
  for (int i = 0; i < count; i++) {
    double diff = values[i] - stats.mean;
    variance += diff * diff;
  }
  variance /= count;
  stats.stddev = sqrt(variance);

  stats.min = values[0];
  stats.max = values[0];
  for (int i = 1; i < count; i++) {
    if (values[i] < stats.min)
      stats.min = values[i];
    if (values[i] > stats.max)
      stats.max = values[i];
  }
  return stats;
}

// ----- PROGRESS -----
void print_progress(int current, int total, double elapsed_time) {
  float progress = (float)current / total;
  int pos = (int)(BAR_WIDTH * progress);

  printf("\r[");
  for (int i = 0; i < BAR_WIDTH; i++) {
    if (i < pos)
      printf("=");
    else if (i == pos)
      printf(">");
    else
      printf(" ");
  }
  printf("] %3.0f%% (Run %d/%d, %.4fs)", progress * 100, current, total, elapsed_time);
  fflush(stdout);
}

void print_help(const char *prog_name) {
  printf("Usage: %s [OPTIONS]\n", prog_name);
  printf("Benchmark the physics engine with single or multi-threaded execution.\n\n");
  printf("Options:\n");
  printf("  --multi            Enable multi-threaded execution with OpenMP (default: single-threaded)\n");
  printf("  --help             Display this help message and exit\n");
}

static inline void generate_random_input(SPlayerInput *pInput, unsigned int *seed) {
  pInput->m_Direction = fast_rand_range(seed, -1, 1);
  pInput->m_Jump = fast_rand_range(seed, 0, 1);
  pInput->m_Fire = fast_rand_range(seed, 0, 1);
  pInput->m_Hook = fast_rand_range(seed, 0, 1);
  pInput->m_TargetX = fast_rand_range(seed, -1000, 1000);
  pInput->m_TargetY = fast_rand_range(seed, -1000, 1000);
  pInput->m_WantedWeapon = fast_rand_range(seed, 0, NUM_WEAPONS - 1);
}

int main(int argc, char *argv[]) {
  int use_multi_threaded = 0;

  // Parse command-line options
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--multi") == 0) {
      use_multi_threaded = 1;
    } else if (strcmp(argv[i], "--help") == 0) {
      print_help(argv[0]);
      return 0;
    } else if (argv[i][0] == '-') {
      printf("Unknown option: %s. Use --help for usage.\n", argv[i]);
      return 1;
    }
  }

  unsigned int global_seed = 0; // (unsigned)time(NULL);

  map_data_t Map = load_map("maps/Aip-Gores.map");
  SCollision Collision;
  if (!init_collision(&Collision, &Map)) {
    printf("Error: Failed to load collision map.\n");
    return 1;
  }
  // Map is now owned by the collision and not needed here anymore
  (void)Map;

  SConfig Config;
  init_config(&Config);

  SWorldCore StartWorld;
  STeeGrid Grid;
  tg_init(&Grid, Collision.m_MapData.width, Collision.m_MapData.height);
  wc_init(&StartWorld, &Collision, &Grid, &Config);
  wc_add_character(&StartWorld, NUM_CHARACTERS);
  for (int t = 0; t < 50; ++t)
    wc_tick(&StartWorld);

  double aTPSValues[NUM_RUNS];
  int total_ticks = ITERATIONS * TICKS_PER_ITERATION;

  printf("Benchmarking physics with random inputs\n");
  printf("Mode: %s-threaded\n", use_multi_threaded ? "multi" : "single");
  if (use_multi_threaded)
    printf("Using %d threads with OpenMP.\n", omp_get_max_threads());

  for (int run = 0; run < NUM_RUNS; run++) {
    double StartTime, ElapsedTime;
    unsigned int run_seed = global_seed ^ (run * 0x9E3779B9u); // vary seeds per run

    if (use_multi_threaded) {
      StartTime = omp_get_wtime();
#pragma omp parallel for
      for (int i = 0; i < ITERATIONS; ++i) {
        unsigned int local_seed = run_seed ^ i; // per-thread unique seed
        SWorldCore World = (SWorldCore){};
        wc_copy_world(&World, &StartWorld);
        for (int t = 0; t < TICKS_PER_ITERATION; ++t) {
          for (int c = 0; c < NUM_CHARACTERS; c++) {
            SPlayerInput Input = {};
            generate_random_input(&Input, &local_seed);
            cc_on_input(&World.m_pCharacters[c], &Input);
          }
          wc_tick(&World);
        }
        wc_free(&World);
      }
      ElapsedTime = omp_get_wtime() - StartTime;
    } else {
      StartTime = omp_get_wtime();
      for (int i = 0; i < ITERATIONS; ++i) {
        unsigned int local_seed = run_seed ^ i;
        SWorldCore World = (SWorldCore){};
        wc_copy_world(&World, &StartWorld);
        for (int t = 0; t < TICKS_PER_ITERATION; ++t) {
          for (int c = 0; c < NUM_CHARACTERS; c++) {
            SPlayerInput Input = {};
            generate_random_input(&Input, &local_seed);
            cc_on_input(&World.m_pCharacters[c], &Input);
          }
          wc_tick(&World);
          // printf("pos:%.2f,%.2f;vel:%.2f,%.2f\n", vgetx(World.m_pCharacters[0].m_Pos),
          //        vgety(World.m_pCharacters[0].m_Pos), vgetx(World.m_pCharacters[0].m_Vel),
          //        vgety(World.m_pCharacters[0].m_Vel));
        }
        wc_free(&World);
      }
      ElapsedTime = omp_get_wtime() - StartTime;
    }

    aTPSValues[run] = (double)total_ticks / ElapsedTime;
    print_progress(run + 1, NUM_RUNS, ElapsedTime);
  }
  printf("\n");

  SStats stats = calculate_stats(aTPSValues, NUM_RUNS);

  char aBuf[32];
  char aBuff[32];
  format_int((int)stats.mean, aBuf);
  format_int((int)stats.stddev, aBuff);
  printf("TPS (mean ± σ):\t\t%s ± %s ticks/s\n", aBuf, aBuff);
  format_int((int)stats.min, aBuf);
  printf("Range (min … max):\t%s … ", aBuf);
  format_int((int)stats.max, aBuf);
  printf("%s ticks/s\t%d runs\n", aBuf, NUM_RUNS);

  wc_free(&StartWorld);
  tg_destroy(&Grid);
  free_collision(&Collision);

  return 0;
}
