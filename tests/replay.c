/* Replay an input script on a map and dump the state of every tick.
 *
 *   replay <map> <script> <dump> [setting value]...
 *
 * The dump has the format the oracle (the patched DDNet server) writes, see
 * tests/oracle/record.h, so the two can be compared with dump_diff. Settings
 * are server settings like "sv_pauseable 1", applied before the map settings.
 *
 * With REPLAY_EVENTS=<file> in the environment the effects and sounds
 * (events.h) are written there too, with the event build of the optimized
 * backend (ddnet_ev_*), to be compared with tests/event_diff.py: per event the
 * tick, the kind (0 sound, 1 particle, 2 damage indicator), the sound,
 * particle or amount, the client id, and the bits of x, y and the angle.
 * With REPLAY_EVENTS_FROM=<tick> the optimized backend runs its build without
 * events until that tick and the event build from there on, like a program
 * that seeks with the one and shows with the other. Without either, it runs
 * the build without events.
 *
 * With REPLAY_DRAW=<file> what a client draws of the world on every tick (see
 * "Effects, sounds and drawing" in the README) is written there, to be
 * compared with cmp: the world of the event build as it is, the other one
 * after ddnet_world_sync. */
#include "oracle/record.h"

#include <ddnet_map_loader.h>
#include <ddnet_physics/ddnet_physics.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *dump_file;

/* the event build where there is one, from REPLAY_EVENTS_FROM on */
#if defined(DDNET_PHYSICS_BACKEND_OPTIMIZED) && defined(DDNET_PHYSICS_HAS_EVENTS)
static int events_from;
#define EV(ev, plain) (world.tick + 1 >= events_from ? ev : plain)
/* (whether the tick that was just done was one of the event build) */
#define EVENT_TICK (world.tick >= events_from)
#define WORLD_TICK EV(ddnet_ev_world_tick, ddnet_world_tick)
#define WORLD_SYNC EV(ddnet_ev_world_sync, ddnet_world_sync)
#define PLAYER_JOIN EV(ddnet_ev_player_join, ddnet_player_join)
#define PLAYER_KILL EV(ddnet_ev_player_kill, ddnet_player_kill)
#define PLAYER_SET_TEAM EV(ddnet_ev_player_set_team, ddnet_player_set_team)
#define SET_EVENTS_FROM(value) (events_from = (value))
#else
#define SET_EVENTS_FROM(value) ((void)(value))
#define EVENT_TICK false
#define WORLD_TICK ddnet_world_tick
#define WORLD_SYNC ddnet_world_sync
#define PLAYER_JOIN ddnet_player_join
#define PLAYER_KILL ddnet_player_kill
#define PLAYER_SET_TEAM ddnet_player_set_team
#endif

static FILE *events_file;
static int events_tick;
static void put_event(int kind, int id, int client_id, ddnet_vec2_t pos, float angle) {
  int32_t rec[7] = {events_tick, kind, id, client_id, 0, 0, 0};
  memcpy(&rec[4], &pos.x, 4);
  memcpy(&rec[5], &pos.y, 4);
  memcpy(&rec[6], &angle, 4);
  fwrite(rec, sizeof(rec), 1, events_file);
}
static void on_sound(ddnet_vec2_t pos, int sound, int client_id, void *user_data) {
  (void)user_data;
  put_event(0, sound, client_id, pos, 0.0f);
}
static void on_particle(ddnet_vec2_t pos, int particle, int client_id, void *user_data) {
  (void)user_data;
  put_event(1, particle, client_id, pos, 0.0f);
}
static void on_damage_indicator(ddnet_vec2_t pos, float angle, int amount, int client_id, void *user_data) {
  (void)user_data;
  put_event(2, amount, client_id, pos, angle);
}

static void put(int32_t value) { fwrite(&value, sizeof(value), 1, dump_file); }

static int32_t bits(float value) {
  int32_t result;
  memcpy(&result, &value, sizeof(result));
  return result;
}

static FILE *draw_file;
static void draw_put(int32_t value) { fwrite(&value, sizeof(value), 1, draw_file); }
static void draw_put_vec(ddnet_vec2_t v) {
  draw_put(bits(v.x));
  draw_put(bits(v.y));
}

