/* Walk to the right on a map and print where the tee ends up.
 *
 *   example_walk maps/Tutorial.map */
#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <stdio.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <map>\n", argv[0]);
    return 1;
  }

  map_data_t map = load_map(argv[1]);
  ddnet_collision_t collision;
  if (!ddnet_collision_init(&collision, &map)) {
    fprintf(stderr, "failed to load %s\n", argv[1]);
    return 1;
  }
  free_map_data(&map);

  ddnet_config_t config = ddnet_config_default(DDNET_MODE_DDRACE);
  ddnet_world_t world;
  if (!ddnet_world_init(&world, &collision, &config))
    return 1;
  ddnet_player_join(&world, 0);

  for (int i = 0; i < 100; ++i) {
    world.players[0].input.direction = 1;
    world.players[0].input.jump = i % 25 == 0;
    ddnet_world_tick(&world);

    const ddnet_character_t *tee = ddnet_world_character(&world, 0);
    if (tee && i % 10 == 0)
      printf("tick %3d  pos %8.2f %8.2f  vel %6.2f %6.2f\n", world.tick, tee->core.pos.x, tee->core.pos.y,
             tee->core.vel.x, tee->core.vel.y);
  }

  ddnet_world_free(&world);
  ddnet_collision_free(&collision);
  return 0;
}
