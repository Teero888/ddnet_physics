#include "../utils.h"
#include <ddnet_physics/collision.h>
#include <ddnet_physics/vmath.h>
#include <limits.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ITERATIONS 3000
#define TICKS_PER_ITERATION 30000
#define TOTAL_TICKS (ITERATIONS * TICKS_PER_ITERATION)
#define NUM_RUNS 10

void print_help(const char *prog_name) {
  printf("Usage: %s [OPTIONS]\n", prog_name);
  printf("Benchmark test_box_character with single or multi-threaded execution.\n\n");
  printf("Options:\n");
  printf("  --multi    Enable multi-threaded execution with OpenMP (default: single-threaded)\n");
  printf("  --help     Display this help message and exit\n");
}

int main(int argc, char *argv[]) {
  int use_multi_threaded = 0;

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

  map_data_t Map = load_map("maps/Aip-Gores.map");
  SCollision Collision;
  if (!init_collision(&Collision, &Map)) {
    printf("Error: Failed to load collision map.\n");
    return 1;
  }
  // Map is now owned by the collision and not needed here anymore
  (void)Map;

  double aTPSValues[NUM_RUNS];
  unsigned int global_seed = 0; // (unsigned)time(NULL);

  printf("Benchmarking test_box_character in %s-threaded mode...\n", use_multi_threaded ? "multi" : "single");
  if (use_multi_threaded)
    printf("Using %d threads with OpenMP.\n", omp_get_max_threads());

  // Generate integer coordinates within the same bounds as the move_box bench
  int min_coord = 128;
  int max_x = Collision.m_MapData.width * 32 - 128;
  int max_y = Collision.m_MapData.height * 32 - 128;

  for (int run = 0; run < NUM_RUNS; run++) {
    double StartTime, ElapsedTime;
    unsigned int run_seed = global_seed ^ (run * 0x9E3779B9u);
    // so compiler doesn't optimize away the function call
    uint64_t total_hits = 0;

    if (use_multi_threaded) {
      StartTime = omp_get_wtime();
#pragma omp parallel for reduction(+ : total_hits)
      for (int i = 0; i < ITERATIONS; ++i) {
        unsigned int local_seed = run_seed ^ i;
        for (int t = 0; t < TICKS_PER_ITERATION; ++t) {
          int x = fast_rand_range(&local_seed, min_coord, max_x);
          int y = fast_rand_range(&local_seed, min_coord, max_y);
          total_hits += test_box_character(&Collision, x, y);
        }
      }
      ElapsedTime = omp_get_wtime() - StartTime;
    } else {
      StartTime = omp_get_wtime();
      for (int i = 0; i < ITERATIONS; ++i) {
        unsigned int local_seed = run_seed ^ i;
        for (int t = 0; t < TICKS_PER_ITERATION; ++t) {
          int x = fast_rand_range(&local_seed, min_coord, max_x);
          int y = fast_rand_range(&local_seed, min_coord, max_y);
          total_hits += test_box_character(&Collision, x, y);
        }
      }
      ElapsedTime = omp_get_wtime() - StartTime;
    }

    (void)total_hits; // Suppress unused variable warning in case it's not used later
    aTPSValues[run] = (double)TOTAL_TICKS / ElapsedTime;
    print_progress(run + 1, NUM_RUNS, ElapsedTime);
  }
  printf("\n");

  PRINT_STATS(aTPSValues, NUM_RUNS)

  free_collision(&Collision);
  return 0;
}
