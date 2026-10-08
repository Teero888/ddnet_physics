/* Compare two state dumps (tests/oracle/record.h) bit for bit and describe the
 * first difference.
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

static int get(reader_t *r, int32_t *value) { return fread(value, sizeof(*value), 1, r->file) == 1; }

static float as_float(int32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static int32_t must_get(reader_t *r) {
  int32_t value;
  if (!get(r, &value)) {
    fprintf(stderr, "dump_diff: '%s' is truncated\n", r->name);
    exit(2);
  }
  return value;
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
  if (header_a[0] != ORACLE_DUMP_MAGIC || memcmp(header_a, header_b, sizeof(header_a)) != 0) {
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
      for (int f = 0; f < REC_NUM; f++) {
        int32_t va = must_get(&a), vb = must_get(&b);
        if (va == vb)
          continue;
        if (oracle_record_is_float(f))
          printf("tick %d player %d %s: expected %.9g (%08x), got %.9g (%08x)\n", tick, p,
                 ORACLE_RECORD_NAMES[f], as_float(va), (unsigned)va, as_float(vb), (unsigned)vb);
        else
          printf("tick %d player %d %s: expected %d, got %d\n", tick, p, ORACLE_RECORD_NAMES[f], va, vb);
        differences++;
      }
    }

    static const char *const LISTS[] = {"projectile", "laser", "pickup"};
    for (int l = 0; l < 3 && !differences; l++) {
      int32_t count_a = must_get(&a), count_b = must_get(&b);
      if (count_a != count_b) {
        printf("tick %d: %d entities in the %s list, expected %d\n", tick, count_b, LISTS[l], count_a);
        differences++;
        break;
      }
      for (int i = 0; i < count_a * 2; i++) {
        int32_t va = must_get(&a), vb = must_get(&b);
        if (va != vb) {
          printf("tick %d %s #%d pos.%c: expected %.9g, got %.9g\n", tick, LISTS[l], i / 2, i % 2 ? 'y' : 'x',
                 as_float(va), as_float(vb));
          differences++;
        }
      }
    }

    if (!differences) {
      int32_t switchers_a = must_get(&a), switchers_b = must_get(&b);
      if (switchers_a != switchers_b) {
        printf("tick %d: %d switchers, expected %d\n", tick, switchers_b, switchers_a);
        differences++;
      } else {
        for (int s = 0; s < switchers_a; s++) {
          for (int team = 0; team < num_players; team++) {
            int32_t va = must_get(&a), vb = must_get(&b);
            if (va != vb) {
              printf("tick %d switch %d team %d: expected %d, got %d\n", tick, s, team, va, vb);
              differences++;
            }
          }
        }
      }
    }

    if (differences) {
      printf("first difference after %d identical ticks\n", t);
      return 1;
    }
  }

  printf("identical: %d ticks, %d players\n", num_ticks, num_players);
  return 0;
}
