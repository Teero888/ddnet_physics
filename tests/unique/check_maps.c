/* Which maps the Unique backends refuse, and why (ddnet_map_unsupported()).
 *
 *   check_maps <map>...
 *
 * Prints one line per refused map and exits non-zero if there is one. */
#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <stdio.h>

int main(int argc, char **argv) {
  int refused = 0, checked = 0;
  for (int i = 1; i < argc; i++) {
    map_data_t map = load_map(argv[i]);
    if (!map.game_layer.data) {
      printf("%s: cannot load\n", argv[i]);
      refused++;
      continue;
    }
    const char *unsupported = ddnet_map_unsupported(&map);
    ddnet_collision_t collision;
    if (unsupported) {
      printf("%s: %s\n", argv[i], unsupported);
      refused++;
    } else if (!ddnet_collision_init(&collision, &map)) {
      printf("%s: ddnet_collision_init failed\n", argv[i]);
      refused++;
    } else {
      ddnet_collision_free(&collision);
    }
    checked++;
    free_map_data(&map);
  }
  printf("%d of %d maps supported\n", checked - refused, checked);
  return refused != 0;
}
