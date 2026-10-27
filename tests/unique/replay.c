/* Replay an input script of the Unique oracle on a map with a Unique backend
 * and dump the state of every tick.
 *
 *   replay <map> <script> <dump> <race|fastcap|fastcap_no_weapons> [setting value]...
 *
 * The dump has the format the oracle (the patched Unique Race server) writes,
 * see oracle/record.h, so the two can be compared with dump_diff. The oracle
 * has all scripted players on one server; here every player has a world of
 * its own. Settings are server settings like "sv_health_and_ammo 1", applied
 * before the map settings.
 *
 * With REPLAY_EVENTS=<file> in the environment the effects and sounds
 * (events.h) are written there, per event: the player, the tick, the kind (0
 * sound, 1 particle, 2 damage indicator), the sound, particle or amount, and
 * the bits of x, y and the angle (the reference backend, and the optimized one
 * with its event build, the functions ddnet_ev_*). */
#include "oracle/record.h"

#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *dump_file;

static void put(int32_t value) { fwrite(&value, sizeof(value), 1, dump_file); }

static int32_t bits(float value) {
  int32_t result;
  memcpy(&result, &value, sizeof(result));
  return result;
}

static FILE *events_file;
static int events_tick;
typedef struct event_user_t {
  int player;
} event_user_t;
#if defined(DDNET_PHYSICS_BACKEND_UNIQUE_REFERENCE) || defined(DDNET_PHYSICS_HAS_EVENTS)
#define REPLAY_HAS_EVENTS 1
#endif
/* A function of the backend, of the event build if events are written */
#if defined(DDNET_PHYSICS_BACKEND_UNIQUE_OPTIMIZED) && defined(DDNET_PHYSICS_HAS_EVENTS)
#define EV(name) (events_file ? ddnet_ev_##name : ddnet_##name)
#else
#define EV(name) ddnet_##name
#endif
#ifdef REPLAY_HAS_EVENTS
static void put_event(void *user_data, int kind, int id, ddnet_vec2_t pos, float angle) {
  int32_t rec[7] = {((event_user_t *)user_data)->player, events_tick, kind, id, 0, 0, 0};
  memcpy(&rec[4], &pos.x, 4);
  memcpy(&rec[5], &pos.y, 4);
  memcpy(&rec[6], &angle, 4);
  fwrite(rec, sizeof(rec), 1, events_file);
}
static void on_sound(ddnet_vec2_t pos, int sound, int client_id, void *user_data) {
  (void)client_id;
  put_event(user_data, 0, sound, pos, 0.0f);
}
static void on_particle(ddnet_vec2_t pos, int particle, int client_id, void *user_data) {
  (void)client_id;
  put_event(user_data, 1, particle, pos, 0.0f);
}
static void on_damage_indicator(ddnet_vec2_t pos, float angle, int amount, int client_id, void *user_data) {
  (void)client_id;
  put_event(user_data, 2, amount, pos, angle);
}
#endif

