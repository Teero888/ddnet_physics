/* ddnet_world_copy must produce a world that behaves exactly like the original.
 *
 *   test_world_copy <map> */
#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <stdio.h>
#include <stdlib.h>
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
  if (rng(10) == 0)
    input->fire = (input->fire + 1) & 0x3f;
  if (rng(8) == 0) {
    input->target_x = rng(400) - 200;
    input->target_y = rng(400) - 200;
  }
  if (rng(100) == 0)
    input->wanted_weapon = rng(6);
}

/* Compare everything that is plain data: both worlds must be in the same state. */
static int worlds_equal(const ddnet_world_t *a, const ddnet_world_t *b) {
  if (a->tick != b->tick || a->num_entities != b->num_entities || a->num_clients != b->num_clients ||
#ifdef DDNET_PHYSICS_BACKEND_OPTIMIZED
      a->num_team_rows != b->num_team_rows ||
      (a->num_team_rows && memcmp(a->teams, b->teams, (size_t)a->num_team_rows * sizeof(*a->teams)) != 0) ||
#else
      memcmp(&a->teams, &b->teams, sizeof(a->teams)) != 0 ||
#endif

      memcmp(a->first_entity, b->first_entity, sizeof(a->first_entity)) != 0)
    return 0;
  for (int i = 0; i < a->num_clients; i++) {
    if (a->players[i].active != b->players[i].active ||
        a->players[i].has_character != b->players[i].has_character)
      return 0;
    if (a->players[i].has_character &&
        memcmp(&a->characters[i], &b->characters[i], sizeof(a->characters[i])) != 0)
      return 0;
  }
  for (int i = 0; i < a->num_entities; i++) {
    const ddnet_entity_t *ea = &a->entities[i], *eb = &b->entities[i];
    if (ea->kind != eb->kind || ea->pos.x != eb->pos.x || ea->pos.y != eb->pos.y ||
        ea->link.next != eb->link.next || ea->marked_for_destroy != eb->marked_for_destroy)
      return 0;
  }
  for (int s = 0; s < a->num_switchers; s++)
    for (int team = 0; team < DDNET_NUM_TEAMS; team++) {
      const ddnet_switch_state_t sa = ddnet_world_switch(a, s, team), sb = ddnet_world_switch(b, s, team);
      if (sa.status != sb.status || sa.end_tick != sb.end_tick || sa.type != sb.type ||
          sa.last_update_tick != sb.last_update_tick)
        return 0;
    }
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <map>\n", argv[0]);
    return 2;
  }
  map_data_t map = load_map(argv[1]);
  ddnet_collision_t collision;
  if (!ddnet_collision_init(&collision, &map)) {
    fprintf(stderr, "cannot load map '%s'\n", argv[1]);
    return 2;
  }
  free_map_data(&map);

  enum { PLAYERS = 4, TICKS = 3000 };
  ddnet_config_t config = ddnet_config_default(DDNET_MODE_DDRACE);
  ddnet_world_t world, copy = {0}, scratch = {0};
  if (!ddnet_world_init(&world, &collision, &config))
    return 2;
  for (int i = 0; i < PLAYERS; i++)
    ddnet_player_join(&world, i);

  /* Advance the original and, in lockstep, a chain of copies of copies. */
  if (!ddnet_world_copy(&copy, &world))
    return 2;
  for (int tick = 0; tick < TICKS; tick++) {
    for (int i = 0; i < PLAYERS; i++) {
      random_input(&world.players[i].input);
      copy.players[i].input = world.players[i].input;
    }
    ddnet_world_tick(&world);
    ddnet_world_tick(&copy);
    if (!worlds_equal(&world, &copy)) {
      printf("FAIL: copy diverged at tick %d\n", world.tick);
      return 1;
    }
    /* Round trip through a third world, reusing its buffers every time. */
    if (tick % 7 == 0) {
      if (!ddnet_world_copy(&scratch, &copy) || !ddnet_world_copy(&copy, &scratch))
        return 2;
    }
  }

  printf("ok: %d ticks, %d entities\n", TICKS, world.num_entities);
  ddnet_world_free(&world);
  ddnet_world_free(&copy);
  ddnet_world_free(&scratch);
  ddnet_collision_free(&collision);
  return 0;
}