/* What the table of the README draws, for every entity of the lists. */
static void dump_draw(const ddnet_world_t *world) {
  draw_put(world->tick);
  static const int TYPES[] = {DDNET_ENTTYPE_PROJECTILE, DDNET_ENTTYPE_LASER, DDNET_ENTTYPE_PICKUP};
  for (int t = 0; t < 3; t++) {
    for (int i = world->first_entity[TYPES[t]]; i != -1; i = world->entities[i].link.next) {
      const ddnet_entity_t *ent = &world->entities[i];
      draw_put(ent->kind);
      draw_put_vec(ent->pos);
      draw_put(ent->number);
      switch (ent->kind) {
      case DDNET_ENTITY_PROJECTILE: {
        const ddnet_projectile_t *proj = &ent->u.projectile;
        draw_put_vec(ddnet_projectile_get_pos(world, ent, (world->tick - proj->start_tick - 1) / 50.0f));
        draw_put_vec(ddnet_projectile_get_pos(world, ent, (world->tick - proj->start_tick) / 50.0f));
        draw_put(proj->type);
        draw_put(proj->owner);
        draw_put(proj->explosive);
        draw_put(proj->freeze);
        draw_put(proj->bouncing);
        break;
      }
      case DDNET_ENTITY_LASER:
        draw_put_vec(ent->u.laser.from);
        draw_put(ent->u.laser.type);
        draw_put(ent->u.laser.eval_tick);
        break;
      case DDNET_ENTITY_LIGHT:
        draw_put_vec(ent->u.light.to);
        break;
      case DDNET_ENTITY_PLASMA:
        draw_put(ent->u.plasma.for_client_id);
        draw_put(ent->u.plasma.eval_tick);
        break;
      case DDNET_ENTITY_DRAGGER_BEAM:
        draw_put(ent->u.dragger_beam.active);
        draw_put(ent->u.dragger_beam.for_client_id);
        break;
      case DDNET_ENTITY_PICKUP:
        draw_put(ent->u.pickup.type);
        draw_put(ent->u.pickup.subtype);
        break;
      default:
        break;
      }
    }
    draw_put(-1);
  }
}

static void dump_character(ddnet_world_t *world, int client_id) {
  int32_t rec[REC_NUM] = {0};
  const ddnet_character_t *chr = ddnet_world_character(world, client_id);
#ifdef DDNET_PHYSICS_BACKEND_OPTIMIZED
  const ddnet_player_t *slot = &world->players[client_id];
  const bool started = slot->tee_started, finished = slot->tee_finished;
  rec[REC_TEAM] = slot->team;
#else
  const bool started = world->teams.tee_started[client_id], finished = world->teams.tee_finished[client_id];
  rec[REC_TEAM] = world->teams.team[client_id];
#endif
  int flags = 0;
  if (started)
    flags |= REC_FLAG_TEE_STARTED;
  if (finished)
    flags |= REC_FLAG_TEE_FINISHED;
  if (chr) {
    const ddnet_character_core_t *core = &chr->core;
    rec[REC_ALIVE] = 1;
    rec[REC_POS_X] = bits(core->pos.x);
    rec[REC_POS_Y] = bits(core->pos.y);
    rec[REC_VEL_X] = bits(core->vel.x);
    rec[REC_VEL_Y] = bits(core->vel.y);
    rec[REC_HOOK_POS_X] = bits(core->hook_pos.x);
    rec[REC_HOOK_POS_Y] = bits(core->hook_pos.y);
    rec[REC_HOOK_DIR_X] = bits(core->hook_dir.x);
    rec[REC_HOOK_DIR_Y] = bits(core->hook_dir.y);
    rec[REC_HOOK_TELE_BASE_X] = bits(core->hook_tele_base.x);
    rec[REC_HOOK_TELE_BASE_Y] = bits(core->hook_tele_base.y);
    rec[REC_HOOK_TICK] = core->hook_tick;
    rec[REC_HOOK_STATE] = core->hook_state;
    rec[REC_HOOKED_PLAYER] = core->hooked_player;
    rec[REC_NEW_HOOK] = core->new_hook;
    rec[REC_ACTIVE_WEAPON] = core->active_weapon;
    rec[REC_JUMPED] = core->jumped;
    rec[REC_JUMPED_TOTAL] = core->jumped_total;
    rec[REC_JUMPS] = core->jumps;
    rec[REC_DIRECTION] = core->direction;
    rec[REC_ANGLE] = ddnet_character_angle(chr);
    rec[REC_COLLIDING] = core->colliding;
    rec[REC_LEFT_WALL] = core->left_wall;
    if (core->solo)
      flags |= REC_FLAG_SOLO;
    if (core->jetpack)
      flags |= REC_FLAG_JETPACK;
    if (core->collision_disabled)
      flags |= REC_FLAG_COLLISION_DISABLED;
    if (core->endless_hook)
      flags |= REC_FLAG_ENDLESS_HOOK;
    if (core->endless_jump)
      flags |= REC_FLAG_ENDLESS_JUMP;
    if (core->hammer_hit_disabled)
      flags |= REC_FLAG_HAMMER_HIT_DISABLED;
    if (core->grenade_hit_disabled)
      flags |= REC_FLAG_GRENADE_HIT_DISABLED;
    if (core->laser_hit_disabled)
      flags |= REC_FLAG_LASER_HIT_DISABLED;
    if (core->shotgun_hit_disabled)
      flags |= REC_FLAG_SHOTGUN_HIT_DISABLED;
    if (core->hook_hit_disabled)
      flags |= REC_FLAG_HOOK_HIT_DISABLED;

    if (core->has_telegun_gun)
      flags |= REC_FLAG_TELEGUN_GUN;
    if (core->has_telegun_grenade)
      flags |= REC_FLAG_TELEGUN_GRENADE;
    if (core->has_telegun_laser)
      flags |= REC_FLAG_TELEGUN_LASER;
    if (core->is_in_freeze)
      flags |= REC_FLAG_IN_FREEZE;
    if (core->deep_frozen)
      flags |= REC_FLAG_DEEP_FROZEN;
    if (core->live_frozen)
      flags |= REC_FLAG_LIVE_FROZEN;
    if (chr->frozen_last_tick)
      flags |= REC_FLAG_FROZEN_LAST_TICK;
    rec[REC_FREEZE_START] = core->freeze_start;
    rec[REC_FREEZE_TIME] = chr->freeze_time;
    rec[REC_RELOAD_TIMER] = chr->reload_timer;
    for (int i = 0; i < DDNET_NUM_WEAPONS; i++) {
      if (core->weapons[i].got)
        rec[REC_WEAPONS_GOT] |= 1 << i;
      rec[REC_AMMO_0 + i] = core->weapons[i].ammo;
    }
    rec[REC_NINJA_DIR_X] = bits(core->ninja.activation_dir.x);
    rec[REC_NINJA_DIR_Y] = bits(core->ninja.activation_dir.y);
    rec[REC_NINJA_ACTIVATION_TICK] = core->ninja.activation_tick;
    rec[REC_NINJA_MOVE_TIME] = core->ninja.current_move_time;
    rec[REC_NINJA_OLD_VEL] = core->ninja.old_vel_amount;
    rec[REC_LAST_WEAPON] = chr->last_weapon;
    rec[REC_QUEUED_WEAPON] = chr->queued_weapon;
    rec[REC_ATTACK_TICK] = chr->attack_tick;
    rec[REC_MOVE_RESTRICTIONS] = chr->move_restrictions;
    rec[REC_ARMOR] = chr->armor;
    rec[REC_HEALTH] = chr->health;
    rec[REC_RACE_STATE] = (int)chr->race_state;
    rec[REC_START_TIME] = chr->start_time;
    rec[REC_TELE_CHECKPOINT] = chr->tele_checkpoint;
    rec[REC_TUNE_ZONE] = chr->tune_zone;
    rec[REC_STRONG_WEAK_ID] = chr->strong_weak_id;
  }
  rec[REC_FLAGS] = flags;
  fwrite(rec, sizeof(rec), 1, dump_file);
}