static void dump_player(ddnet_world_t *world) {
  int32_t rec[UREC_NUM] = {0};
  const ddnet_character_t *chr = ddnet_world_character(world);
  int flags = 0;
  if (chr) {
    const ddnet_character_core_t *core = &chr->core;
    rec[UREC_ALIVE] = 1;
    rec[UREC_POS_X] = bits(core->pos.x);
    rec[UREC_POS_Y] = bits(core->pos.y);
    rec[UREC_VEL_X] = bits(core->vel.x);
    rec[UREC_VEL_Y] = bits(core->vel.y);
    rec[UREC_PREV_POS_X] = bits(chr->prev_pos.x);
    rec[UREC_PREV_POS_Y] = bits(chr->prev_pos.y);
    rec[UREC_HOOK_POS_X] = bits(core->hook_pos.x);
    rec[UREC_HOOK_POS_Y] = bits(core->hook_pos.y);
    rec[UREC_HOOK_DIR_X] = bits(core->hook_dir.x);
    rec[UREC_HOOK_DIR_Y] = bits(core->hook_dir.y);
    /* (no hook teleporters: the hook never gets a tele base) */
    rec[UREC_HOOK_TELE_BASE_X] = bits(0.0f);
    rec[UREC_HOOK_TELE_BASE_Y] = bits(0.0f);
    rec[UREC_HOOK_TICK] = core->hook_tick;
    rec[UREC_HOOK_STATE] = core->hook_state;
    rec[UREC_HOOKED_PLAYER] = -1;
    rec[UREC_NEW_HOOK] = 0;
    rec[UREC_ACTIVE_WEAPON] = core->active_weapon;
    rec[UREC_JUMPED] = core->jumped;
    rec[UREC_JUMPED_TOTAL] = core->jumped_total;
    rec[UREC_JUMPS] = core->jumps;
    rec[UREC_DIRECTION] = core->direction;
    rec[UREC_ANGLE] = ddnet_character_angle(chr);
    rec[UREC_COLLIDING] = core->colliding;
    rec[UREC_LEFT_WALL] = core->left_wall;
    /* a tee on a solo server is solo */
    flags |= UREC_FLAG_SOLO;
    if (chr->got_flag[0])
      flags |= UREC_FLAG_GOT_RED_FLAG;
    if (chr->got_flag[1])
      flags |= UREC_FLAG_GOT_BLUE_FLAG;
    rec[UREC_FREEZE_START] = core->freeze_start;
    rec[UREC_FREEZE_TIME] = chr->freeze_time;
    rec[UREC_RELOAD_TIMER] = chr->reload_timer;
    for (int i = 0; i < DDNET_NUM_WEAPONS; i++)
      if (core->weapons[i].got)
        rec[UREC_WEAPONS_GOT] |= 1 << i;
    rec[UREC_AMMO_HAMMER] = core->weapons[DDNET_WEAPON_HAMMER].ammo;
    rec[UREC_AMMO_GUN] = core->weapons[DDNET_WEAPON_GUN].ammo;
    rec[UREC_AMMO_GRENADE] = core->weapons[DDNET_WEAPON_GRENADE].ammo;
    rec[UREC_LAST_WEAPON] = chr->last_weapon;
    rec[UREC_QUEUED_WEAPON] = chr->queued_weapon;
    rec[UREC_ATTACK_TICK] = chr->attack_tick;
    rec[UREC_MOVE_RESTRICTIONS] = chr->move_restrictions;
    rec[UREC_ARMOR] = chr->armor;
    rec[UREC_HEALTH] = chr->health;
    rec[UREC_RACE_STATE] = (int)chr->race_state;
    rec[UREC_START_TIME] = chr->start_time;
    rec[UREC_START_TIME_OFFSET] = bits(chr->start_time_offset);
    rec[UREC_TIME] = bits(chr->time);
    rec[UREC_TELE_CHECKPOINT] = chr->tele_checkpoint;
    rec[UREC_TUNE_ZONE] = chr->tune_zone;
  }
  rec[UREC_FLAGS] = flags;
  const ddnet_player_t *player = &world->player;
  rec[UREC_SPAWNING] = player->spawning;
  rec[UREC_DIE_TICK] = player->die_tick;
  rec[UREC_PREVIOUS_DIE_TICK] = player->previous_die_tick;
  rec[UREC_LAST_KILL] = player->last_kill;
  rec[UREC_FASTCAP_SIDE] = player->fastcap_side;
  rec[UREC_FINISH_TICK] = player->finish_tick;
  rec[UREC_FINISH_TIME] = bits(player->finish_time);
  fwrite(rec, sizeof(rec), 1, dump_file);
}

static void put_projectile(const ddnet_entity_t *ent) {
  int32_t rec[UPROJ_NUM];
  rec[UPROJ_POS_X] = bits(ent->pos.x);
  rec[UPROJ_POS_Y] = bits(ent->pos.y);
  rec[UPROJ_DIR_X] = bits(ent->u.projectile.direction.x);
  rec[UPROJ_DIR_Y] = bits(ent->u.projectile.direction.y);
  rec[UPROJ_START_TICK] = ent->u.projectile.start_tick;
  rec[UPROJ_LIFE_SPAN] = ent->u.projectile.life_span;
  rec[UPROJ_TYPE] = DDNET_WEAPON_GRENADE;
  fwrite(rec, sizeof(rec), 1, dump_file);
}

