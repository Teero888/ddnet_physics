/* ddnet_world_copy must produce a world that behaves exactly like the
 * original: a copy made now and then (into the same world again and again,
 * which reuses its buffers) is ticked along with the original, with the same
 * inputs, and both must stay the same.
 *
 *   test_world_copy <map> <race|fastcap|fastcap_no_weapons> */
#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <stdio.h>
#include <string.h>

static unsigned rng_state = 12345;

static int rng(int n) {
  rng_state = rng_state * 1664525u + 1013904223u;
  return (int)((rng_state >> 8) % (unsigned)n);
}

static void random_input(ddnet_input_t *input) {
  if (rng(10) == 0)
    input->direction = rng(3) - 1;
  if (rng(15) == 0)
    input->jump = !input->jump;
  if (rng(20) == 0)
    input->hook = !input->hook;
  if (rng(4) == 0)
    input->fire = (input->fire + 1) & 0x3f;
  if (rng(8) == 0) {
    input->target_x = rng(400) - 200;
    input->target_y = rng(400) - 100;
  }
  if (rng(100) == 0)
    input->wanted_weapon = rng(5);
}

/* What both worlds must agree on: the tee, the player and the grenades. */
static int same(ddnet_world_t *a, ddnet_world_t *b) {
  if (a->tick != b->tick || memcmp(&a->player, &b->player, sizeof(a->player)) != 0)
    return 0;
  const ddnet_character_t *ca = ddnet_world_character(a), *cb = ddnet_world_character(b);
  if (!ca != !cb || (ca && memcmp(&ca->core.pos, &cb->core.pos, 2 * sizeof(ddnet_vec2_t)) != 0))
    return 0;
  if (ca && (ca->health != cb->health || ca->armor != cb->armor || ca->race_state != cb->race_state ||
             ca->core.weapons[DDNET_WEAPON_GRENADE].ammo != cb->core.weapons[DDNET_WEAPON_GRENADE].ammo))
    return 0;
#if defined(DDNET_PHYSICS_BACKEND_UNIQUE_OPTIMIZED)
  if (a->num_projectiles != b->num_projectiles ||
      memcmp(a->projectiles, b->projectiles, (size_t)a->num_projectiles * sizeof(*a->projectiles)) != 0)
    return 0;
#else
  if (a->num_entities != b->num_entities ||
      memcmp(a->entities, b->entities, (size_t)a->num_entities * sizeof(*a->entities)) != 0)
    return 0;
#endif
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <map> <race|fastcap|fastcap_no_weapons>\n", argv[0]);
    return 2;
  }
  map_data_t map = load_map(argv[1]);
  ddnet_collision_t collision;
  if (!map.game_layer.data || !ddnet_collision_init(&collision, &map)) {
    fprintf(stderr, "test_world_copy: cannot load map '%s'\n", argv[1]);
    return 2;
  }
  free_map_data(&map);

  ddnet_gamemode_t gamemode = strcmp(argv[2], "fastcap") == 0              ? DDNET_MODE_UNIQUE_FASTCAP
                              : strcmp(argv[2], "fastcap_no_weapons") == 0 ? DDNET_MODE_UNIQUE_FASTCAP_NO_WEAPONS
                                                                           : DDNET_MODE_UNIQUE_RACE;
  ddnet_config_t config = ddnet_config_default(gamemode);
  ddnet_config_set(&config, "sv_health_and_ammo", 1);
  ddnet_world_t world, copy = {0};
  if (!ddnet_world_init(&world, &collision, &config))
    return 2;

  int failures = 0;
  ddnet_input_t input = {0};
  for (int t = 0; t < 20000; t++) {
    if (t % 97 == 0 && !ddnet_world_copy(&copy, &world))
      return 2;
    random_input(&input);
    world.player.input = input;
    copy.player.input = input;
    if (rng(400) == 0) {
      ddnet_player_kill(&world);
      ddnet_player_kill(&copy);
    }
    ddnet_world_tick(&world);
    ddnet_world_tick(&copy);
    if (!same(&world, &copy)) {
      printf("tick %d: the copy differs\n", world.tick);
      failures++;
      if (failures > 5)
        break;
    }
  }

  ddnet_world_free(&copy);
  ddnet_world_free(&world);
  ddnet_collision_free(&collision);
  if (!failures)
    printf("identical\n");
  return failures != 0;
}
