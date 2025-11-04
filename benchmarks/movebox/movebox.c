#include "../utils.h"
#include <ddnet_physics/collision.h>
#include <ddnet_physics/vmath.h>
#include <math.h>
#include <omp.h>
#include <stdio.h>

#define ITERATIONS 3000
#define TICKS_PER_ITERATION 3000
#define TOTAL_TICKS (ITERATIONS * TICKS_PER_ITERATION)
#define NUM_RUNS 10

void print_help(const char *prog_name) {
  printf("Usage: %s [OPTIONS]\n", prog_name);
  printf("Benchmark move_box with single or multi-threaded execution.\n\n");
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
  // Map is owned by the collision and not needed here anymore
  (void)Map;

  double aTPSValues[NUM_RUNS];
  unsigned int global_seed = 0; // (unsigned)time(NULL);

  printf("Benchmarking move_box in %s-threaded mode...\n", use_multi_threaded ? "multi" : "single");
  if (use_multi_threaded)
    printf("Using %d threads with OpenMP.\n", omp_get_max_threads());

  float max = fminf(Collision.m_MapData.width * 32.f, Collision.m_MapData.height * 32.f) - 128.f;

  for (int run = 0; run < NUM_RUNS; run++) {
    double StartTime, ElapsedTime;
    unsigned int run_seed = global_seed ^ (run * 0x9E3779B9u);

    if (use_multi_threaded) {
      StartTime = omp_get_wtime();
#pragma omp parallel for
      for (int i = 0; i < ITERATIONS; ++i) {
        unsigned int local_seed = run_seed ^ i;
        for (int t = 0; t < TICKS_PER_ITERATION; ++t) {
          // Generate synthetic random positions and velocities
          mvec2 Pos = vec2_init(fast_rand_float(&local_seed, 128.0f, max), fast_rand_float(&local_seed, 128.0f, max));
          mvec2 Vel = vec2_init(fast_rand_float(&local_seed, -32.0f, 32.0f), fast_rand_float(&local_seed, -32.0f, 32.0f));
          bool Grounded = false;
          mvec2 NewPos, NewVel;
          move_box(&Collision, Pos, Vel, &NewPos, &NewVel, vec2_init(0, 0), &Grounded);
        }
      }
      ElapsedTime = omp_get_wtime() - StartTime;
    } else {
      StartTime = omp_get_wtime();
      for (int i = 0; i < ITERATIONS; ++i) {
        unsigned int local_seed = run_seed ^ i;
        for (int t = 0; t < TICKS_PER_ITERATION; ++t) {
          mvec2 Pos = vec2_init(fast_rand_float(&local_seed, 128.0f, max), fast_rand_float(&local_seed, 128.0f, max));
          mvec2 Vel = vec2_init(fast_rand_float(&local_seed, -32.0f, 32.0f), fast_rand_float(&local_seed, -32.0f, 32.0f));
          bool Grounded = false;
          mvec2 NewPos, NewVel;
          move_box(&Collision, Pos, Vel, &NewPos, &NewVel, vec2_init(0, 0), &Grounded);
        }
      }
      ElapsedTime = omp_get_wtime() - StartTime;
    }

    aTPSValues[run] = (double)TOTAL_TICKS / ElapsedTime;
    print_progress(run + 1, NUM_RUNS, ElapsedTime);
  }
  printf("\n");

  PRINT_STATS(aTPSValues, NUM_RUNS)

  free_collision(&Collision);
  return 0;
}
