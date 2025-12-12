#include "tiles.h"
#include <ddnet_map_loader.h>
#include <ddnet_physics/collision.h>
#include <ddnet_physics/gamecore.h>

tile_handler_f g_apGameTileHandlers[MAX_TILES];
switch_handler_f g_apSwitchTileHandlers[MAX_TILES];

static void cc_handle_tile_empty(SCharacterCore *pCore) {
  (void)pCore;
  return;
}

static void cc_handle_tile_start(SCharacterCore *pCore) {
  if (pCore->m_StartTick == -1 || !pCore->m_pWorld->m_pConfig->m_SvSoloServer) {
    pCore->m_StartTick = pCore->m_pWorld->m_GameTick;
    pCore->m_FinishTick = -1;
  }
}

static void cc_handle_tile_finish(SCharacterCore *pCore) {
  if (pCore->m_StartTick != -1 && pCore->m_FinishTick == -1) {
    pCore->m_FinishTick = pCore->m_pWorld->m_GameTick;
  }
}

static void cc_handle_tile_freeze(SCharacterCore *pCore) {
  if (!pCore->m_DeepFrozen) {
    cc_freeze(pCore, pCore->m_pWorld->m_pConfig->m_SvFreezeDelay);
  }
}

static void cc_handle_tile_unfreeze(SCharacterCore *pCore) {
  if (!pCore->m_DeepFrozen) {
    cc_unfreeze(pCore);
  }
}

static void cc_handle_tile_dfreeze(SCharacterCore *pCore) { pCore->m_DeepFrozen = true; }
static void cc_handle_tile_dunfreeze(SCharacterCore *pCore) { pCore->m_DeepFrozen = false; }
static void cc_handle_tile_lfreeze(SCharacterCore *pCore) { pCore->m_LiveFrozen = true; }
static void cc_handle_tile_lunfreeze(SCharacterCore *pCore) { pCore->m_LiveFrozen = false; }
static void cc_handle_tile_ehook_enable(SCharacterCore *pCore) { pCore->m_EndlessHook = true; }
static void cc_handle_tile_ehook_disable(SCharacterCore *pCore) { pCore->m_EndlessHook = false; }

static void cc_handle_tile_hit_disable(SCharacterCore *pCore) {
  pCore->m_HammerHitDisabled = true;
  pCore->m_ShotgunHitDisabled = true;
  pCore->m_GrenadeHitDisabled = true;
  pCore->m_LaserHitDisabled = true;
}

static void cc_handle_tile_hit_enable(SCharacterCore *pCore) {
  pCore->m_ShotgunHitDisabled = false;
  pCore->m_GrenadeHitDisabled = false;
  pCore->m_HammerHitDisabled = false;
  pCore->m_LaserHitDisabled = false;
}

static void cc_handle_tile_npc_disable(SCharacterCore *pCore) { pCore->m_CollisionDisabled = true; }
static void cc_handle_tile_npc_enable(SCharacterCore *pCore) { pCore->m_CollisionDisabled = false; }
static void cc_handle_tile_nph_disable(SCharacterCore *pCore) { pCore->m_HookHitDisabled = true; }
static void cc_handle_tile_nph_enable(SCharacterCore *pCore) { pCore->m_HookHitDisabled = false; }
static void cc_handle_tile_solo_disable(SCharacterCore *pCore) { pCore->m_Solo = false; }
static void cc_handle_tile_solo_enable(SCharacterCore *pCore) { pCore->m_Solo = true; }
static void cc_handle_tile_jumps_disable(SCharacterCore *pCore) { pCore->m_EndlessJump = false; }
static void cc_handle_tile_jumps_enable(SCharacterCore *pCore) { pCore->m_EndlessJump = true; }

static void cc_handle_tile_walljump(SCharacterCore *pCore) {
  if (vgety(pCore->m_Vel) > 0 && pCore->m_Colliding && pCore->m_LeftWall) {
    pCore->m_LeftWall = false;
    pCore->m_JumpedTotal = pCore->m_Jumps >= 2 ? pCore->m_Jumps - 2 : 0;
    pCore->m_Jumped = 1;
  }
}

