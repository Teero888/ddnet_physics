/* Unique Race: spawn on a fastcap map with the grenade, fire it at the
 * ground and print how the tee flies.
 *
 *   example_rocket_jump ctf1.map */
#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <stdio.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <map>\n", argv[0]);
    return 1;
  }

  map_data_t map = load_map(argv[1]);
  const char *unsupported = map.game_layer.data ? ddnet_map_unsupported(&map) : NULL;
  ddnet_collision_t collision;
  if (unsupported || !ddnet_collision_init(&collision, &map)) {
    fprintf(stderr, "failed to load %s%s%s\n", argv[1], unsupported ? ": it has " : "",
            unsupported ? unsupported : "");
    return 1;
  }
  free_map_data(&map);

  ddnet_config_t config = ddnet_config_default(DDNET_MODE_UNIQUE_FASTCAP);
  ddnet_world_t world;
  if (!ddnet_world_init(&world, &collision, &config))
    return 1;

  for (int i = 0; i < 60; ++i) {
    /* aim down, fire once the spawn delay is over, jump at the same time */
    world.player.input.target_x = 0;
    world.player.input.target_y = 100;
    world.player.input.fire = i >= 10 ? 1 : 0;
    world.player.input.jump = i == 10;
    ddnet_world_tick(&world);

    const ddnet_character_t *tee = ddnet_world_character(&world);
    if (tee && i % 5 == 0)
      printf("tick %3d  pos %8.2f %8.2f  vel %6.2f %6.2f  grenades %d\n", world.tick, tee->core.pos.x,
             tee->core.pos.y, tee->core.vel.x, tee->core.vel.y, tee->core.weapons[DDNET_WEAPON_GRENADE].ammo);
  }

  ddnet_world_free(&world);
  ddnet_collision_free(&collision);
  return 0;
}
