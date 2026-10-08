/* Shared between the patched DDNet server (the oracle) and the test harness.
 *
 * Script file: what the players do on every tick.
 * Dump file:   the complete physics state after every tick.
 *
 * Both are flat streams of little-endian int32. Floats are stored as their raw
 * bit pattern, so comparing two dumps is a bit-exact comparison. */
#ifndef DDNET_ORACLE_RECORD_H
#define DDNET_ORACLE_RECORD_H

#include <stdint.h>

#define ORACLE_SCRIPT_MAGIC 0x524f4444 /* "DDOR" */
#define ORACLE_DUMP_MAGIC 0x504d4444   /* "DDMP" */
#define ORACLE_VERSION 3

/* Script: header {magic, version, num_players, num_ticks}, then for every tick
 * and every player one oracle_step_t. */
enum {
  ORACLE_ACTION_NONE = 0,
  ORACLE_ACTION_KILL,      /* self kill, like the kill bind */
  ORACLE_ACTION_TELEPORT,  /* arg = tile x | tile y << 16: move the tee to the center of that tile */
  ORACLE_ACTION_SET_TEAM,  /* arg = ddrace team */
  ORACLE_ACTION_LOCK_TEAM, /* arg = 1 to lock, 0 to unlock the team the player is in */
};

typedef struct oracle_step_t {
  int32_t action;
  int32_t arg;
  int32_t spec; /* nonzero while the player wants to be in /spec */
  /* Decides which exit of a teleporter with several exits is taken. The oracle
   * uses the value of player 0 for everything that happens in the tick. */
  int32_t tele_out;
  int32_t input[10]; /* CNetObj_PlayerInput */
} oracle_step_t;

/* Dump: header {magic, version, num_players, num_ticks}, then for every tick:
 *   int32 tick
 *   num_players * int32[REC_NUM]
 *   3 entity lists (projectile, laser, pickup): int32 count, count * {pos.x, pos.y}
 *   int32 num_switchers, then num_switchers * num_players status values (teams 0..num_players-1) */
enum {
  REC_ALIVE,
  REC_POS_X,
  REC_POS_Y,
  REC_VEL_X,
  REC_VEL_Y,
  REC_HOOK_POS_X,
  REC_HOOK_POS_Y,
  REC_HOOK_DIR_X,
  REC_HOOK_DIR_Y,
  REC_HOOK_TELE_BASE_X,
  REC_HOOK_TELE_BASE_Y,
  REC_HOOK_TICK,
  REC_HOOK_STATE,
  REC_HOOKED_PLAYER,
  REC_NEW_HOOK,
  REC_ACTIVE_WEAPON,
  REC_JUMPED,
  REC_JUMPED_TOTAL,
  REC_JUMPS,
  REC_DIRECTION,
  REC_ANGLE,
  REC_COLLIDING,
  REC_LEFT_WALL,
  REC_FLAGS, /* REC_FLAG_* */
  REC_FREEZE_START,
  REC_FREEZE_TIME,
  REC_RELOAD_TIMER,
  REC_WEAPONS_GOT, /* bit per weapon */
  REC_AMMO_0,
  REC_AMMO_1,
  REC_AMMO_2,
  REC_AMMO_3,
  REC_AMMO_4,
  REC_AMMO_5,
  REC_NINJA_DIR_X,
  REC_NINJA_DIR_Y,
  REC_NINJA_ACTIVATION_TICK,
  REC_NINJA_MOVE_TIME,
  REC_NINJA_OLD_VEL,
  REC_LAST_WEAPON,
  REC_QUEUED_WEAPON,
  REC_ATTACK_TICK,
  REC_MOVE_RESTRICTIONS,
  REC_ARMOR,
  REC_HEALTH,
  REC_TEAM,
  REC_RACE_STATE,
  REC_START_TIME,
  REC_TELE_CHECKPOINT,
  REC_TUNE_ZONE,
  REC_STRONG_WEAK_ID,
  REC_NUM
};

enum {
  REC_FLAG_SOLO = 1 << 0,
  REC_FLAG_JETPACK = 1 << 1,
  REC_FLAG_COLLISION_DISABLED = 1 << 2,
  REC_FLAG_ENDLESS_HOOK = 1 << 3,
  REC_FLAG_ENDLESS_JUMP = 1 << 4,
  REC_FLAG_HAMMER_HIT_DISABLED = 1 << 5,
  REC_FLAG_GRENADE_HIT_DISABLED = 1 << 6,
  REC_FLAG_LASER_HIT_DISABLED = 1 << 7,
  REC_FLAG_SHOTGUN_HIT_DISABLED = 1 << 8,
  REC_FLAG_HOOK_HIT_DISABLED = 1 << 9,

  REC_FLAG_TELEGUN_GUN = 1 << 12,
  REC_FLAG_TELEGUN_GRENADE = 1 << 13,
  REC_FLAG_TELEGUN_LASER = 1 << 14,
  REC_FLAG_IN_FREEZE = 1 << 15,
  REC_FLAG_DEEP_FROZEN = 1 << 16,
  REC_FLAG_LIVE_FROZEN = 1 << 17,
  REC_FLAG_FROZEN_LAST_TICK = 1 << 18,
  REC_FLAG_TEE_STARTED = 1 << 19,
  REC_FLAG_TEE_FINISHED = 1 << 20,
};

static const char *const ORACLE_RECORD_NAMES[REC_NUM] = {"alive",
                                                         "pos.x",
                                                         "pos.y",
                                                         "vel.x",
                                                         "vel.y",
                                                         "hook_pos.x",
                                                         "hook_pos.y",
                                                         "hook_dir.x",
                                                         "hook_dir.y",
                                                         "hook_tele_base.x",
                                                         "hook_tele_base.y",
                                                         "hook_tick",
                                                         "hook_state",
                                                         "hooked_player",
                                                         "new_hook",
                                                         "active_weapon",
                                                         "jumped",
                                                         "jumped_total",
                                                         "jumps",
                                                         "direction",
                                                         "angle",
                                                         "colliding",
                                                         "left_wall",
                                                         "flags",
                                                         "freeze_start",
                                                         "freeze_time",
                                                         "reload_timer",
                                                         "weapons_got",
                                                         "ammo[hammer]",
                                                         "ammo[gun]",
                                                         "ammo[shotgun]",
                                                         "ammo[grenade]",
                                                         "ammo[laser]",
                                                         "ammo[ninja]",
                                                         "ninja.dir.x",
                                                         "ninja.dir.y",
                                                         "ninja.activation_tick",
                                                         "ninja.move_time",
                                                         "ninja.old_vel",
                                                         "last_weapon",
                                                         "queued_weapon",
                                                         "attack_tick",
                                                         "move_restrictions",
                                                         "armor",
                                                         "health",
                                                         "team",
                                                         "race_state",
                                                         "start_time",
                                                         "tele_checkpoint",
                                                         "tune_zone",
                                                         "strong_weak_id"};

/* Records that hold a float bit pattern (for pretty printing only). */
static inline int oracle_record_is_float(int field) {
  return (field >= REC_POS_X && field <= REC_HOOK_TELE_BASE_Y) || field == REC_NINJA_DIR_X ||
         field == REC_NINJA_DIR_Y;
}

#endif