static void put_pickup(ddnet_vec2_t pos, int type, int subtype, int spawn_tick) {
  int32_t rec[UPICKUP_NUM];
  rec[UPICKUP_POS_X] = bits(pos.x);
  rec[UPICKUP_POS_Y] = bits(pos.y);
  rec[UPICKUP_TYPE] = type;
  rec[UPICKUP_SUBTYPE] = subtype;
  rec[UPICKUP_SPAWN_TICK] = spawn_tick;
  fwrite(rec, sizeof(rec), 1, dump_file);
}

static void dump_entities(const ddnet_world_t *world) {
#if defined(DDNET_PHYSICS_BACKEND_UNIQUE_OPTIMIZED)
  put(world->num_projectiles);
  for (int i = 0; i < world->num_projectiles; i++)
    put_projectile(&world->projectiles[i]);
  put(world->num_pickups);
  for (int i = 0; i < world->num_pickups; i++)
    put_pickup(world->pickups[i].pos, world->pickups[i].type, world->pickups[i].subtype,
               world->pickup_spawn_ticks ? world->pickup_spawn_ticks[i] : -1);
#else
  int count = 0;
  for (int i = world->first_entity[DDNET_ENTTYPE_PROJECTILE]; i != -1; i = world->entities[i].link.next)
    count++;
  put(count);
  for (int i = world->first_entity[DDNET_ENTTYPE_PROJECTILE]; i != -1; i = world->entities[i].link.next)
    put_projectile(&world->entities[i]);

  count = 0;
  for (int i = world->first_entity[DDNET_ENTTYPE_PICKUP]; i != -1; i = world->entities[i].link.next)
    count++;
  put(count);
  for (int i = world->first_entity[DDNET_ENTTYPE_PICKUP]; i != -1; i = world->entities[i].link.next) {
    const ddnet_entity_t *ent = &world->entities[i];
    put_pickup(ent->pos, ent->u.pickup.type, ent->u.pickup.subtype, ent->u.pickup.spawn_tick);
  }
#endif

  /* (the maps have no switches) */
  put(0);
}