static void cc_handle_tile_jetpack_enable(SCharacterCore *pCore) { pCore->m_Jetpack = true; }
static void cc_handle_tile_jetpack_disable(SCharacterCore *pCore) { pCore->m_Jetpack = false; }

static void cc_handle_tile_refill_jumps(SCharacterCore *pCore) {
  // The m_LastRefillJumps flag logic is handled in the main cc_handle_tiles
  // function to correctly manage enter/exit state.
  if (!pCore->m_LastRefillJumps) {
    pCore->m_JumpedTotal = 0;
    pCore->m_Jumped = 0;
  }
}

static void cc_handle_tile_telegun_gun_enable(SCharacterCore *pCore) { pCore->m_HasTelegunGun = true; }
static void cc_handle_tile_telegun_gun_disable(SCharacterCore *pCore) { pCore->m_HasTelegunGun = false; }
static void cc_handle_tile_telegun_grenade_enable(SCharacterCore *pCore) { pCore->m_HasTelegunGrenade = true; }
static void cc_handle_tile_telegun_grenade_disable(SCharacterCore *pCore) { pCore->m_HasTelegunGrenade = false; }
static void cc_handle_tile_telegun_laser_enable(SCharacterCore *pCore) { pCore->m_HasTelegunLaser = true; }
static void cc_handle_tile_telegun_laser_disable(SCharacterCore *pCore) { pCore->m_HasTelegunLaser = false; }

static void cc_handle_switch_empty(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  (void)pCore;
  (void)ucNumber;
  (void)ucDelay;
  return;
}

static void cc_handle_switch_open(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (ucNumber == 0)
    return;
  SSwitch *pSwitch = pCore->m_pWorld->m_pSwitches + ucNumber;
  pSwitch->m_Status = true;
  pSwitch->m_EndTick = 0;
  pSwitch->m_Type = TILE_SWITCHOPEN;
  pSwitch->m_LastUpdateTick = pCore->m_pWorld->m_GameTick;
}

static void cc_handle_switch_timed_open(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (ucNumber == 0)
    return;
  SSwitch *pSwitch = pCore->m_pWorld->m_pSwitches + ucNumber;
  pSwitch->m_Status = true;
  pSwitch->m_EndTick = pCore->m_pWorld->m_GameTick + 1 + ucDelay * GAME_TICK_SPEED;
  pSwitch->m_Type = TILE_SWITCHTIMEDOPEN;
  pSwitch->m_LastUpdateTick = pCore->m_pWorld->m_GameTick;
}

static void cc_handle_switch_timed_close(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (ucNumber == 0)
    return;
  SSwitch *pSwitch = pCore->m_pWorld->m_pSwitches + ucNumber;
  pSwitch->m_Status = false;
  pSwitch->m_EndTick = pCore->m_pWorld->m_GameTick + 1 + ucDelay * GAME_TICK_SPEED;
  pSwitch->m_Type = TILE_SWITCHTIMEDCLOSE;
  pSwitch->m_LastUpdateTick = pCore->m_pWorld->m_GameTick;
}

static void cc_handle_switch_close(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (ucNumber == 0)
    return;
  SSwitch *pSwitch = pCore->m_pWorld->m_pSwitches + ucNumber;
  pSwitch->m_Status = false;
  pSwitch->m_EndTick = 0;
  pSwitch->m_Type = TILE_SWITCHCLOSE;
  pSwitch->m_LastUpdateTick = pCore->m_pWorld->m_GameTick;
}

static void cc_handle_switch_freeze(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (ucNumber == 0 || (pCore->m_pWorld->m_pSwitches + ucNumber)->m_Status) {
    cc_freeze(pCore, ucDelay);
  }
}

static void cc_handle_switch_dfreeze(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (ucNumber == 0 || (pCore->m_pWorld->m_pSwitches + ucNumber)->m_Status) {
    pCore->m_DeepFrozen = true;
  }
}

