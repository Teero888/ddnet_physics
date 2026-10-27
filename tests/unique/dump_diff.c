/* Compare two state dumps of the Unique oracle (oracle/record.h) bit for bit
 * and describe the first difference.
 *
 *   dump_diff <expected> <actual>
 *
 * Exit code 0 if identical, 1 if different, 2 on errors. */
#include "oracle/record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct reader_t {
  FILE *file;
  const char *name;
} reader_t;

static float as_float(int32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static int32_t must_get(reader_t *r) {
  int32_t value;
  if (fread(&value, sizeof(value), 1, r->file) != 1) {
    fprintf(stderr, "dump_diff: '%s' is truncated\n", r->name);
    exit(2);
  }
  return value;
}

static const char *const PROJ_NAMES[UPROJ_NUM] = {"pos.x", "pos.y", "dir.x", "dir.y", "start_tick", "life_span",
                                                  "type"};
static const char *const PICKUP_NAMES[UPICKUP_NUM] = {"pos.x", "pos.y", "type", "subtype", "spawn_tick"};

/* Compare a list of records of n fields; float_until: the fields below are floats. */
static int compare_list(reader_t *a, reader_t *b, int tick, int player, const char *list, int n,
                        const char *const *names, int float_until) {
  int32_t count_a = must_get(a), count_b = must_get(b);
  if (count_a != count_b) {
    printf("tick %d player %d: %d %ss, expected %d\n", tick, player, count_b, list, count_a);
    return -1;
  }
  int differences = 0;
  for (int i = 0; i < count_a; i++) {
    for (int f = 0; f < n; f++) {
      int32_t va = must_get(a), vb = must_get(b);
      if (va == vb)
        continue;
      if (f < float_until)
        printf("tick %d player %d %s #%d %s: expected %.9g, got %.9g\n", tick, player, list, i, names[f],
               as_float(va), as_float(vb));
      else
        printf("tick %d player %d %s #%d %s: expected %d, got %d\n", tick, player, list, i, names[f], va, vb);
      differences++;
    }
  }
  return differences;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <expected> <actual>\n", argv[0]);
    return 2;
  }
  reader_t a = {fopen(argv[1], "rb"), argv[1]};
  reader_t b = {fopen(argv[2], "rb"), argv[2]};
  if (!a.file || !b.file) {
    fprintf(stderr, "dump_diff: cannot open dumps\n");
    return 2;
  }

  int32_t header_a[4], header_b[4];
  for (int i = 0; i < 4; i++) {
    header_a[i] = must_get(&a);
    header_b[i] = must_get(&b);
  }
  if (header_a[0] != UNIQUE_DUMP_MAGIC || memcmp(header_a, header_b, sizeof(header_a)) != 0) {
    fprintf(stderr, "dump_diff: headers differ or are invalid\n");
    return 2;
  }
  const int num_players = header_a[2];
  const int num_ticks = header_a[3];

  for (int t = 0; t < num_ticks; t++) {
    int32_t tick = must_get(&a);
    int32_t tick_b = must_get(&b);
    if (tick != tick_b) {
      printf("step %d: tick %d != %d\n", t, tick, tick_b);
      return 1;
    }

    int differences = 0;
    for (int p = 0; p < num_players; p++) {
      for (int f = 0; f < UREC_NUM; f++) {
        int32_t va = must_get(&a), vb = must_get(&b);
        if (va == vb)
          continue;
        if (unique_record_is_float(f))
          printf("tick %d player %d %s: expected %.9g (%08x), got %.9g (%08x)\n", tick, p,
                 UNIQUE_RECORD_NAMES[f], as_float(va), (unsigned)va, as_float(vb), (unsigned)vb);
        else
          printf("tick %d player %d %s: expected %d, got %d\n", tick, p, UNIQUE_RECORD_NAMES[f], va, vb);
        differences++;
      }
      int d = compare_list(&a, &b, tick, p, "projectile", UPROJ_NUM, PROJ_NAMES, UPROJ_START_TICK);
      if (d < 0) {
        printf("first difference after %d identical ticks\n", t);
        return 1;
      }
      differences += d;
      d = compare_list(&a, &b, tick, p, "pickup", UPICKUP_NUM, PICKUP_NAMES, UPICKUP_TYPE);
      if (d < 0) {
        printf("first difference after %d identical ticks\n", t);
        return 1;
      }
      differences += d;
      static const char *const SWITCH_NAMES[3] = {"status", "type", "end_tick"};
      d = compare_list(&a, &b, tick, p, "switch", 3, SWITCH_NAMES, 0);
      if (d < 0) {
        printf("first difference after %d identical ticks\n", t);
        return 1;
      }
      differences += d;
    }

    if (differences) {
      printf("first difference after %d identical ticks\n", t);
      return 1;
    }
  }

  printf("identical: %d ticks, %d players\n", num_ticks, num_players);
  return 0;
}