int main(int argc, char **argv) {
  if (argc < 5 || (argc - 5) % 2 != 0) {
    fprintf(stderr, "usage: %s <map> <script> <dump> <race|fastcap|fastcap_no_weapons> [setting value]...\n",
            argv[0]);
    return 2;
  }

  map_data_t map = load_map(argv[1]);
  const char *unsupported = map.game_layer.data ? ddnet_map_unsupported(&map) : NULL;
  if (unsupported) {
    fprintf(stderr, "replay: the map has %s\n", unsupported);
    return 2;
  }
  ddnet_collision_t collision;
  if (!ddnet_collision_init(&collision, &map)) {
    fprintf(stderr, "replay: cannot load map '%s'\n", argv[1]);
    return 2;
  }
  free_map_data(&map);

  FILE *script = fopen(argv[2], "rb");
  int32_t header[4];
  if (!script || fread(header, sizeof(header), 1, script) != 1 || header[0] != UNIQUE_SCRIPT_MAGIC ||
      header[1] != UNIQUE_VERSION) {
    fprintf(stderr, "replay: cannot read script '%s'\n", argv[2]);
    return 2;
  }
  const int num_players = header[2];
  const int num_ticks = header[3];
  unique_step_t *steps = malloc((size_t)num_players * num_ticks * sizeof(*steps));
  if (fread(steps, sizeof(*steps), (size_t)num_players * num_ticks, script) != (size_t)num_players * num_ticks) {
    fprintf(stderr, "replay: truncated script '%s'\n", argv[2]);
    return 2;
  }
  fclose(script);

  dump_file = fopen(argv[3], "wb");
  if (!dump_file) {
    fprintf(stderr, "replay: cannot write dump '%s'\n", argv[3]);
    return 2;
  }
  header[0] = UNIQUE_DUMP_MAGIC;
  fwrite(header, sizeof(header), 1, dump_file);

  ddnet_gamemode_t gamemode;
  if (strcmp(argv[4], "race") == 0)
    gamemode = DDNET_MODE_UNIQUE_RACE;
  else if (strcmp(argv[4], "fastcap") == 0)
    gamemode = DDNET_MODE_UNIQUE_FASTCAP;
  else if (strcmp(argv[4], "fastcap_no_weapons") == 0)
    gamemode = DDNET_MODE_UNIQUE_FASTCAP_NO_WEAPONS;
  else {
    fprintf(stderr, "replay: unknown game mode '%s'\n", argv[4]);
    return 2;
  }
  ddnet_config_t config = ddnet_config_default(gamemode);
  for (int i = 5; i + 1 < argc; i += 2) {
    if (!ddnet_config_set(&config, argv[i], atoi(argv[i + 1]))) {
      fprintf(stderr, "replay: unknown setting '%s'\n", argv[i]);
      return 2;
    }
  }

  ddnet_world_t *worlds = calloc((size_t)num_players, sizeof(*worlds));
  ddnet_world_t snapshot = {0};
  event_user_t *users = calloc((size_t)num_players, sizeof(*users));
  const char *events_path = getenv("REPLAY_EVENTS");
  if (events_path) {
    events_file = fopen(events_path, "wb");
    if (!events_file) {
      fprintf(stderr, "replay: cannot write events '%s'\n", events_path);
      return 2;
    }
  }
  for (int i = 0; i < num_players; i++) {
    if (!ddnet_world_init(&worlds[i], &collision, &config))
      return 2;
    users[i].player = i;
#ifdef REPLAY_HAS_EVENTS
    if (events_file) {
      worlds[i].user_data = &users[i];
      worlds[i].sound = on_sound;
      worlds[i].particle = on_particle;
      worlds[i].damage_indicator = on_damage_indicator;
    }
#endif
  }

  for (int tick = 0; tick < num_ticks; tick++) {
    events_tick = worlds[0].tick + 1;
    /* the oracle has one value per tick, see record.h */
    const int tele_out = steps[(size_t)tick * num_players].tele_out;
    for (int i = 0; i < num_players; i++) {
      ddnet_world_t *world = &worlds[i];
      const unique_step_t *step = &steps[(size_t)tick * num_players + i];
      if (step->action == UNIQUE_ACTION_KILL) {
        EV(player_kill)(world);
      } else if (step->action == UNIQUE_ACTION_SIDE) {
        EV(player_set_fastcap_side)(world, step->arg);
      } else if (step->action == UNIQUE_ACTION_TELEPORT) {
        ddnet_character_t *chr = ddnet_world_character(world);
        if (chr) {
          chr->core.pos.x = (step->arg & 0xffff) * 32.0f + 16.0f;
          chr->core.pos.y = (step->arg >> 16) * 32.0f + 16.0f;
          ddnet_character_changed(world);
        }
      }
      world->tele_out = tele_out;

      ddnet_input_t *input = &world->player.input;
      input->direction = step->input[0];
      input->target_x = step->input[1];
      input->target_y = step->input[2];
      input->jump = step->input[3];
      input->fire = step->input[4];
      input->hook = step->input[5];
      input->player_flags = step->input[6];
      input->wanted_weapon = step->input[7];
      input->next_weapon = step->input[8];
      input->prev_weapon = step->input[9];
    }
    for (int i = 0; i < num_players; i++)
      EV(world_tick)(&worlds[i]);

    put(worlds[0].tick);
    for (int i = 0; i < num_players; i++) {
      /* What is dumped has to be up to date. Mostly that is done on a copy,
       * so that the world itself goes on like one that nobody looks at. */
      ddnet_world_t *dumped = &worlds[i];
      if (worlds[i].tick % 97 == 0) {
        ddnet_world_sync(&worlds[i]);
      } else {
        ddnet_world_copy(&snapshot, &worlds[i]);
#ifdef REPLAY_HAS_EVENTS
        snapshot.sound = NULL;
        snapshot.particle = NULL;
        snapshot.damage_indicator = NULL;
#endif
        ddnet_world_sync(&snapshot);
        dumped = &snapshot;
      }
      dump_player(dumped);
      dump_entities(dumped);
    }
  }

  fclose(dump_file);
  if (events_file)
    fclose(events_file);
  for (int i = 0; i < num_players; i++)
    ddnet_world_free(&worlds[i]);
  ddnet_world_free(&snapshot);
  free(worlds);
  free(users);
  ddnet_collision_free(&collision);
  free(steps);
  return 0;
}
