#ifndef LIB_TESTS_UTIL_H
#define LIB_TESTS_UTIL_H

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>

clock_t timer_start() { return clock(); }

double timer_end(clock_t start_time) {
  clock_t end_time = clock();
  return (double)(end_time - start_time) / CLOCKS_PER_SEC;
}

void format_int(long long num, char *result) {
  char buffer[50];
  sprintf(buffer, "%lld", num);

  int len = strlen(buffer);
  int newLen = len + (len - 1) / 3;
  int j = newLen - 1;
  result[newLen] = '\0';

  for (int i = len - 1, k = 0; i >= 0; i--, k++) {
    result[j--] = buffer[i];
    if (k % 3 == 2 && i != 0) {
      result[j--] = ',';
    }
  }
}

#define BAR_WIDTH 50
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

static inline float fast_rand_float(unsigned int *state, float min, float max) {
  return min + (fast_rand_u32(state) / (float)UINT32_MAX) * (max - min);
}

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

#endif // LIB_TESTS_UTIL_H
