/* Shared between the patched Unique Race server (the oracle) and the test
 * harness of the Unique backends.
 *
 * Script file: what the players do on every tick.
 * Dump file:   the complete physics state after every tick.
 *
 * Both are flat streams of little-endian int32. Floats are stored as their raw
 * bit pattern, so comparing two dumps is a bit-exact comparison.
 *
 * Unique Race is solo: every tee is in a ddrace team of its own and nothing of
 * one tee touches another. The oracle runs all scripted players on one server,
 * ddnet_physics runs a world of its own for every one of them; per player the
 * dump has what the world of that player holds. */
#ifndef UNIQUE_ORACLE_RECORD_H
#define UNIQUE_ORACLE_RECORD_H

#include <stdint.h>

#define UNIQUE_SCRIPT_MAGIC 0x524f5255 /* "UROR" */
#define UNIQUE_DUMP_MAGIC 0x504d5255   /* "URMP" */
#define UNIQUE_VERSION 1

/* Script: header {magic, version, num_players, num_ticks}, then for every tick
 * and every player one unique_step_t. */
enum {
  UNIQUE_ACTION_NONE = 0,
  UNIQUE_ACTION_KILL,     /* the kill bind (with the kill delay of Unique Race) */
  UNIQUE_ACTION_TELEPORT, /* arg = tile x | tile y << 16: move the tee to the center of that tile */
  UNIQUE_ACTION_SIDE,     /* arg = 1 red, 2 blue: the /red and /blue chat commands of fastcap */
};

typedef struct unique_step_t {
  int32_t action;
  int32_t arg;
  /* Decides which exit of a teleporter with several exits is taken. The oracle
   * uses the value of player 0 for everything that happens in the tick. */
  int32_t tele_out;
  int32_t input[10]; /* CNetObj_PlayerInput */
} unique_step_t;

/* Dump: header {magic, version, num_players, num_ticks}, then for every tick:
 *   int32 tick
 *   for every player:
 *     int32[UREC_NUM]
 *     the projectiles of the player in list order: int32 count, count * UPROJ_NUM
 *     the pickups in list order: int32 count, count * UPICKUP_NUM
 *     the switches of the player's team: int32 count, count * {status, type, end_tick} */
enum {
  UREC_ALIVE,
  UREC_POS_X,
  UREC_POS_Y,
  UREC_VEL_X,
  UREC_VEL_Y,
  UREC_PREV_POS_X,
  UREC_PREV_POS_Y,
  UREC_HOOK_POS_X,
  UREC_HOOK_POS_Y,
  UREC_HOOK_DIR_X,
  UREC_HOOK_DIR_Y,
  UREC_HOOK_TELE_BASE_X,
  UREC_HOOK_TELE_BASE_Y,
  UREC_HOOK_TICK,
  UREC_HOOK_STATE,
  UREC_HOOKED_PLAYER,
  UREC_NEW_HOOK,
  UREC_ACTIVE_WEAPON,
  UREC_JUMPED,
  UREC_JUMPED_TOTAL,
  UREC_JUMPS,
  UREC_DIRECTION,
  UREC_ANGLE,
  UREC_COLLIDING,
  UREC_LEFT_WALL,
  UREC_FLAGS, /* UREC_FLAG_* */
  UREC_FREEZE_START,
  UREC_FREEZE_TIME,
  UREC_RELOAD_TIMER,
  UREC_WEAPONS_GOT, /* bit per weapon */
  UREC_AMMO_HAMMER,
  UREC_AMMO_GUN,
  UREC_AMMO_GRENADE,
  UREC_LAST_WEAPON,
  UREC_QUEUED_WEAPON,
  UREC_ATTACK_TICK,
  UREC_MOVE_RESTRICTIONS,
  UREC_ARMOR,
  UREC_HEALTH,
  UREC_RACE_STATE,
  UREC_START_TIME,
  UREC_START_TIME_OFFSET, /* float */
  UREC_TIME,              /* float, the race time of the last tile handled */
  UREC_TELE_CHECKPOINT,
  UREC_TUNE_ZONE,
  /* of the player */
  UREC_SPAWNING,
  UREC_DIE_TICK,
  UREC_PREVIOUS_DIE_TICK,
  UREC_LAST_KILL,
  UREC_FASTCAP_SIDE,
  UREC_FINISH_TICK, /* -1 until the first finish */
  UREC_FINISH_TIME, /* float */
  UREC_NUM
};