static void dump_world(ddnet_world_t *world, int num_players) {
  put(world->tick);
  for (int i = 0; i < num_players; i++)
    dump_character(world, i);

  /* Doors are entities in DDNet but part of the collision here. They sit in
   * the laser list and never move, so they are written from the collision. */
  static const int TYPES[] = {DDNET_ENTTYPE_PROJECTILE, DDNET_ENTTYPE_LASER, DDNET_ENTTYPE_PICKUP};
  for (int t = 0; t < 3; t++) {
    int count = 0;
    for (int i = world->first_entity[TYPES[t]]; i != -1; i = world->entities[i].link.next)
      count++;
    put(count);
    for (int i = world->first_entity[TYPES[t]]; i != -1; i = world->entities[i].link.next) {
      put(bits(world->entities[i].pos.x));
      put(bits(world->entities[i].pos.y));
    }
  }

  put(world->num_switchers);
  for (int s = 0; s < world->num_switchers; s++)
    for (int team = 0; team < num_players; team++)
      put(ddnet_world_switch(world, s, team).status);
}

int main(int argc, char **argv) {
  if (argc < 4 || (argc - 4) % 2 != 0) {
    fprintf(stderr, "usage: %s <map> <script> <dump> [setting value]...\n", argv[0]);
    return 2;
  }

  map_data_t map = load_map(argv[1]);
  ddnet_collision_t collision;
  if (!ddnet_collision_init(&collision, &map)) {
    fprintf(stderr, "replay: cannot load map '%s'\n", argv[1]);
    return 2;
  }
  free_map_data(&map);

  FILE *script = fopen(argv[2], "rb");
  int32_t header[4];
  if (!script || fread(header, sizeof(header), 1, script) != 1 || header[0] != ORACLE_SCRIPT_MAGIC ||
      header[1] != ORACLE_VERSION) {
    fprintf(stderr, "replay: cannot read script '%s'\n", argv[2]);
    return 2;
  }
  const int num_players = header[2];
  const int num_ticks = header[3];
  oracle_step_t *steps = malloc((size_t)num_players * num_ticks * sizeof(*steps));
  if (fread(steps, sizeof(*steps), (size_t)num_players * num_ticks, script) !=
      (size_t)num_players * num_ticks) {
    fprintf(stderr, "replay: truncated script '%s'\n", argv[2]);
    return 2;
  }
  fclose(script);

  dump_file = fopen(argv[3], "wb");
  if (!dump_file) {
    fprintf(stderr, "replay: cannot write dump '%s'\n", argv[3]);
    return 2;
  }
  header[0] = ORACLE_DUMP_MAGIC;
  fwrite(header, sizeof(header), 1, dump_file);

  ddnet_config_t config = ddnet_config_default(DDNET_MODE_DDRACE);
  for (int i = 4; i + 1 < argc; i += 2) {
    if (!ddnet_config_set(&config, argv[i], atoi(argv[i + 1]))) {
      fprintf(stderr, "replay: unknown setting '%s'\n", argv[i]);
      return 2;
    }
  }

  ddnet_world_t world, snapshot = {0};
  if (!ddnet_world_init(&world, &collision, &config))
    return 2;
  const char *events_from_value = getenv("REPLAY_EVENTS_FROM");
  const char *events_path = getenv("REPLAY_EVENTS");
  SET_EVENTS_FROM(events_from_value ? atoi(events_from_value) : events_path ? 0 : INT_MAX);
  const char *draw_path = getenv("REPLAY_DRAW");
  if (draw_path) {
    draw_file = fopen(draw_path, "wb");
    if (!draw_file) {
      fprintf(stderr, "replay: cannot write drawing '%s'\n", draw_path);
      return 2;
    }
  }
  if (events_path) {
    events_file = fopen(events_path, "wb");
    if (!events_file) {
      fprintf(stderr, "replay: cannot write events '%s'\n", events_path);
      return 2;
    }
    world.sound = on_sound;
    world.particle = on_particle;
    world.damage_indicator = on_damage_indicator;
  }

  for (int tick = 0; tick < num_ticks; tick++) {
    /* (what happens before the tick, the kills of the script, counts for the tick) */
    events_tick = world.tick + 1;
    if (tick == 0) {
      for (int i = 0; i < num_players; i++)
        PLAYER_JOIN(&world, i);
    }
    world.tele_out = steps[(size_t)tick * num_players].tele_out;
    for (int i = 0; i < num_players; i++) {
      const oracle_step_t *step = &steps[(size_t)tick * num_players + i];
      if (step->action == ORACLE_ACTION_KILL) {
        PLAYER_KILL(&world, i);
      } else if (step->action == ORACLE_ACTION_SET_TEAM) {
        PLAYER_SET_TEAM(&world, i, step->arg);
      } else if (step->action == ORACLE_ACTION_LOCK_TEAM) {
        /* CGameTeams::SetTeamLock: team 0 cannot be locked */
#ifdef DDNET_PHYSICS_BACKEND_OPTIMIZED
        ddnet_world_lock_team(&world, world.players[i].team, step->arg != 0);
#else
        int team = world.teams.team[i];
        if (team != DDNET_TEAM_FLOCK)
          world.teams.team_locked[team] = step->arg != 0;
#endif
      } else if (step->action == ORACLE_ACTION_TELEPORT) {
        ddnet_character_t *chr = ddnet_world_character(&world, i);
        if (chr) {
          chr->core.pos.x = (step->arg & 0xffff) * 32.0f + 16.0f;
          chr->core.pos.y = (step->arg >> 16) * 32.0f + 16.0f;
        }
      }
      world.characters[i].spec = step->spec;
      /* the oracle has one value per tick, see record.h */
      world.characters[i].tele_out = world.tele_out;

      ddnet_input_t *input = &world.players[i].input;
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
    WORLD_TICK(&world);
    /* (the event build draws without a sync) */
    if (draw_file && EVENT_TICK)
      dump_draw(&world);
    /* What is dumped has to be up to date. Mostly that is done on a copy, so
     * that the world itself goes on like one that nobody looks at. */
    const ddnet_world_t *dumped = &world;
    if (world.tick % 97 == 0) {
      WORLD_SYNC(&world);
    } else {
      ddnet_world_copy(&snapshot, &world);
      snapshot.sound = NULL;
      snapshot.particle = NULL;
      snapshot.damage_indicator = NULL;
      ddnet_world_sync(&snapshot);
      dumped = &snapshot;
    }
    dump_world((ddnet_world_t *)dumped, num_players);
    if (draw_file && !EVENT_TICK)
      dump_draw(dumped);
  }

  fclose(dump_file);
  if (draw_file)
    fclose(draw_file);
  if (events_file)
    fclose(events_file);
  ddnet_world_free(&snapshot);
  ddnet_world_free(&world);
  ddnet_collision_free(&collision);
  free(steps);
  return 0;
}