static void cc_handle_switch_dunfreeze(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (ucNumber == 0 || (pCore->m_pWorld->m_pSwitches + ucNumber)->m_Status) {
    pCore->m_DeepFrozen = false;
  }
}

static void cc_handle_switch_lfreeze(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (ucNumber == 0 || (pCore->m_pWorld->m_pSwitches + ucNumber)->m_Status) {
    pCore->m_LiveFrozen = true;
  }
}

static void cc_handle_switch_lunfreeze(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (ucNumber == 0 || (pCore->m_pWorld->m_pSwitches + ucNumber)->m_Status) {
    pCore->m_LiveFrozen = false;
  }
}

static void cc_handle_switch_hit_enable(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  // ucDelay holds the weapon type
  switch (ucDelay) {
  case WEAPON_HAMMER:
    pCore->m_HammerHitDisabled = false;
    break;
  case WEAPON_SHOTGUN:
    pCore->m_ShotgunHitDisabled = false;
    break;
  case WEAPON_GRENADE:
    pCore->m_GrenadeHitDisabled = false;
    break;
  case WEAPON_LASER:
    pCore->m_LaserHitDisabled = false;
    break;
  }
}

static void cc_handle_switch_hit_disable(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  // ucDelay holds the weapon type
  switch (ucDelay) {
  case WEAPON_HAMMER:
    pCore->m_HammerHitDisabled = true;
    break;
  case WEAPON_SHOTGUN:
    pCore->m_ShotgunHitDisabled = true;
    break;
  case WEAPON_GRENADE:
    pCore->m_GrenadeHitDisabled = true;
    break;
  case WEAPON_LASER:
    pCore->m_LaserHitDisabled = true;
    break;
  }
}

static void cc_handle_switch_jump(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  int NewJumps = ucDelay;
  if (NewJumps == 255) {
    NewJumps = -1;
  }
  if (NewJumps != pCore->m_Jumps) {
    pCore->m_Jumps = NewJumps;
  }
}

static void cc_handle_switch_add_time(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (pCore->m_LastPenalty)
    return;

  int min = ucDelay;
  int sec = ucNumber;
  pCore->m_StartTime -= (min * 60 + sec) * GAME_TICK_SPEED;
  pCore->m_LastPenalty = true; // Set flag here
}

static void cc_handle_switch_subtract_time(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay) {
  if (pCore->m_LastBonus)
    return;

  int min = ucDelay;
  int sec = ucNumber;
  pCore->m_StartTime += (min * 60 + sec) * GAME_TICK_SPEED;
  if (pCore->m_StartTime > pCore->m_pWorld->m_GameTick)
    pCore->m_StartTime = pCore->m_pWorld->m_GameTick;
  pCore->m_LastBonus = true; // Set flag here
}

