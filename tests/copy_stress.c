/* Checks that ddnet_world_copy() of the optimized backend, which only copies
 * what changed between two worlds that were the same before, copies
 * everything that differs.
 *
 *   copy_stress <map> [players] [rounds] [name]
 *
 * It is built with DDNET_PHYSICS_CHECK_COPY, which makes every such copy
 * compare all of the two worlds and abort if they differ, and copies worlds
 * back and forth between ticks in the ways a search does: snapshot and
 * restore, the same start again and again, chains. Tees are put onto tiles
 * and entities that do something, so that switches, draggers and turrets get
 * used. A world that was restored is also compared with one that was copied
 * in full, after both went on for a while. */
#include "../src/optimized/unity.c"
#include <stdio.h>

static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

/* places where something is: tiles that do something to a tee, and entities */
static ddnet_vec2_t *points;
static int num_points;

static void tick(ddnet_world_t *w, int players, int ticks) {
  for (int t = 0; t < ticks; t++) {
    for (int c = 0; c < players; c++) {
      const uint64_t r = rnd();
      if (num_points && ((r >> 54) & 63) == 0 && w->players[c].has_character && w->characters[c].alive) {
        ddnet_character_t *chr = &w->characters[c];
        ddnet_vec2_t to = points[(rnd() >> 8) % (unsigned)num_points];
        to.x += (float)((int)(rnd() % 129) - 64);
        to.y += (float)((int)(rnd() % 129) - 64);
        chr->pos = chr->core.pos = chr->prev_pos = to;
        chr->core.vel.x = chr->core.vel.y = 0;
      }
      ddnet_input_t *in = &w->players[c].input;
      in->direction = (int)(r % 3) - 1;
      in->jump = (r >> 8) & 1;
      in->fire = (r >> 9) & 1;
      in->hook = (r >> 10) & 1;
      in->wanted_weapon = (int)((r >> 12) % 6);
      in->target_x = (int)((r >> 20) % 2001) - 1000;
      in->target_y = (int)((r >> 40) % 2001) - 1000;
      if (((r >> 52) & 1023) == 0)
        ddnet_player_kill(w, c);
    }
    ddnet_world_tick(w);
  }
}

static uint32_t sum(ddnet_world_t *w, int players) {
  uint32_t s = (uint32_t)w->num_entities;
  for (int c = 0; c < players; c++) {
    const ddnet_character_t *chr = &w->characters[c];
    s = s * 31 + (uint32_t)(int)chr->core.pos.x + (uint32_t)(int)chr->core.pos.y * 7919u + (uint32_t)chr->freeze_time;
  }
  return s;
}

int main(int argc, char **argv) {
  const int players = argc > 2 ? atoi(argv[2]) : 1;
  const int rounds = argc > 3 ? atoi(argv[3]) : 3000;
  map_data_t map = load_map(argv[1]);
  ddnet_collision_t collision;
  if (!ddnet_collision_init(&collision, &map))
    return 2;
  free_map_data(&map);
  ddnet_config_t config = ddnet_config_default(DDNET_MODE_DDRACE);
  ddnet_world_t a, snap = {0}, work = {0}, full = {0};
  ddnet_world_init(&a, &collision, &config);
  for (int i = 0; i < players; i++)
    ddnet_player_join(&a, i);
  points = malloc(((size_t)collision.width * collision.height + (size_t)a.num_entities + 1) * sizeof(*points));
  for (int i = 0; i < collision.width * collision.height; i++)
    if ((collision.tile_flags[i] & DDNET_TILEFLAG_EXISTS) || (collision.switch_tiles && collision.switch_tiles[i].type))
      points[num_points++] = (ddnet_vec2_t){i % collision.width * 32.0f + 16.0f, i / collision.width * 32.0f + 16.0f};
  for (int i = 0; i < a.num_entities; i++)
    points[num_points++] = a.entities[i].pos;
  tick(&a, players, 60);
  long sparse_like = 0;
  for (int round = 0; round < rounds; round++) {
    const int what = (int)(rnd() % 6);
    const int n = (int)(rnd() % 40);
    switch (what) {
    case 0: /* snapshot, go on, restore */
      ddnet_world_copy(&snap, &a);
      tick(&a, players, n);
      ddnet_world_copy(&a, &snap);
      break;
    case 1: /* search: the same start again and again */
      for (int i = 0; i < 4; i++) {
        ddnet_world_copy(&work, &a);
        tick(&work, players, n);
      }
      break;
    case 2: /* go on */
      tick(&a, players, n * 4);
      break;
    case 3: /* a chain */
      ddnet_world_copy(&work, &a);
      tick(&work, players, n);
      ddnet_world_copy(&snap, &work);
      tick(&snap, players, n);
      ddnet_world_copy(&a, &snap);
      break;
    case 4: /* restore after both went on */
      ddnet_world_copy(&snap, &a);
      tick(&a, players, n);
      tick(&snap, players, 3);
      ddnet_world_copy(&a, &snap);
      break;
    case 5: { /* a restored world goes on like one copied in full */
      ddnet_world_copy(&work, &a);
      tick(&work, players, n);
      ddnet_world_copy(&work, &a);
      ddnet_world_free(&full);
      ddnet_world_copy(&full, &a);
      const uint64_t keep = rng;
      tick(&work, players, 150);
      rng = keep;
      tick(&full, players, 150);
      if (sum(&work, players) != sum(&full, players)) {
        fprintf(stderr, "%s: a restored world went on differently (round %d)\n", argv[1], round);
        return 1;
      }
      sparse_like++;
      break;
    }
    }
  }
  printf("%-22s %d player%s: ok, %d rounds, %ld compared, %ld copies of what changed with %ld entries\n", argv[4] ? argv[4] : "", players, players == 1 ? " " : "s", rounds, sparse_like, ddnet_copy_check_sparse, ddnet_copy_check_entries);
  return 0;
}