enum {
  UREC_FLAG_SOLO = 1 << 0,
  UREC_FLAG_JETPACK = 1 << 1,
  UREC_FLAG_COLLISION_DISABLED = 1 << 2,
  UREC_FLAG_ENDLESS_HOOK = 1 << 3,
  UREC_FLAG_ENDLESS_JUMP = 1 << 4,
  UREC_FLAG_HAMMER_HIT_DISABLED = 1 << 5,
  UREC_FLAG_GRENADE_HIT_DISABLED = 1 << 6,
  UREC_FLAG_LASER_HIT_DISABLED = 1 << 7,
  UREC_FLAG_SHOTGUN_HIT_DISABLED = 1 << 8,
  UREC_FLAG_HOOK_HIT_DISABLED = 1 << 9,
  UREC_FLAG_TELEGUN_GUN = 1 << 12,
  UREC_FLAG_TELEGUN_GRENADE = 1 << 13,
  UREC_FLAG_TELEGUN_LASER = 1 << 14,
  UREC_FLAG_IN_FREEZE = 1 << 15,
  UREC_FLAG_DEEP_FROZEN = 1 << 16,
  UREC_FLAG_LIVE_FROZEN = 1 << 17,
  UREC_FLAG_FROZEN_LAST_TICK = 1 << 18,
  UREC_FLAG_GOT_RED_FLAG = 1 << 19,
  UREC_FLAG_GOT_BLUE_FLAG = 1 << 20,
};

enum {
  UPROJ_POS_X,
  UPROJ_POS_Y,
  UPROJ_DIR_X,
  UPROJ_DIR_Y,
  UPROJ_START_TICK,
  UPROJ_LIFE_SPAN,
  UPROJ_TYPE,
  UPROJ_NUM
};

enum {
  UPICKUP_POS_X,
  UPICKUP_POS_Y,
  UPICKUP_TYPE, /* the POWERUP_* of Unique Race: 0 health, 1 armor, 2 weapon */
  UPICKUP_SUBTYPE,
  UPICKUP_SPAWN_TICK, /* of this player, -1 if it is there for them */
  UPICKUP_NUM
};

static const char *const UNIQUE_RECORD_NAMES[UREC_NUM] = {
    "alive",           "pos.x",        "pos.y",           "vel.x",          "vel.y",
    "prev_pos.x",      "prev_pos.y",   "hook_pos.x",      "hook_pos.y",     "hook_dir.x",
    "hook_dir.y",      "hook_tele_base.x", "hook_tele_base.y", "hook_tick",  "hook_state",
    "hooked_player",   "new_hook",     "active_weapon",   "jumped",         "jumped_total",
    "jumps",           "direction",    "angle",           "colliding",      "left_wall",
    "flags",           "freeze_start", "freeze_time",     "reload_timer",   "weapons_got",
    "ammo[hammer]",    "ammo[gun]",    "ammo[grenade]",   "last_weapon",    "queued_weapon",
    "attack_tick",     "move_restrictions", "armor",      "health",         "race_state",
    "start_time",      "start_time_offset", "time",       "tele_checkpoint", "tune_zone",
    "spawning",        "die_tick",     "previous_die_tick", "last_kill",    "fastcap_side",
    "finish_tick",     "finish_time"};

/* Records that hold a float bit pattern (for pretty printing only). */
static inline int unique_record_is_float(int field) {
  return (field >= UREC_POS_X && field <= UREC_HOOK_TELE_BASE_Y) || field == UREC_START_TIME_OFFSET ||
         field == UREC_TIME || field == UREC_FINISH_TIME;
}

#endif
