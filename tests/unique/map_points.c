/* Print tile positions ("x y" per line) that tees should visit to exercise a
 * map of Unique Race: every tile that does something, the air around the
 * entities (spawns, flags and pickups), and a sample of plain air.
 *
 *   map_points <map> */
#include <ddnet_map_loader.h>

#include <stdio.h>

static int is_solid(const map_data_t *map, int x, int y) {
  if (x < 0 || y < 0 || x >= map->width || y >= map->height)
    return 1;
  int index = map->game_layer.data[y * map->width + x];
  return index == TILE_SOLID || index == TILE_NOHOOK;
}

static int is_entity(const map_data_t *map, int i) {
  return map->game_layer.data[i] > ENTITY_OFFSET || (map->front_layer.data && map->front_layer.data[i] > ENTITY_OFFSET);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <map>\n", argv[0]);
    return 2;
  }
  map_data_t map = load_map(argv[1]);
  if (!map.game_layer.data) {
    fprintf(stderr, "map_points: cannot load map '%s'\n", argv[1]);
    return 2;
  }

  unsigned state = 1;
  for (int y = 0; y < map.height; y++) {
    for (int x = 0; x < map.width; x++) {
      const int i = y * map.width + x;
      if (is_solid(&map, x, y))
        continue;

      int game = map.game_layer.data[i];
      int front = map.front_layer.data ? map.front_layer.data[i] : 0;
      int interesting = (game != TILE_AIR && game <= ENTITY_OFFSET) || (front != TILE_AIR && front <= ENTITY_OFFSET) ||
                        (map.tele_layer.type && map.tele_layer.type[i]) ||
                        (map.speedup_layer.force && map.speedup_layer.force[i]) ||
                        (map.tune_layer.type && map.tune_layer.type[i]);

      /* air within two tiles of an entity */
      for (int dy = -2; dy <= 2 && !interesting; dy++) {
        for (int dx = -2; dx <= 2 && !interesting; dx++) {
          int nx = x + dx, ny = y + dy;
          if (nx >= 0 && ny >= 0 && nx < map.width && ny < map.height)
            interesting = is_entity(&map, ny * map.width + nx);
        }
      }

      /* and every 64th tile of everything else */
      state = state * 1664525u + 1013904223u;
      if (interesting || (state >> 16) % 64 == 0)
        printf("%d %d\n", x, y);
    }
  }

  free_map_data(&map);
  return 0;
}