void cc_init_tile_handlers(void) {
  for (int i = 0; i < MAX_TILES; ++i) {
    g_apGameTileHandlers[i] = cc_handle_tile_empty;
    g_apSwitchTileHandlers[i] = cc_handle_switch_empty;
  }

  g_apGameTileHandlers[TILE_START] = cc_handle_tile_start;
  g_apGameTileHandlers[TILE_FINISH] = cc_handle_tile_finish;
  g_apGameTileHandlers[TILE_FREEZE] = cc_handle_tile_freeze;
  g_apGameTileHandlers[TILE_UNFREEZE] = cc_handle_tile_unfreeze;
  g_apGameTileHandlers[TILE_DFREEZE] = cc_handle_tile_dfreeze;
  g_apGameTileHandlers[TILE_DUNFREEZE] = cc_handle_tile_dunfreeze;
  g_apGameTileHandlers[TILE_LFREEZE] = cc_handle_tile_lfreeze;
  g_apGameTileHandlers[TILE_LUNFREEZE] = cc_handle_tile_lunfreeze;
  g_apGameTileHandlers[TILE_EHOOK_ENABLE] = cc_handle_tile_ehook_enable;
  g_apGameTileHandlers[TILE_EHOOK_DISABLE] = cc_handle_tile_ehook_disable;
  g_apGameTileHandlers[TILE_HIT_DISABLE] = cc_handle_tile_hit_disable;
  g_apGameTileHandlers[TILE_HIT_ENABLE] = cc_handle_tile_hit_enable;
  g_apGameTileHandlers[TILE_NPC_DISABLE] = cc_handle_tile_npc_disable;
  g_apGameTileHandlers[TILE_NPC_ENABLE] = cc_handle_tile_npc_enable;
  g_apGameTileHandlers[TILE_NPH_DISABLE] = cc_handle_tile_nph_disable;
  g_apGameTileHandlers[TILE_NPH_ENABLE] = cc_handle_tile_nph_enable;
  g_apGameTileHandlers[TILE_SOLO_ENABLE] = cc_handle_tile_solo_enable;
  g_apGameTileHandlers[TILE_SOLO_DISABLE] = cc_handle_tile_solo_disable;
  g_apGameTileHandlers[TILE_UNLIMITED_JUMPS_ENABLE] = cc_handle_tile_jumps_enable;
  g_apGameTileHandlers[TILE_UNLIMITED_JUMPS_DISABLE] = cc_handle_tile_jumps_disable;
  g_apGameTileHandlers[TILE_WALLJUMP] = cc_handle_tile_walljump;
  g_apGameTileHandlers[TILE_JETPACK_ENABLE] = cc_handle_tile_jetpack_enable;
  g_apGameTileHandlers[TILE_JETPACK_DISABLE] = cc_handle_tile_jetpack_disable;
  g_apGameTileHandlers[TILE_REFILL_JUMPS] = cc_handle_tile_refill_jumps;
  g_apGameTileHandlers[TILE_TELE_GUN_ENABLE] = cc_handle_tile_telegun_gun_enable;
  g_apGameTileHandlers[TILE_TELE_GUN_DISABLE] = cc_handle_tile_telegun_gun_disable;
  g_apGameTileHandlers[TILE_TELE_GRENADE_ENABLE] = cc_handle_tile_telegun_grenade_enable;
  g_apGameTileHandlers[TILE_TELE_GRENADE_DISABLE] = cc_handle_tile_telegun_grenade_disable;
  g_apGameTileHandlers[TILE_TELE_LASER_ENABLE] = cc_handle_tile_telegun_laser_enable;
  g_apGameTileHandlers[TILE_TELE_LASER_DISABLE] = cc_handle_tile_telegun_laser_disable;
  g_apSwitchTileHandlers[TILE_SWITCHOPEN] = cc_handle_switch_open;
  g_apSwitchTileHandlers[TILE_SWITCHTIMEDOPEN] = cc_handle_switch_timed_open;
  g_apSwitchTileHandlers[TILE_SWITCHTIMEDCLOSE] = cc_handle_switch_timed_close;
  g_apSwitchTileHandlers[TILE_SWITCHCLOSE] = cc_handle_switch_close;
  g_apSwitchTileHandlers[TILE_FREEZE] = cc_handle_switch_freeze;
  g_apSwitchTileHandlers[TILE_DFREEZE] = cc_handle_switch_dfreeze;
  g_apSwitchTileHandlers[TILE_DUNFREEZE] = cc_handle_switch_dunfreeze;
  g_apSwitchTileHandlers[TILE_LFREEZE] = cc_handle_switch_lfreeze;
  g_apSwitchTileHandlers[TILE_LUNFREEZE] = cc_handle_switch_lunfreeze;
  g_apSwitchTileHandlers[TILE_HIT_ENABLE] = cc_handle_switch_hit_enable;
  g_apSwitchTileHandlers[TILE_HIT_DISABLE] = cc_handle_switch_hit_disable;
  g_apSwitchTileHandlers[TILE_JUMP] = cc_handle_switch_jump;
  g_apSwitchTileHandlers[TILE_ADD_TIME] = cc_handle_switch_add_time;
  g_apSwitchTileHandlers[TILE_SUBTRACT_TIME] = cc_handle_switch_subtract_time;
}
