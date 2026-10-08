/* Print tile positions ("x y" per line) that tees should visit to exercise a
 * map: every tile that does something, the air next to map entities, and a
 * sample of plain air.
 *
 *   map_points <map>
 */
#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <stdio.h>

static int is_solid(const ddnet_collision_t *col, int x, int y) {
  if (x < 0 || y < 0 || x >= col->width || y >= col->height)
    return 1;
  int index = col->game[y * col->width + x].index;
  return index == TILE_SOLID || index == TILE_NOHOOK;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <map>\n", argv[0]);
    return 2;
  }
  map_data_t map = load_map(argv[1]);
  ddnet_collision_t col;
  if (!ddnet_collision_init(&col, &map)) {
    fprintf(stderr, "map_points: cannot load map '%s'\n", argv[1]);
    return 2;
  }

  unsigned state = 1;
  for (int y = 0; y < col.height; y++) {
    for (int x = 0; x < col.width; x++) {
      const int i = y * col.width + x;
      if (is_solid(&col, x, y))
        continue;

      int game = col.game[i].index;
      int front = col.front ? col.front[i].index : 0;
      int interesting = (game != TILE_AIR && game < ENTITY_OFFSET) ||
                        (front != TILE_AIR && front < ENTITY_OFFSET) || (col.tele && col.tele[i].type) ||
                        (col.speedup && col.speedup[i].force) ||
                        (col.switch_tiles && col.switch_tiles[i].type) || (col.tune && col.tune[i].type) ||
                        (col.door && col.door[i].index);

      /* air within two tiles of a map entity (weapons, turrets, draggers, lasers) */
      for (int dy = -2; dy <= 2 && !interesting; dy++) {
        for (int dx = -2; dx <= 2 && !interesting; dx++) {
          int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= col.width || ny >= col.height)
            continue;
          int n = ny * col.width + nx;
          interesting = col.game[n].index >= ENTITY_OFFSET ||
                        (col.front && col.front[n].index >= ENTITY_OFFSET) ||
                        (col.switch_tiles && col.switch_tiles[n].type >= ENTITY_OFFSET);
        }
      }

      /* and every 64th tile of everything else */
      state = state * 1664525u + 1013904223u;
      if (interesting || (state >> 16) % 64 == 0)
        printf("%d %d\n", x, y);
    }
  }

  ddnet_collision_free(&col);
  free_map_data(&map);
  return 0;
}
