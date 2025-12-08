#include "tiles.h"
#include <assert.h>
#include <ddnet_map_loader.h>
#include <ddnet_physics/collision.h>
#include <ddnet_physics/gamecore.h>
#include <ddnet_physics/vmath.h>
#include <float.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NINJA_DURATION 15000
#define NINJA_MOVETIME 200
#define NINJA_VELOCITY 50

#define CLIP(p, q)                                                                                                                                   \
  do {                                                                                                                                               \
    if ((p) == 0.0f) {                                                                                                                               \
      if ((q) < 0.0f)                                                                                                                                \
        break;                                                                                                                                       \
    } else {                                                                                                                                         \
      float r = (q) / (p);                                                                                                                           \
      if ((p) < 0.0f) {                                                                                                                              \
        if (r > t1)                                                                                                                                  \
          break;                                                                                                                                     \
        if (r > t0)                                                                                                                                  \
          t0 = r;                                                                                                                                    \
      } else if ((p) > 0.0f) {                                                                                                                       \
        if (r < t0)                                                                                                                                  \
          break;                                                                                                                                     \
        if (r < t1)                                                                                                                                  \
          t1 = r;                                                                                                                                    \
      }                                                                                                                                              \
    }                                                                                                                                                \
  } while (0)

void init_config(SConfig *pConfig) {
#define MACRO_CONFIG_INT(Name, Def) pConfig->m_##Name = Def;
#include <ddnet_physics/config.h>
#undef MACRO_CONFIG_INT
}

// TODO: implement guns, doors, lasers, lights and draggers

// Physics helper functions {{{

mvec2 clamp_vel(int MoveRestriction, mvec2 Vel) {
  float x = vgetx(Vel);
  float y = vgety(Vel);

  if (x > 0 && (MoveRestriction & CANTMOVE_RIGHT)) {
    Vel = vsetx(Vel, 0);
  } else if (x < 0 && (MoveRestriction & CANTMOVE_LEFT)) {
    Vel = vsetx(Vel, 0);
  }
  if (y > 0 && (MoveRestriction & CANTMOVE_DOWN)) {
    Vel = vsety(Vel, 0);
  } else if (y < 0 && (MoveRestriction & CANTMOVE_UP)) {
    Vel = vsety(Vel, 0);
  }
  return Vel;
}

static inline float saturate_add(float Min, float Max, float Current, float Modifier) {
  if (Modifier < 0) {
    if (Current < Min)
      return Current;
    Current += Modifier;
    if (Current < Min)
      Current = Min;
    return Current;
  } else {
    if (Current > Max)
      return Current;
    Current += Modifier;
    if (Current > Max)
      Current = Max;
    return Current;
  }
}

mvec2 calc_pos(mvec2 Pos, mvec2 Velocity, float Curvature, float Speed, float Time) {
  float n[2] = {vgetx(Pos), vgety(Pos)}, v[2] = {vgetx(Velocity), vgety(Velocity)};
  Time *= Speed;
  n[0] = n[0] + v[0] * Time;
  n[1] = n[1] + v[1] * Time + Curvature / 10000 * (Time * Time);
  return vec2_init(n[0], n[1]);
}

// }}}

// Entities {{{

void ent_init(SEntity *pEnt, SWorldCore *pGameWorld, int ObjType, mvec2 Pos) {
  pEnt->m_pWorld = pGameWorld;
  pEnt->m_ObjType = ObjType;
  pEnt->m_Pos = Pos;
  pEnt->m_pCollision = pGameWorld->m_pCollision;
  pEnt->m_MarkedForDestroy = false;
  pEnt->m_pPrevTypeEntity = NULL;
  pEnt->m_pNextTypeEntity = NULL;
  pEnt->m_Spawned = true;
}

void lsr_bounce(SLaser *pLaser);
void lsr_init(SLaser *pLaser, SWorldCore *pGameWorld, int Type, int Owner, mvec2 Pos, mvec2 Dir, float StartEnergy) {
  memset(pLaser, 0, sizeof(SLaser));
  ent_init(&pLaser->m_Base, pGameWorld, WORLD_ENTTYPE_LASER, Pos);
  pLaser->m_Owner = Owner;
  pLaser->m_Energy = StartEnergy;
  pLaser->m_Dir = Dir;
  pLaser->m_Type = Type;
  pLaser->m_pTuning = &pGameWorld->m_pTunings[is_tune(pGameWorld->m_pCollision, get_map_index(pGameWorld->m_pCollision, Pos))];
  lsr_bounce(pLaser);
}

void cc_unfreeze(SCharacterCore *pCore);
void cc_take_damage(SCharacterCore *pCore, mvec2 Force);
bool lsr_hit_character(SLaser *pLaser, mvec2 From, mvec2 To) {
  mvec2 At;
  SCharacterCore *pOwnerChar = &pLaser->m_Base.m_pWorld->m_pCharacters[pLaser->m_Owner];
  SCharacterCore *pHit;
  bool pDontHitSelf = pLaser->m_Bounces == 0 && !pLaser->m_WasTele;
  if (pOwnerChar ? (!pOwnerChar->m_LaserHitDisabled && pLaser->m_Type == WEAPON_LASER) ||
                       (!pOwnerChar->m_ShotgunHitDisabled && pLaser->m_Type == WEAPON_SHOTGUN)
                 : pLaser->m_Base.m_pWorld->m_pConfig->m_SvHit) {
    pHit = wc_intersect_character(pLaser->m_Base.m_pWorld, pLaser->m_Base.m_Pos, To, 0.f, &At, pDontHitSelf ? pOwnerChar : NULL, NULL);
  } else
    pHit = wc_intersect_character(pLaser->m_Base.m_pWorld, pLaser->m_Base.m_Pos, To, 0.f, &At, pDontHitSelf ? pOwnerChar : NULL, pOwnerChar);

  if (!pHit || (pHit != pOwnerChar && pOwnerChar ? (pOwnerChar->m_LaserHitDisabled && pLaser->m_Type == WEAPON_LASER) ||
                                                       (pOwnerChar->m_ShotgunHitDisabled && pLaser->m_Type == WEAPON_SHOTGUN)
                                                 : !pLaser->m_Base.m_pWorld->m_pConfig->m_SvHit))
    return false;
  pLaser->m_From = From;
  pLaser->m_Base.m_Pos = At;
  pLaser->m_Energy = -1;
  switch (pLaser->m_Type) {
  case WEAPON_SHOTGUN: {
    pHit->m_Vel = vvadd(pHit->m_Vel, vfmul(vnormalize(vvsub(pLaser->m_PrevPos, pHit->m_Pos)), pLaser->m_pTuning->m_ShotgunStrength));
    break;
  }
  case WEAPON_LASER: {
    cc_unfreeze(pHit);
    break;
  }
  default:
    __builtin_unreachable();
    break;
  }
  pHit->m_Vel = clamp_vel(pHit->m_MoveRestrictions, pHit->m_Vel);
  pHit->m_HitNum += 2;
  return true;
}

void lsr_bounce(SLaser *pLaser) {
  pLaser->m_EvalTick = pLaser->m_Base.m_pWorld->m_GameTick;

  if (pLaser->m_Energy < 0) {
    pLaser->m_Base.m_MarkedForDestroy = true;
    return;
  }
  pLaser->m_PrevPos = pLaser->m_Base.m_Pos;
  mvec2 Coltile;

  int Res;
  uint8_t z = 0;

  if (pLaser->m_WasTele) {
    pLaser->m_PrevPos = pLaser->m_TelePos;
    pLaser->m_Base.m_Pos = pLaser->m_TelePos;
    pLaser->m_TelePos = vec2_init(0, 0);
  }

  mvec2 From = pLaser->m_Base.m_Pos;
  mvec2 To = vvadd(pLaser->m_Base.m_Pos, vfmul(pLaser->m_Dir, pLaser->m_Energy));

  // printf("Before: From:%.f,%.f, To:%.f,%.f\n", vgetx(From), vgety(From), vgetx(To), vgety(To));
  float x0 = vgetx(From), y0 = vgety(From);
  float x1 = vgetx(To), y1 = vgety(To);

  float dx = x1 - x0;
  float dy = y1 - y0;

  float W = (float)pLaser->m_Base.m_pCollision->m_MapData.width * 32.0f;
  float H = (float)pLaser->m_Base.m_pCollision->m_MapData.height * 32.0f;

  float xmin = 0.0f, ymin = 0.0f;
  float xmax = W - 1.f, ymax = H - 1.f;

  float t0 = 0.0f, t1 = 1.0f;

  CLIP(-dx, x0 - xmin); // left
  CLIP(dx, xmax - x0);  // right
  CLIP(-dy, y0 - ymin); // top
  CLIP(dy, ymax - y0);  // bottom

  // we only care about moving the end point inside, so use t1
  To = vec2_init(x0 + dx * t1, y0 + dy * t1);

  // printf("After: From:%.f,%.f, To:%.f,%.f\n", vgetx(From), vgety(From), vgetx(To), vgety(To));
  Res =
      intersect_line_tele_weapon(pLaser->m_Base.m_pCollision, From, To, &Coltile, pLaser->m_Base.m_pCollision->m_MapData.tele_layer.type ? &z : NULL);

  if (Res) {
    To = Coltile;
    // printf("From:%.f,%.f, To:%.f,%.f\n", vgetx(From), vgety(From), vgetx(To), vgety(To));
    if (!lsr_hit_character(pLaser, pLaser->m_Base.m_Pos, To)) {
      // intersected
      pLaser->m_From = pLaser->m_Base.m_Pos;
      pLaser->m_Base.m_Pos = To;

      if (Res != TILE_TELEINWEAPON) {
        bool div = false;
        if (check_point(pLaser->m_Base.m_pCollision, vvadd(pLaser->m_Base.m_Pos, vec2_init(0, 1))) ||
            check_point(pLaser->m_Base.m_pCollision, vvadd(pLaser->m_Base.m_Pos, vec2_init(0, -1)))) {
          pLaser->m_Dir = vvmul(pLaser->m_Dir, vec2_init(1, -1));
          div = true;
        }
        if (check_point(pLaser->m_Base.m_pCollision, vvadd(pLaser->m_Base.m_Pos, vec2_init(1, 0))) ||
            check_point(pLaser->m_Base.m_pCollision, vvadd(pLaser->m_Base.m_Pos, vec2_init(-1, 0)))) {
          pLaser->m_Dir = vvmul(pLaser->m_Dir, vec2_init(-1, 1));
          div = true;
        }
        if (!div)
          pLaser->m_Dir = vvmul(pLaser->m_Dir, vec2_init(-1, -1));
      }

      const float Distance = vdistance(pLaser->m_From, pLaser->m_Base.m_Pos);
      // Prevent infinite bounces
      if (Distance == 0.0f && pLaser->m_ZeroEnergyBounceInLastTick) {
        pLaser->m_Energy = -1;
      } else
        pLaser->m_Energy -= Distance + pLaser->m_pTuning->m_LaserBounceCost;
      pLaser->m_ZeroEnergyBounceInLastTick = Distance == 0.0f;

      int NumTeles = pLaser->m_Base.m_pCollision->m_aNumTeleOuts[z];
      if (Res == TILE_TELEINWEAPON && NumTeles) {
        pLaser->m_TelePos = pLaser->m_Base.m_pCollision->m_apTeleOuts[z][pLaser->m_Base.m_pWorld->m_GameTick % NumTeles];
        pLaser->m_WasTele = true;
      } else {
        pLaser->m_Bounces++;
        pLaser->m_WasTele = false;
      }
      if (pLaser->m_Bounces > pLaser->m_pTuning->m_LaserBounceNum)
        pLaser->m_Energy = -1;
    }
  } else {
    if (!lsr_hit_character(pLaser, pLaser->m_Base.m_Pos, To)) {
      pLaser->m_From = pLaser->m_Base.m_Pos;
      pLaser->m_Base.m_Pos = To;
      pLaser->m_Energy = -1;
    }
  }

  SCharacterCore *pOwnerChar = &pLaser->m_Base.m_pWorld->m_pCharacters[pLaser->m_Owner];
  if (pLaser->m_Energy <= 0 && !pLaser->m_TeleportCancelled && pOwnerChar && pOwnerChar->m_HasTelegunLaser && pLaser->m_Type == WEAPON_LASER) {
    mvec2 PossiblePos;
    bool Found = false;

    // Check if the laser hits a player.
    bool pDontHitSelf = (pLaser->m_Bounces == 0 && !pLaser->m_WasTele);
    mvec2 At;
    SCharacterCore *pHit;
    if (pOwnerChar ? (!pOwnerChar->m_LaserHitDisabled && pLaser->m_Type == WEAPON_LASER) : pLaser->m_Base.m_pWorld->m_pConfig->m_SvHit)
      pHit = wc_intersect_character(pLaser->m_Base.m_pWorld, pLaser->m_Base.m_Pos, To, 0.f, &At, pDontHitSelf ? pOwnerChar : NULL, NULL);
    else
      pHit = wc_intersect_character(pLaser->m_Base.m_pWorld, pLaser->m_Base.m_Pos, To, 0.f, &At, pDontHitSelf ? pOwnerChar : NULL, pOwnerChar);

    if (pHit)
      Found = get_nearest_air_pos_player(pLaser->m_Base.m_pCollision, pHit->m_Pos, &PossiblePos);
    else
      Found = get_nearest_air_pos(pLaser->m_Base.m_pCollision, pLaser->m_Base.m_Pos, pLaser->m_From, &PossiblePos);

    if (Found) {
      pOwnerChar->m_TeleGunPos = PossiblePos;
      pOwnerChar->m_TeleGunTeleport = true;
      pOwnerChar->m_IsBlueTeleGunTeleport = pLaser->m_IsBlueTeleport;
    }
  } else if (pLaser->m_Owner >= 0) {
    int MapIndex = get_pure_map_index(pLaser->m_Base.m_pCollision, Coltile);
    int TileFIndex = pLaser->m_Base.m_pCollision->m_MapData.front_layer.data ? get_front_tile_index(pLaser->m_Base.m_pCollision, MapIndex) : 0;
    bool IsSwitchTeleGun =
        pLaser->m_Base.m_pCollision->m_MapData.switch_layer.type ? get_switch_type(pLaser->m_Base.m_pCollision, MapIndex) == TILE_ALLOW_TELE_GUN : 0;
    bool IsBlueSwitchTeleGun = pLaser->m_Base.m_pCollision->m_MapData.switch_layer.type
                                   ? get_switch_type(pLaser->m_Base.m_pCollision, MapIndex) == TILE_ALLOW_BLUE_TELE_GUN
                                   : 0;
    int IsTeleInWeapon = pLaser->m_Base.m_pCollision->m_MapData.tele_layer.type ? is_teleport_weapon(pLaser->m_Base.m_pCollision, MapIndex) : 0;

    if (!IsTeleInWeapon) {
      if (IsSwitchTeleGun || IsBlueSwitchTeleGun) {
        // Delay specifies which weapon the tile should work for.
        // Delay = 0 means all.
        int delay = get_switch_delay(pLaser->m_Base.m_pCollision, MapIndex);

        if ((delay != 3 && delay != 0) && pLaser->m_Type == WEAPON_LASER) {
          IsSwitchTeleGun = IsBlueSwitchTeleGun = false;
        }
      }

      pLaser->m_IsBlueTeleport = TileFIndex == TILE_ALLOW_BLUE_TELE_GUN || IsBlueSwitchTeleGun;

      // Teleport is canceled if the last bounce tile is not a TILE_ALLOW_TELE_GUN.
      // Teleport also works if laser didn't bounce.
      pLaser->m_TeleportCancelled = pLaser->m_Type == WEAPON_LASER && (TileFIndex != TILE_ALLOW_TELE_GUN && TileFIndex != TILE_ALLOW_BLUE_TELE_GUN &&
                                                                       !IsSwitchTeleGun && !IsBlueSwitchTeleGun);
    }
  }
}

void lsr_tick(SLaser *pLaser) {
  if ((pLaser->m_Base.m_pWorld->m_GameTick - pLaser->m_EvalTick) > (GAME_TICK_SPEED * pLaser->m_pTuning->m_LaserBounceDelay / 1000.0f))
    lsr_bounce(pLaser);
  pLaser->m_Base.m_Spawned = false;
}

void prj_init(SProjectile *pProj, SWorldCore *pGameWorld, int Type, int Owner, mvec2 Pos, mvec2 Dir, int Span, bool Freeze, bool Explosive, int Layer,
              int Number) {
  memset(pProj, 0, sizeof(SProjectile));
  ent_init(&pProj->m_Base, pGameWorld, WORLD_ENTTYPE_PROJECTILE, Pos);
  pProj->m_Type = Type;
  pProj->m_Direction = Dir;
  pProj->m_LifeSpan = Span;
  pProj->m_Owner = Owner;
  pProj->m_StartTick = pGameWorld->m_GameTick;
  pProj->m_Explosive = Explosive;
  pProj->m_Base.m_Layer = Layer;
  pProj->m_Base.m_Number = Number;
  pProj->m_Freeze = Freeze;
  pProj->m_pTuning = &pGameWorld->m_pTunings[is_tune(pGameWorld->m_pCollision, get_map_index(pGameWorld->m_pCollision, Pos))];
  if (Owner > 0)
    pProj->m_IsSolo = pGameWorld->m_pCharacters[Owner].m_Solo;
}

mvec2 prj_get_pos(SProjectile *pProj, float Time) {
  float Curvature = 0;
  float Speed = 0;

  switch (pProj->m_Type) {
  case WEAPON_GRENADE:
    Curvature = pProj->m_pTuning->m_GrenadeCurvature;
    Speed = pProj->m_pTuning->m_GrenadeSpeed;
    break;

  case WEAPON_SHOTGUN:
    Curvature = pProj->m_pTuning->m_ShotgunCurvature;
    Speed = pProj->m_pTuning->m_ShotgunSpeed;
    break;

  case WEAPON_GUN:
    Curvature = pProj->m_pTuning->m_GunCurvature;
    Speed = pProj->m_pTuning->m_GunSpeed;
    break;
  }

  return calc_pos(pProj->m_Base.m_Pos, pProj->m_Direction, Curvature, Speed, Time);
}

bool cc_freeze(SCharacterCore *pCore, int Seconds);

void wc_create_explosion(SWorldCore *pWorld, mvec2 Pos, int Owner);

void prj_tick(SProjectile *pProj) {
  pProj->m_Base.m_Spawned = false;
  float Pt = (pProj->m_Base.m_pWorld->m_GameTick - pProj->m_StartTick - 1) / (float)GAME_TICK_SPEED;
  float Ct = (pProj->m_Base.m_pWorld->m_GameTick - pProj->m_StartTick) / (float)GAME_TICK_SPEED;
  mvec2 PrevPos = prj_get_pos(pProj, Pt);
  mvec2 CurPos = prj_get_pos(pProj, Ct);
  mvec2 ColPos;
  mvec2 NewPos;
  if (vgetx(CurPos) < 0 || vgety(CurPos) < 0 || (int)(vgetx(CurPos) + 0.5) >> 5 >= pProj->m_Base.m_pCollision->m_MapData.width ||
      (int)(vgety(CurPos) + 0.5) >> 5 >= pProj->m_Base.m_pCollision->m_MapData.height) {
    pProj->m_Base.m_MarkedForDestroy = true;
    return;
  }
  bool Collide = intersect_line(pProj->m_Base.m_pCollision, PrevPos, CurPos, &ColPos, &NewPos);
  SCharacterCore *pOwnerChar = NULL;

  if (pProj->m_Owner >= 0)
    pOwnerChar = &pProj->m_Base.m_pWorld->m_pCharacters[pProj->m_Owner];

  SCharacterCore *pTargetChr = NULL;

  if (pOwnerChar ? !pOwnerChar->m_GrenadeHitDisabled : pProj->m_Base.m_pWorld->m_pConfig->m_SvHit)
    pTargetChr = wc_intersect_character(pProj->m_Base.m_pWorld, PrevPos, ColPos, 6.0f, &ColPos, pOwnerChar, NULL);

  if (pProj->m_LifeSpan > -1)
    pProj->m_LifeSpan--;

  if (Collide || (pTargetChr && (pOwnerChar ? !pOwnerChar->m_GrenadeHitDisabled
                                            : pProj->m_Base.m_pWorld->m_pConfig->m_SvHit || pProj->m_Owner == -1 || pTargetChr == pOwnerChar))) {
    if (pProj->m_Explosive && (!pTargetChr || (pTargetChr && (pProj->m_Type == WEAPON_SHOTGUN && Collide)))) {
      wc_create_explosion(pProj->m_Base.m_pWorld, ColPos, pProj->m_Owner);
    } else if (pProj->m_Freeze) {
      for (int i = 0; i < pProj->m_Base.m_pWorld->m_NumCharacters; ++i) {
        SCharacterCore *pChr = &pProj->m_Base.m_pWorld->m_pCharacters[i];
        if (vdistance(CurPos, pChr->m_Pos) >= 1.f + PHYSICALSIZE)
          continue;
        if (pChr && (pProj->m_Base.m_Layer != LAYER_SWITCH || (pProj->m_Base.m_Layer == LAYER_SWITCH && pProj->m_Base.m_Number > 0 &&
                                                               pProj->m_Base.m_pWorld->m_pSwitches[pProj->m_Base.m_Number].m_Status)))
          cc_freeze(pChr, pProj->m_Base.m_pWorld->m_pConfig->m_SvFreezeDelay);
      }
    } else if (pTargetChr)
      pTargetChr->m_Vel = clamp_vel(pTargetChr->m_MoveRestrictions, pTargetChr->m_Vel);

    if (pOwnerChar &&
        ((pProj->m_Type == WEAPON_GRENADE && pOwnerChar->m_HasTelegunGrenade) || (pProj->m_Type == WEAPON_GUN && pOwnerChar->m_HasTelegunGun))) {
      int MapIndex = get_pure_map_index(pProj->m_Base.m_pCollision, pTargetChr ? pTargetChr->m_Pos : ColPos);
      int TileFIndex = pProj->m_Base.m_pCollision->m_MapData.front_layer.data ? get_front_tile_index(pProj->m_Base.m_pCollision, MapIndex) : 0;
      bool IsSwitchTeleGun = false;
      bool IsBlueSwitchTeleGun = false;
      if (pProj->m_Base.m_pCollision->m_MapData.switch_layer.type) {
        IsSwitchTeleGun = get_switch_type(pProj->m_Base.m_pCollision, MapIndex) == TILE_ALLOW_TELE_GUN;
        IsBlueSwitchTeleGun = get_switch_type(pProj->m_Base.m_pCollision, MapIndex) == TILE_ALLOW_BLUE_TELE_GUN;
      }

      if (IsSwitchTeleGun || IsBlueSwitchTeleGun) {
        int delay = get_switch_delay(pProj->m_Base.m_pCollision, MapIndex);

        if (delay == 1 && pProj->m_Type != WEAPON_GUN)
          IsSwitchTeleGun = IsBlueSwitchTeleGun = false;
        if (delay == 2 && pProj->m_Type != WEAPON_GRENADE)
          IsSwitchTeleGun = IsBlueSwitchTeleGun = false;
        if (delay == 3 && pProj->m_Type != WEAPON_LASER)
          IsSwitchTeleGun = IsBlueSwitchTeleGun = false;
      }

      if (TileFIndex == TILE_ALLOW_TELE_GUN || TileFIndex == TILE_ALLOW_BLUE_TELE_GUN || IsSwitchTeleGun || IsBlueSwitchTeleGun || pTargetChr) {
        bool Found;
        mvec2 PossiblePos;

        if (!Collide)
          Found = get_nearest_air_pos_player(pProj->m_Base.m_pCollision, pTargetChr ? pTargetChr->m_Pos : ColPos, &PossiblePos);
        else
          Found = get_nearest_air_pos(pProj->m_Base.m_pCollision, NewPos, CurPos, &PossiblePos);

        if (Found) {
          pOwnerChar->m_TeleGunPos = PossiblePos;
          pOwnerChar->m_TeleGunTeleport = true;
          pOwnerChar->m_IsBlueTeleGunTeleport = TileFIndex == TILE_ALLOW_BLUE_TELE_GUN || IsBlueSwitchTeleGun;
        }
      }
    }

    if (Collide && pProj->m_Bouncing != 0) {
      pProj->m_StartTick = pProj->m_Base.m_pWorld->m_GameTick;
      pProj->m_Base.m_Pos = vvadd(NewPos, vfmul(pProj->m_Direction, -4));
      if (pProj->m_Bouncing == 1)
        pProj->m_Direction = vsetx(pProj->m_Direction, -vgetx(pProj->m_Direction));
      else if (pProj->m_Bouncing == 2)
        pProj->m_Direction = vsety(pProj->m_Direction, -vgety(pProj->m_Direction));
      if (fabs(vgetx(pProj->m_Direction)) < 1e-6f)
        pProj->m_Direction = vsetx(pProj->m_Direction, 0);
      if (fabs(vgety(pProj->m_Direction)) < 1e-6f)
        pProj->m_Direction = vsety(pProj->m_Direction, 0);
      pProj->m_Base.m_Pos = vvadd(pProj->m_Base.m_Pos, pProj->m_Direction);
    } else if (pProj->m_Type == WEAPON_GUN) {
      pProj->m_Base.m_MarkedForDestroy = true;
      return;
    }
    if (Collide && !pProj->m_Bouncing && !pProj->m_Freeze) {
      pProj->m_Base.m_MarkedForDestroy = true;
      return;
    }
  }
  if (pProj->m_LifeSpan == -1) {
    if (pProj->m_Explosive) {
      wc_create_explosion(pProj->m_Base.m_pWorld, ColPos, pProj->m_Owner);
    }
    pProj->m_Base.m_MarkedForDestroy = true;
    return;
  }

  if (!pProj->m_Base.m_pCollision->m_MapData.tele_layer.type)
    return;
  int x = get_index(pProj->m_Base.m_pCollision, PrevPos, CurPos);
  int z = is_teleport_weapon(pProj->m_Base.m_pCollision, x);
  int Num = pProj->m_Base.m_pCollision->m_aNumTeleOuts[z];
  if (z && Num > 0) {
    pProj->m_Base.m_Pos = pProj->m_Base.m_pCollision->m_apTeleOuts[z][pProj->m_Base.m_pWorld->m_GameTick % Num];
    pProj->m_StartTick = pProj->m_Base.m_pWorld->m_GameTick;
  }
}

void cc_calc_indices(SCharacterCore *pCore) {
  const int x = ((int)vgetx(pCore->m_Pos) >> 5);
  const int y = ((int)vgety(pCore->m_Pos) >> 5);
  pCore->m_BlockPos.x = x;
  pCore->m_BlockPos.y = y;
  pCore->m_BlockIdx = y * pCore->m_pCollision->m_MapData.width + x;
}

void cc_do_pickup(SCharacterCore *pCore) {
  if (!(pCore->m_pCollision->m_pTileInfos[pCore->m_BlockIdx] & INFO_PICKUPNEXT))
    return;

  const int Width = pCore->m_pCollision->m_MapData.width;
  const int Height = pCore->m_pCollision->m_MapData.height;
  const int ix = pCore->m_BlockPos.x;
  const int iy = pCore->m_BlockPos.y;

  // TODO: Getting the memory could be done in parralel/non-sequential and clamps could be removed
  for (int dy = -1; dy <= 1; ++dy) {
    const int Idx = pCore->m_pCollision->m_pWidthLookup[iclamp(iy + dy, 0, Height - 1)];
    for (int dx = -1; dx <= 1; ++dx) {
      for (int i = 0; i < 2; ++i) {
        // NOTE: doing a copy here should be faster since it is only 3 bytes
        const SPickup Pickup = !i ? pCore->m_pCollision->m_pPickups[Idx + iclamp(ix + dx, 0, Width - 1)]
                                  : pCore->m_pCollision->m_pFrontPickups[Idx + iclamp(ix + dx, 0, Width - 1)];
        if (Pickup.m_Type < 0)
          continue;
        if (vdistance(pCore->m_Pos, vec2_init(((ix + dx) * 32) + 16, ((iy + dy) * 32) + 16)) >= 48)
          continue;
        if (Pickup.m_Number > 0 && !pCore->m_pWorld->m_pSwitches[Pickup.m_Number].m_Status)
          continue;

        switch (Pickup.m_Type) {
        case POWERUP_HEALTH:
          cc_freeze(pCore, pCore->m_pWorld->m_pConfig->m_SvFreezeDelay);
          break;

        case POWERUP_ARMOR:
#pragma clang loop unroll(full)
          for (int j = WEAPON_SHOTGUN; j < NUM_WEAPONS; j++) {
            pCore->m_aWeaponGot[j] = false;
          }
          pCore->m_Ninja.m_ActivationDir = vec2_init(0, 0);
          pCore->m_Ninja.m_ActivationTick = -500;
          pCore->m_Ninja.m_CurrentMoveTime = 0;
          if (pCore->m_ActiveWeapon >= WEAPON_SHOTGUN)
            pCore->m_ActiveWeapon = WEAPON_HAMMER;
          break;

        case POWERUP_ARMOR_SHOTGUN:
          pCore->m_aWeaponGot[WEAPON_SHOTGUN] = false;
          if (pCore->m_ActiveWeapon == WEAPON_SHOTGUN)
            pCore->m_ActiveWeapon = WEAPON_HAMMER;
          break;

        case POWERUP_ARMOR_GRENADE:
          pCore->m_aWeaponGot[WEAPON_GRENADE] = false;
          if (pCore->m_ActiveWeapon == WEAPON_GRENADE)
            pCore->m_ActiveWeapon = WEAPON_HAMMER;
          break;

        case POWERUP_ARMOR_NINJA:
          pCore->m_Ninja.m_ActivationDir = vec2_init(0, 0);
          pCore->m_Ninja.m_ActivationTick = -500;
          pCore->m_Ninja.m_CurrentMoveTime = 0;
          break;

        case POWERUP_ARMOR_LASER:
          pCore->m_aWeaponGot[WEAPON_LASER] = false;
          if (pCore->m_ActiveWeapon == WEAPON_LASER)
            pCore->m_ActiveWeapon = WEAPON_HAMMER;
          break;

        case POWERUP_WEAPON:
          pCore->m_aWeaponGot[Pickup.m_Subtype] = true;
          break;

        case POWERUP_NINJA: {
          pCore->m_Ninja.m_ActivationTick = pCore->m_pWorld->m_GameTick;
          pCore->m_aWeaponGot[WEAPON_NINJA] = true;
          pCore->m_ActiveWeapon = WEAPON_NINJA;
          break;
        }
        default:
          __builtin_unreachable();
        }
      }
    }
  }
}

// }}}

// CharacterCore functions {{{

bool wc_next_spawn(SWorldCore *pCore, mvec2 *pOutPos, int Id);

void cc_init(SCharacterCore *pCore, SWorldCore *pWorld) {
  memset(pCore, 0, sizeof(SCharacterCore));
  pCore->m_HookedPlayer = -1;
  pCore->m_Jumps = 2;
  pCore->m_pWorld = pWorld;
  pCore->m_pCollision = pWorld->m_pCollision;
  pCore->m_pTuning = &pWorld->m_pTunings[0];
  pCore->m_aWeaponGot[0] = true;
  pCore->m_aWeaponGot[1] = true;
  pCore->m_ActiveWeapon = WEAPON_GUN;
  pCore->m_Input.m_TargetY = -1;

  pCore->m_StartTick = -1;
  pCore->m_FinishTick = -1;

  pCore->m_AttackTick = INT_MIN * 0.5;

  // The world assigns ids to the core
  pCore->m_Id = -1;
}

void cc_set_worldcore(SCharacterCore *pCore, SWorldCore *pWorld, SCollision *pCollision) {
  pCore->m_pWorld = pWorld;
  pCore->m_pCollision = pCollision;
}

void cc_unfreeze(SCharacterCore *pCore) {
  if (pCore->m_FreezeTime <= 0)
    return;
  if (!pCore->m_aWeaponGot[pCore->m_ActiveWeapon])
    pCore->m_ActiveWeapon = WEAPON_GUN;
  pCore->m_FreezeTime = 0;
  pCore->m_FrozenLastTick = true;
}

bool is_switch_active_cb(int Number, void *pUser) {
  SCharacterCore *pThis = (SCharacterCore *)pUser;
  return pThis->m_pWorld->m_pSwitches && pThis->m_pWorld->m_pSwitches[Number].m_Status;
}

void cc_quantize(SCharacterCore *pCore) {
  // Common constants
  __m128 half = _mm_set1_ps(0.5f);
  __m128 zero = _mm_setzero_ps();
  __m128 scale = _mm_set1_ps(256.0f);

  // Quantize m_Pos
  __m128 pos = pCore->m_Pos;
  __m128 pos_plus_half = _mm_add_ps(pos, half);
  __m128i pos_int = _mm_cvttps_epi32(pos_plus_half);
  __m128 pos_rounded = _mm_cvtepi32_ps(pos_int);
  pCore->m_Pos = pos_rounded;

  // Quantize m_HookPos
  __m128 hook_pos = pCore->m_HookPos;
  __m128 hook_pos_plus_half = _mm_add_ps(hook_pos, half);
  __m128i hook_pos_int = _mm_cvttps_epi32(hook_pos_plus_half);
  __m128 hook_pos_rounded = _mm_cvtepi32_ps(hook_pos_int);
  pCore->m_HookPos = hook_pos_rounded;

  // Quantize m_Vel
  __m128 vel = pCore->m_Vel;
  __m128 vel_scaled = _mm_mul_ps(vel, scale);
  __m128 adjusted_vel = _mm_blendv_ps(_mm_sub_ps(vel_scaled, half), _mm_add_ps(vel_scaled, half), _mm_cmpge_ps(vel_scaled, zero));
  __m128i vel_int = _mm_cvttps_epi32(adjusted_vel);
  __m128 vel_rounded = _mm_cvtepi32_ps(vel_int);
  pCore->m_Vel = _mm_div_ps(vel_rounded, scale);

  // Quantize m_HookDir
  __m128 hook_dir = pCore->m_HookDir;
  __m128 hook_dir_scaled = _mm_mul_ps(hook_dir, scale);
  __m128 adjusted_hook_dir = _mm_blendv_ps(_mm_sub_ps(hook_dir_scaled, half), _mm_add_ps(hook_dir_scaled, half), _mm_cmpge_ps(hook_dir_scaled, zero));
  __m128i hook_dir_int = _mm_cvttps_epi32(adjusted_hook_dir);
  __m128 hook_dir_rounded = _mm_cvtepi32_ps(hook_dir_int);
  pCore->m_HookDir = _mm_div_ps(hook_dir_rounded, scale);

  cc_calc_indices(pCore);
}

void cc_die(SCharacterCore *pCore) {
  int Id = pCore->m_Id;
  cc_init(pCore, pCore->m_pWorld);

  mvec2 SpawnPos;
  if (wc_next_spawn(pCore->m_pWorld, &SpawnPos, Id)) {
    pCore->m_Pos = SpawnPos;
    pCore->m_PrevPos = SpawnPos;
    cc_calc_indices(pCore);
  }

  pCore->m_RespawnDelay = 25;
  pCore->m_Id = Id;
}

static inline float fast_expf(float x) {
  union {
    float f;
    int i;
  } v;
  v.i = (int)(x * (1 << 23) / 0.69314718f + 127 * (1 << 23));
  return v.f;
}

void cc_move(SCharacterCore *pCore) {
  pCore->m_VelMag = vlength(pCore->m_Vel);
  const float VelMag = pCore->m_VelMag * 50;
  float OldVel = vgetx(pCore->m_Vel);

  float RampValue = 1.f;
  if (VelMag >= pCore->m_pTuning->m_VelrampStart) {
    float t = VelMag - pCore->m_pTuning->m_VelrampStart;
    RampValue = fast_expf(-t * pCore->m_pTuning->m_VelrampValue);
  }
  pCore->m_VelRamp = RampValue;

  OldVel = OldVel * RampValue;
  pCore->m_Vel = vsetx(pCore->m_Vel, OldVel);

  mvec2 NewPos = pCore->m_Pos;
  bool Grounded = false;
  mvec2 MaxNewPos = vvadd(NewPos, pCore->m_Vel);

  // OOB of the map
  if (vgetx(MaxNewPos) < HALFPHYSICALSIZE + 2 || vgety(MaxNewPos) < HALFPHYSICALSIZE + 2 ||
      vgetx(MaxNewPos) >= (float)pCore->m_pCollision->m_MapData.width * 32.f - (HALFPHYSICALSIZE + 2) ||
      vgety(MaxNewPos) >= (float)pCore->m_pCollision->m_MapData.height * 32.f - (HALFPHYSICALSIZE + 2)) {
    cc_die(pCore);
    return;
  }

  pCore->m_Vel = vvclamp(pCore->m_Vel, vec2_init(-4 * 32, -4 * 32), vec2_init(4 * 32, 4 * 32));

  move_box(pCore->m_pCollision, NewPos, pCore->m_Vel, &NewPos, &pCore->m_Vel,
           vec2_init(pCore->m_pTuning->m_GroundElasticityX, pCore->m_pTuning->m_GroundElasticityY), &Grounded);

  if (Grounded) {
    pCore->m_Jumped &= ~2;
    pCore->m_JumpedTotal = 0;
  }

  pCore->m_Colliding = 0;
  const float velX = vgetx(pCore->m_Vel);
  if (velX < 0.001f && velX > -0.001f) {
    if (OldVel > 0)
      pCore->m_Colliding = 1;
    else if (OldVel < 0)
      pCore->m_Colliding = 2;
  } else
    pCore->m_LeftWall = true;

  if (RampValue != 1.f)
    pCore->m_Vel = vsetx(pCore->m_Vel, velX * (1.f / RampValue));

  pCore->m_Pos = NewPos;
  cc_calc_indices(pCore);

  pCore->m_MoveRestrictions = get_move_restrictions(pCore->m_pCollision, pCore, pCore->m_Pos, pCore->m_BlockIdx);
}

void cc_world_tick_deferred(SCharacterCore *pCore) {
  cc_move(pCore);
  cc_quantize(pCore);
}

static inline float fast_rand(unsigned int *state) {
  unsigned int x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return (x % 1000) / 1000.0f;
}
// static inline unsigned int ifast_rand(unsigned int *state) {
//   unsigned int x = *state;
//   x ^= x << 13;
//   x ^= x >> 17;
//   x ^= x << 5;
//   *state = x;
//   return x;
// }

void cc_tee_interact_deferred(SCharacterCore *pCore, int Id, int *pCollisions) {
  SCharacterCore *pCharCore = &pCore->m_pWorld->m_pCharacters[Id];
  mvec2 Pos = pCharCore->m_Pos;
  bool Solo = pCharCore->m_Solo;
  bool CollisionDisabled = pCharCore->m_CollisionDisabled;

  if (pCore->m_Solo || Solo)
    return;
  float Distance = vdistance(pCore->m_Pos, Pos);
  if (Distance > 0) {
    mvec2 Dir = vnormalize(vvsub(pCore->m_Pos, Pos));
    bool CanCollide = (!pCore->m_CollisionDisabled && !CollisionDisabled && pCore->m_pTuning->m_PlayerCollision);

    if (CanCollide && Distance < PHYSICALSIZE * 1.25f) {
      float a = (PHYSICALSIZE * 1.45f - Distance);
      float Velocity = 0.5f;

      if (vlength(pCore->m_Vel) > 0.0001f)
        Velocity = 1 - (vdot(vnormalize_nomask(pCore->m_Vel), Dir) + 1) / 2;

      pCore->m_Vel = vvadd(pCore->m_Vel, vfmul(Dir, a * (Velocity * 0.75f * 0.85f)));
      ++*pCollisions;
    }
  } else {
    unsigned int seed = (uint32_t)((uint32_t)pCore->m_Id + (uint32_t)Id * 0x1234567) ^ (uint32_t)pCore->m_pWorld->m_GameTick;
    pCore->m_Vel = vvadd(pCore->m_Vel, vec2_init((fast_rand(&seed) - fast_rand(&seed)) * 0.5f, (fast_rand(&seed) - fast_rand(&seed)) * 0.5f));
  }
}

void cc_tick_deferred(SCharacterCore *pCore) {
  if (pCore->m_pWorld->m_NumCharacters > 1) {
    int Num = 0;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        // TODO: use the block idx +- 1 +- map_width
        int Idx = (((int)vgety(pCore->m_Pos) >> 5) + dx) * pCore->m_pCollision->m_MapData.width + (((int)vgetx(pCore->m_Pos) >> 5) + dy);
        Idx = iclamp(Idx, 0, pCore->m_pCollision->m_MapData.width * pCore->m_pCollision->m_MapData.height - 1);
        int Id = pCore->m_pWorld->m_Accelerator.m_pGrid->m_pTeeGrid[Idx];
        while (Id >= 0) {
          if (pCore->m_Id == Id) {
            Id = pCore->m_pWorld->m_Accelerator.m_pTeeList[Id].m_Child;
            continue;
          }
          cc_tee_interact_deferred(pCore, Id, &Num);
          Id = pCore->m_pWorld->m_Accelerator.m_pTeeList[Id].m_Child;
          if (Num > 8)
            goto EndCollisions;
        }
      }
    }
    // pCore->m_Vel = vvclamp(pCore->m_Vel, vec2_init(-1, -1), vec2_init(1, 1));
  }
EndCollisions:
  // player hooking logic
  if (pCore->m_pWorld->m_NumCharacters > 1 && pCore->m_HookedPlayer >= 0) {
    SCharacterCore *pCharCore = &pCore->m_pWorld->m_pCharacters[pCore->m_HookedPlayer];
    float Distance = vdistance(pCore->m_Pos, pCharCore->m_Pos);
    mvec2 Dir = vnormalize(vvsub(pCore->m_Pos, pCharCore->m_Pos));
    if (!pCore->m_HookHitDisabled && pCore->m_pTuning->m_PlayerHooking) {
      if (Distance > PHYSICALSIZE * 1.50f) {
        float HookAccel = pCore->m_pTuning->m_HookDragAccel * (Distance / pCore->m_pTuning->m_HookLength);
        float DragSpeed = pCore->m_pTuning->m_HookDragSpeed;

        mvec2 Temp;
        Temp = clamp_vel(pCharCore->m_MoveRestrictions,
                         vec2_init(saturate_add(-DragSpeed, DragSpeed, vgetx(pCharCore->m_Vel), HookAccel * vgetx(Dir) * 1.5f),
                                   saturate_add(-DragSpeed, DragSpeed, vgety(pCharCore->m_Vel), HookAccel * vgety(Dir) * 1.5f)));
        pCharCore->m_Vel = Temp;

        Temp = clamp_vel(pCore->m_MoveRestrictions,
                         vec2_init(saturate_add(-DragSpeed, DragSpeed, vgetx(pCore->m_Vel), -HookAccel * vgetx(Dir) * 0.25f),
                                   saturate_add(-DragSpeed, DragSpeed, vgety(pCore->m_Vel), -HookAccel * vgety(Dir) * 0.25f)));
        pCore->m_Vel = Temp;
      }
    }
  }
  if (pCore->m_HookState != HOOK_FLYING) {
    pCore->m_NewHook = false;
  }
}

void cc_ddracetick(SCharacterCore *pCore) {
  if (pCore->m_LiveFrozen) {
    pCore->m_Input.m_Direction = 0;
    pCore->m_Input.m_Jump = 0;
  }
  if (pCore->m_FreezeTime > 0) {
    pCore->m_FreezeTime--;
    pCore->m_Input.m_Direction = 0;
    pCore->m_Input.m_Jump = 0;
    pCore->m_Input.m_Hook = 0;
    if (pCore->m_FreezeTime == 1)
      cc_unfreeze(pCore);
  }

  pCore->m_pTuning = &pCore->m_pWorld->m_pTunings[is_tune(pCore->m_pCollision, pCore->m_BlockIdx)];
}

void cc_handle_skippable_tiles(SCharacterCore *pCore, int Index) {
  static const mvec2 DeathOffset1 = {DEATH, -DEATH, 0.f, 0.f};
  static const mvec2 DeathOffset2 = {DEATH, DEATH, 0.f, 0.f};
  static const mvec2 DeathOffset3 = {-DEATH, -DEATH, 0.f, 0.f};
  static const mvec2 DeathOffset4 = {-DEATH, DEATH, 0.f, 0.f};
  if (pCore->m_pCollision->m_pTileInfos[Index] & INFO_CANHITKILL &&
      (get_collision_at(pCore->m_pCollision, vvadd(pCore->m_Pos, DeathOffset1)) == TILE_DEATH ||
       get_collision_at(pCore->m_pCollision, vvadd(pCore->m_Pos, DeathOffset2)) == TILE_DEATH ||
       get_collision_at(pCore->m_pCollision, vvadd(pCore->m_Pos, DeathOffset3)) == TILE_DEATH ||
       get_collision_at(pCore->m_pCollision, vvadd(pCore->m_Pos, DeathOffset4)) == TILE_DEATH ||
       (pCore->m_pCollision->m_MapData.front_layer.data &&
        (get_front_collision_at(pCore->m_pCollision, vvadd(pCore->m_Pos, DeathOffset1)) == TILE_DEATH ||
         get_front_collision_at(pCore->m_pCollision, vvadd(pCore->m_Pos, DeathOffset2)) == TILE_DEATH ||
         get_front_collision_at(pCore->m_pCollision, vvadd(pCore->m_Pos, DeathOffset3)) == TILE_DEATH ||
         get_front_collision_at(pCore->m_pCollision, vvadd(pCore->m_Pos, DeathOffset4)) == TILE_DEATH)))) {
    cc_die(pCore);
    return;
  }

  if (Index < 0)
    return;

  if (is_speedup(pCore->m_pCollision, Index)) {
    mvec2 Direction, TempVel = pCore->m_Vel;
    int Force, Type, MaxSpeed = 0;
    get_speedup(pCore->m_pCollision, Index, &Direction, &Force, &MaxSpeed, &Type);

    if (Type == TILE_SPEED_BOOST_OLD) {
      float TeeAngle, SpeederAngle, DiffAngle, SpeedLeft, TeeSpeed;
      if (Force == 255 && MaxSpeed) {
        pCore->m_Vel = vfmul(Direction, ((float)MaxSpeed / 5.f));
      } else {
        if (MaxSpeed > 0 && MaxSpeed < 5)
          MaxSpeed = 5;
        if (MaxSpeed > 0) {
          float dirX = vgetx(Direction);
          float dirY = vgety(Direction);
          float tempVelX = vgetx(TempVel);
          float tempVelY = vgety(TempVel);

          if (dirX > 0.0000001f)
            SpeederAngle = -atanf(dirY / dirX);
          else if (dirX < 0.0000001f)
            SpeederAngle = atanf(dirY / dirX) + 2.0f * asinf(1.0f);
          else if (dirY > 0.0000001f)
            SpeederAngle = asinf(1.0f);
          else
            SpeederAngle = asinf(-1.0f);

          if (SpeederAngle < 0)
            SpeederAngle = 4.0f * asinf(1.0f) + SpeederAngle;

          if (tempVelX > 0.0000001f)
            TeeAngle = -atanf(tempVelY / tempVelX);
          else if (tempVelX < 0.0000001f)
            TeeAngle = atanf(tempVelY / tempVelX) + 2.0f * asinf(1.0f);
          else if (tempVelY > 0.0000001f)
            TeeAngle = asinf(1.0f);
          else
            TeeAngle = asinf(-1.0f);

          if (TeeAngle < 0)
            TeeAngle = 4.0f * asinf(1.0f) + TeeAngle;

          TeeSpeed = sqrtf(powf(tempVelX, 2) + powf(tempVelY, 2));
          DiffAngle = SpeederAngle - TeeAngle;
          SpeedLeft = MaxSpeed / 5.0f - cosf(DiffAngle) * TeeSpeed;
          if (abs((int)SpeedLeft) > Force && SpeedLeft > 0.0000001f)
            TempVel = vvadd(TempVel, vfmul(Direction, Force));
          else if (abs((int)SpeedLeft) > Force)
            TempVel = vvadd(TempVel, vfmul(Direction, -Force));
          else
            TempVel = vvadd(TempVel, vfmul(Direction, SpeedLeft));
        } else
          TempVel = vvadd(TempVel, vfmul(Direction, Force));

        pCore->m_Vel = clamp_vel(pCore->m_MoveRestrictions, TempVel);
      }
    } else if (Type == TILE_SPEED_BOOST) {
      static const float MaxSpeedScale = 5.0f;
      if (MaxSpeed == 0) {
        float MaxRampSpeed = pCore->m_pTuning->m_VelrampRange / (50 * logf(fmaxf((float)pCore->m_pTuning->m_VelrampCurvature, 1.01f)));
        MaxSpeed = fmaxf(MaxRampSpeed, pCore->m_pTuning->m_VelrampStart) / 50 * MaxSpeedScale;
      }

      float CurrentDirectionalSpeed = vdot(Direction, pCore->m_Vel);
      float TempMaxSpeed = MaxSpeed / MaxSpeedScale;
      if (CurrentDirectionalSpeed + Force > TempMaxSpeed)
        TempVel = vvadd(TempVel, vfmul(Direction, (TempMaxSpeed - CurrentDirectionalSpeed)));
      else
        TempVel = vvadd(TempVel, vfmul(Direction, Force));

      pCore->m_Vel = clamp_vel(pCore->m_MoveRestrictions, TempVel);
    }
  }
}

bool cc_freeze(SCharacterCore *pCore, int Seconds) {
  if (Seconds <= 0 || pCore->m_FreezeTime > Seconds * GAME_TICK_SPEED)
    return false;
  if (pCore->m_FreezeTime == 0 || pCore->m_FreezeStart < pCore->m_pWorld->m_GameTick - GAME_TICK_SPEED) {
    pCore->m_FreezeTime = Seconds * GAME_TICK_SPEED;
    pCore->m_FreezeStart = pCore->m_pWorld->m_GameTick;
    return true;
  }
  return false;
}

void cc_release_hook(SCharacterCore *pCore) {
  pCore->m_HookedPlayer = -1;
  pCore->m_HookState = HOOK_RETRACTED;
}

void cc_reset_hook(SCharacterCore *pCore) {
  cc_release_hook(pCore);
  pCore->m_HookPos = pCore->m_Pos;
}

void cc_reset_pickups(SCharacterCore *pCore) {
  memset(pCore->m_aWeaponGot + WEAPON_SHOTGUN, 0, 4);
  if (pCore->m_ActiveWeapon > WEAPON_SHOTGUN)
    pCore->m_ActiveWeapon = WEAPON_GUN;
}

void wc_release_hooked(SWorldCore *pCore, int Id);

static void cc_handle_teleport(SCharacterCore *pCore, int z) {
  int Num = pCore->m_pCollision->m_aNumTeleOuts[z];
  if (Num <= 0)
    return;

  pCore->m_Pos = pCore->m_pCollision->m_apTeleOuts[z][pCore->m_Input.m_TeleOut % Num];
  cc_calc_indices(pCore);
  if (!pCore->m_pWorld->m_pConfig->m_SvTeleportHoldHook) {
    cc_reset_hook(pCore);
  }
  if (pCore->m_pWorld->m_pConfig->m_SvTeleportLoseWeapons)
    cc_reset_pickups(pCore);
}

static void cc_handle_evil_teleport(SCharacterCore *pCore, int evilz) {
  int Num = pCore->m_pCollision->m_aNumTeleOuts[evilz];
  if (Num <= 0)
    return;

  pCore->m_Pos = pCore->m_pCollision->m_apTeleOuts[evilz][pCore->m_Input.m_TeleOut % Num];
  cc_calc_indices(pCore);
  pCore->m_Vel = vec2_init(0, 0);

  if (!pCore->m_pWorld->m_pConfig->m_SvTeleportHoldHook) {
    cc_reset_hook(pCore);
    wc_release_hooked(pCore->m_pWorld, pCore->m_Id);
  }
  if (pCore->m_pWorld->m_pConfig->m_SvTeleportLoseWeapons) {
    cc_reset_pickups(pCore);
  }
}

static void cc_handle_check_teleport(SCharacterCore *pCore) {
  int Num;
  for (int k = pCore->m_TeleCheckpoint; k >= 0; k--) {
    if ((Num = pCore->m_pCollision->m_aNumTeleCheckOuts[k])) {
      pCore->m_Pos = pCore->m_pCollision->m_apTeleCheckOuts[k][pCore->m_Input.m_TeleOut % Num];
      cc_calc_indices(pCore);

      if (!pCore->m_pWorld->m_pConfig->m_SvTeleportHoldHook) {
        cc_reset_hook(pCore);
      }
      return;
    }
  }

  // fallback to regular spawn
  mvec2 SpawnPos;
  if (wc_next_spawn(pCore->m_pWorld, &SpawnPos, pCore->m_Id)) {
    pCore->m_Pos = SpawnPos;
    cc_calc_indices(pCore);

    if (!pCore->m_pWorld->m_pConfig->m_SvTeleportHoldHook) {
      cc_reset_hook(pCore);
    }
  }
}

static void cc_handle_check_evil_teleport(SCharacterCore *pCore) {
  int Num;
  for (int k = pCore->m_TeleCheckpoint; k >= 0; k--) {
    if ((Num = pCore->m_pCollision->m_aNumTeleCheckOuts[k])) {
      pCore->m_Pos = pCore->m_pCollision->m_apTeleCheckOuts[k][pCore->m_Input.m_TeleOut % Num];
      cc_calc_indices(pCore);
      pCore->m_Vel = vec2_init(0, 0);

      if (!pCore->m_pWorld->m_pConfig->m_SvTeleportHoldHook) {
        cc_reset_hook(pCore);
        wc_release_hooked(pCore->m_pWorld, pCore->m_Id);
      }
      return;
    }
  }

  // fallback to regular spawn
  mvec2 SpawnPos;
  if (wc_next_spawn(pCore->m_pWorld, &SpawnPos, pCore->m_Id)) {
    pCore->m_Pos = SpawnPos;
    cc_calc_indices(pCore);
    pCore->m_Vel = vec2_init(0, 0);

    if (!pCore->m_pWorld->m_pConfig->m_SvTeleportHoldHook) {
      cc_reset_hook(pCore);
      wc_release_hooked(pCore->m_pWorld, pCore->m_Id);
    }
  }
}

void cc_handle_tiles(SCharacterCore *pCore, int Index) {
  int MapIndex = Index;
  pCore->m_IsInFreeze = false;

  if (Index < 0) {
    pCore->m_LastRefillJumps = false;
    pCore->m_LastPenalty = false;
    pCore->m_LastBonus = false;
    return;
  }

  int TileIndex = get_tile_index(pCore->m_pCollision, MapIndex);
  int TileFIndex = pCore->m_pCollision->m_MapData.front_layer.data ? get_front_tile_index(pCore->m_pCollision, MapIndex) : 0;

  // teleport checkpoint
  if (pCore->m_pCollision->m_MapData.tele_layer.type) {
    int TeleCheckpoint = is_tele_checkpoint(pCore->m_pCollision, MapIndex);
    if (TeleCheckpoint)
      pCore->m_TeleCheckpoint = TeleCheckpoint;
  }

  // use the dispatch table for O(1) lookup
  g_apGameTileHandlers[TileIndex](pCore);
  // don't do it twice for the same tile
  if (TileFIndex != TileIndex)
    g_apGameTileHandlers[TileFIndex](pCore);

  // this logic must run after the handlers
  if (TileIndex == TILE_REFILL_JUMPS || TileFIndex == TILE_REFILL_JUMPS)
    pCore->m_LastRefillJumps = true;
  else
    pCore->m_LastRefillJumps = false;

  if (TileIndex == TILE_FREEZE || TileFIndex == TILE_FREEZE || TileIndex == TILE_DFREEZE || TileFIndex == TILE_DFREEZE)
    pCore->m_IsInFreeze = true;

  if (vgety(pCore->m_Vel) > 0 && (pCore->m_MoveRestrictions & CANTMOVE_DOWN)) {
    pCore->m_Jumped = 0;
    pCore->m_JumpedTotal = 0;
  }
  pCore->m_Vel = clamp_vel(pCore->m_MoveRestrictions, pCore->m_Vel);

  // switch layer handling
  if (pCore->m_pCollision->m_MapData.switch_layer.type) {
    unsigned char Number = get_switch_number(pCore->m_pCollision, MapIndex);
    unsigned char Type = get_switch_type(pCore->m_pCollision, MapIndex);
    unsigned char Delay = get_switch_delay(pCore->m_pCollision, MapIndex);

    // use the dispatch table for O(1) lookup
    g_apSwitchTileHandlers[Type](pCore, Number, Delay);

    // Handle state resets for bonus/penalty
    // The handlers set the flag to true, this function resets them
    if (Type != TILE_ADD_TIME)
      pCore->m_LastPenalty = false;
    if (Type != TILE_SUBTRACT_TIME)
      pCore->m_LastBonus = false;
  }

  // teleport layer handling
  if (!pCore->m_pCollision->m_MapData.tele_layer.type)
    return;

  int z = is_teleport(pCore->m_pCollision, MapIndex);
  if (z) {
    cc_handle_teleport(pCore, z);
    return;
  }

  int evilz = is_evil_teleport(pCore->m_pCollision, MapIndex);
  if (evilz) {
    cc_handle_evil_teleport(pCore, evilz);
    return;
  }

  if (is_check_evil_teleport(pCore->m_pCollision, MapIndex)) {
    cc_handle_check_evil_teleport(pCore);
    return;
  }

  if (is_check_teleport(pCore->m_pCollision, MapIndex)) {
    cc_handle_check_teleport(pCore);
    return;
  }
}

static inline bool broad_indices_check(const SCollision *__restrict__ pCollision, mvec2 Start, mvec2 End) {
  const mvec2 MinVec = _mm_min_ps(Start, End);
  const mvec2 MaxVec = _mm_max_ps(Start, End);
  const int MinX = (int)vgetx(MinVec) >> 5;
  const int MinY = (int)vgety(MinVec) >> 5;
  const int MaxX = ((int)vgetx(MaxVec) + 1) >> 5;
  const int MaxY = ((int)vgety(MaxVec) + 1) >> 5;
  const int DiffY = (MaxY - MinY);
  const int DiffX = (MaxX - MinX);

  return (bool)(pCollision->m_pBroadIndicesBitField[(MinY * pCollision->m_MapData.width) + MinX] & (uint64_t)1 << ((DiffY << 3) + DiffX));
}

void cc_ddrace_postcore_tick(SCharacterCore *pCore) {
  if (pCore->m_EndlessHook)
    pCore->m_HookTick = 0;

  pCore->m_FrozenLastTick = false;

  if (pCore->m_DeepFrozen)
    cc_freeze(pCore, pCore->m_pWorld->m_pConfig->m_SvFreezeDelay);

  // following jump rules can be overridden by tiles, like Refill Jumps,
  // Stopper and Wall Jump
  if (pCore->m_Jumps == -1) {
    // The player has only one ground jump, so his feet are always dark
    pCore->m_Jumped |= 2;
  } else if (pCore->m_Jumps == 0) {
    // The player has no jumps at all, so his feet are always dark
    pCore->m_Jumped |= 2;
  } else if (pCore->m_Jumps == 1 && pCore->m_Jumped > 0) {
    // If the player has only one jump, each jump is the last one
    pCore->m_Jumped |= 2;
  } else if (pCore->m_JumpedTotal < pCore->m_Jumps - 1 && pCore->m_Jumped > 1) {
    // The player has not yet used up all his jumps, so his feet remain light
    pCore->m_Jumped = 1;
  }

  if ((pCore->m_EndlessJump) && pCore->m_Jumped > 1) {
    // Super players and players with infinite jumps always have light feet
    pCore->m_Jumped = 1;
  }

  cc_handle_skippable_tiles(pCore, pCore->m_BlockIdx);

  const mvec2 PrevPos = pCore->m_PrevPos;
  const mvec2 Pos = pCore->m_Pos;
  const int Width = pCore->m_pCollision->m_MapData.width;
  if (broad_indices_check(pCore->m_pCollision, PrevPos, Pos)) {
    int sx = (int)vgetx(PrevPos) >> 5;
    int sy = (int)vgety(PrevPos) >> 5;
    int ex = (int)vgetx(Pos) >> 5;
    int ey = (int)vgety(Pos) >> 5;

    if ((sx != ex || sy != ey)) {
      bool yFirst = false;
      if (sx != ex && sy != ey) {
        float corner_x = (float)((sx < ex) ? sx + 1 : sx) * 32.f;
        float corner_y = (float)((sy < ey) ? sy + 1 : sy) * 32.f;
        mvec2 to_corner = vec2_init(corner_x - vgetx(PrevPos), corner_y - vgety(PrevPos));
        mvec2 to_pos = vec2_init(vgetx(Pos) - vgetx(PrevPos), vgety(Pos) - vgety(PrevPos));
        float cross_product = vgetx(to_pos) * vgety(to_corner) - vgety(to_pos) * vgetx(to_corner);
        if (cross_product * vgety(to_pos) < 0) {
          yFirst = true;
        }
      }
      if (yFirst) {
        int stepY = (ey > sy) ? 1 : -1;
        for (int y = sy; y != ey; y += stepY)
          cc_handle_tiles(pCore, y * Width + sx);
        int stepX = (ex > sx) ? 1 : -1;
        for (int x = sx; x != ex; x += stepX)
          cc_handle_tiles(pCore, ey * Width + x);

      } else {
        int stepX = (ex > sx) ? 1 : -1;
        for (int x = sx; x != ex; x += stepX)
          cc_handle_tiles(pCore, sy * Width + x);
        int stepY = (ey > sy) ? 1 : -1;
        for (int y = sy; y != ey; y += stepY)
          cc_handle_tiles(pCore, y * Width + ex);
      }
    }
    cc_handle_tiles(pCore, ey * Width + ex);
  }
  // teleport gun
  if (pCore->m_TeleGunTeleport) {
    pCore->m_Pos = pCore->m_TeleGunPos;
    if (!pCore->m_IsBlueTeleGunTeleport)
      pCore->m_Vel = vec2_init(0, 0);
    pCore->m_TeleGunTeleport = false;
    pCore->m_IsBlueTeleGunTeleport = false;
  }
}

static inline int compare_sign_bits(float f, int32_t i) {
  union {
    float f;
    uint32_t u;
  } bits;
  bits.f = f;
  return !((bits.u ^ (uint32_t)i) >> 31);
}

void cc_pre_tick(SCharacterCore *pCore) {
  cc_ddracetick(pCore);

  // getting move restrictions is always done after moving the character so don't do it here

  const bool Grounded =
      (pCore->m_pCollision->m_pTileInfos[pCore->m_BlockIdx] & INFO_CANGROUND) &&
      (check_point(pCore->m_pCollision, vec2_init(vgetx(pCore->m_Pos) + HALFPHYSICALSIZE, vgety(pCore->m_Pos) + HALFPHYSICALSIZE + 5)) ||
       check_point(pCore->m_pCollision, vec2_init(vgetx(pCore->m_Pos) - HALFPHYSICALSIZE, vgety(pCore->m_Pos) + HALFPHYSICALSIZE + 5)));

  pCore->m_Vel = vadd_y(pCore->m_Vel, pCore->m_pTuning->m_Gravity);
  pCore->m_Grounded = Grounded;
  if (pCore->m_Input.m_Jump) {
    if (!(pCore->m_Jumped & 1)) {
      if (Grounded && (!(pCore->m_Jumped & 2) || pCore->m_Jumps != 0)) {
        pCore->m_Vel = vsety(pCore->m_Vel, -pCore->m_pTuning->m_GroundJumpImpulse);
        if (pCore->m_Jumps > 1) {
          pCore->m_Jumped |= 1;
        } else {
          pCore->m_Jumped |= 3;
        }
        pCore->m_JumpedTotal = 0;
      } else if (!(pCore->m_Jumped & 2)) {
        pCore->m_Vel = vsety(pCore->m_Vel, -pCore->m_pTuning->m_AirJumpImpulse);
        pCore->m_Jumped |= 3;
        pCore->m_JumpedTotal++;
      }
    }
  } else {
    pCore->m_Jumped &= ~1;
  }

  if (pCore->m_Input.m_Hook) {
    if (pCore->m_HookState == HOOK_IDLE) {
      mvec2 TargetDirection = vnormalize_nomask(vec2_init(pCore->m_Input.m_TargetX, pCore->m_Input.m_TargetY));

      pCore->m_HookState = HOOK_FLYING;
      pCore->m_HookPos = vvadd(pCore->m_Pos, vfmul(TargetDirection, PHYSICALSIZE * 1.5f));
      pCore->m_HookDir = TargetDirection;
      pCore->m_HookedPlayer = -1;
      pCore->m_HookTick = (float)GAME_TICK_SPEED * (1.25f - pCore->m_pTuning->m_HookDuration);
    }
  } else {
    pCore->m_HookedPlayer = -1;
    pCore->m_HookState = HOOK_IDLE;
    pCore->m_HookPos = pCore->m_Pos;
  }

  float MaxSpeed = pCore->m_pTuning->m_AirControlSpeed;
  float Accel = pCore->m_pTuning->m_AirControlAccel;
  float Friction = pCore->m_pTuning->m_AirFriction;
  if (Grounded) {
    MaxSpeed = pCore->m_pTuning->m_GroundControlSpeed;
    Accel = pCore->m_pTuning->m_GroundControlAccel;
    Friction = pCore->m_pTuning->m_GroundFriction;
    pCore->m_Jumped &= ~2;
    pCore->m_JumpedTotal = 0;
  }

  if (pCore->m_Input.m_Direction < 0)
    pCore->m_Vel = vsetx(pCore->m_Vel, saturate_add(-MaxSpeed, MaxSpeed, vgetx(pCore->m_Vel), -Accel));
  else if (pCore->m_Input.m_Direction > 0)
    pCore->m_Vel = vsetx(pCore->m_Vel, saturate_add(-MaxSpeed, MaxSpeed, vgetx(pCore->m_Vel), Accel));
  else
    pCore->m_Vel = vsetx(pCore->m_Vel, vgetx(pCore->m_Vel) * Friction);

  if (pCore->m_HookState == HOOK_IDLE) {
    pCore->m_HookedPlayer = -1;
    pCore->m_HookPos = pCore->m_Pos;
  } else if (pCore->m_HookState >= HOOK_RETRACT_START && pCore->m_HookState < HOOK_RETRACT_END) {
    pCore->m_HookState++;
  } else if (pCore->m_HookState == HOOK_RETRACT_END) {
    pCore->m_HookState = HOOK_RETRACTED;
  } else if (pCore->m_HookState == HOOK_FLYING) {
    mvec2 HookBase = pCore->m_Pos;
    if (pCore->m_NewHook) {
      HookBase = pCore->m_HookTeleBase;
    }
    mvec2 NewPos = vvadd(pCore->m_HookPos, vfmul(pCore->m_HookDir, pCore->m_pTuning->m_HookFireSpeed));

    if (vsqdistance(HookBase, NewPos) > pCore->m_pTuning->m_HookLength * pCore->m_pTuning->m_HookLength) {
      pCore->m_HookState = HOOK_RETRACT_START;
      NewPos = vvadd(HookBase, vfmul(vnormalize_nomask(vvsub(NewPos, HookBase)), pCore->m_pTuning->m_HookLength));
    }
    // NOTE: this only really matters at the edge of the map but since we offset maps by 200 block idk if it actually matters. might remove this if it
    // ends up being a hot path. same for this logic in laser bounce
    {
      float x0 = vgetx(pCore->m_HookPos), y0 = vgety(pCore->m_HookPos);
      float x1 = vgetx(NewPos), y1 = vgety(NewPos);

      float dx = x1 - x0;
      float dy = y1 - y0;

      float W = (float)pCore->m_pCollision->m_MapData.width * 32.0f;
      float H = (float)pCore->m_pCollision->m_MapData.height * 32.0f;

      float xmin = 0.0f, ymin = 0.0f;
      float xmax = W - 1.0f, ymax = H - 1.0f;

      float t0 = 0.0f, t1 = 1.0f;

      CLIP(-dx, x0 - xmin); // left
      CLIP(dx, xmax - x0);  // right
      CLIP(-dy, y0 - ymin); // top
      CLIP(dy, ymax - y0);  // bottom

      // printf("Before: From:%.f,%.f, To:%.f,%.f\n", x0, y0, vgetx(NewPos), vgety(NewPos));
      NewPos = vec2_init(x0 + dx * t1, y0 + dy * t1);
      // printf("After: From:%.f,%.f, To:%.f,%.f\n", x0, y0, vgetx(NewPos), vgety(NewPos)); }
    }

    bool GoingToHitGround = false;
    bool GoingToRetract = false;
    bool GoingThroughTele = false;
    unsigned char teleNr = 0;
    unsigned char Hit = intersect_line_tele_hook(pCore->m_pCollision, pCore->m_HookPos, NewPos, &NewPos,
                                                 pCore->m_pCollision->m_MapData.tele_layer.type ? &teleNr : NULL);

    if (Hit) {
      if (Hit == TILE_NOHOOK)
        GoingToRetract = true;
      else if (Hit == TILE_TELEINHOOK)
        GoingThroughTele = true;
      else
        GoingToHitGround = true;
    }

    if (pCore->m_pWorld->m_NumCharacters > 1 && !pCore->m_HookHitDisabled && pCore->m_pTuning->m_PlayerHooking &&
        (pCore->m_HookState == HOOK_FLYING || !pCore->m_NewHook)) {

      SWorldCore *pWorld = pCore->m_pWorld;
      float ClosestDist = FLT_MAX;

      mvec2 StartPos = pCore->m_HookPos;
      mvec2 EndPos = NewPos;

      int MapWidth = pWorld->m_pCollision->m_MapData.width;
      int MapHeight = pWorld->m_pCollision->m_MapData.height;

      int StartX = (int)vgetx(StartPos) >> 5;
      int StartY = (int)vgety(StartPos) >> 5;
      int EndX = (int)vgetx(EndPos) >> 5;
      int EndY = (int)vgety(EndPos) >> 5;

      int CurrentX = StartX;
      int CurrentY = StartY;

      mvec2 Dir = vvsub(EndPos, StartPos);
      float dx = vgetx(Dir);
      float dy = vgety(Dir);

      int StepX = (dx > 0) ? 1 : -1;
      int StepY = (dy > 0) ? 1 : -1;

      float NextBoundaryX = (CurrentX + (dx > 0 ? 1 : 0)) * 32.0f;
      float NextBoundaryY = (CurrentY + (dy > 0 ? 1 : 0)) * 32.0f;

      float tMaxX = (dx != 0.0f) ? (NextBoundaryX - vgetx(StartPos)) / dx : FLT_MAX;
      float tMaxY = (dy != 0.0f) ? (NextBoundaryY - vgety(StartPos)) / dy : FLT_MAX;

      float tDeltaX = (dx != 0.0f) ? (32.0f * StepX) / dx : FLT_MAX;
      float tDeltaY = (dy != 0.0f) ? (32.0f * StepY) / dy : FLT_MAX;

      int aCheckedIndices[128];
      int NumChecked = 0;

      while (true) {
        // 3x3 block around the current cell
        for (int offsetY = -1; offsetY <= 1; ++offsetY) {
          for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            int CheckX = CurrentX + offsetX;
            int CheckY = CurrentY + offsetY;

            // Bounds check
            if (CheckX < 0 || CheckY < 0 || CheckX >= MapWidth || CheckY >= MapHeight)
              continue;

            int MapIndex = CheckY * MapWidth + CheckX;

            // Check if we already processed this cell for this hook tick
            bool bAlreadyChecked = false;
            for (int j = 0; j < NumChecked; ++j) {
              if (aCheckedIndices[j] == MapIndex) {
                bAlreadyChecked = true;
                break;
              }
            }

            if (bAlreadyChecked)
              continue;

            // Add to checked list
            if (NumChecked < 128) {
              aCheckedIndices[NumChecked++] = MapIndex;
            }

            // Now, check against all players in this cell
            int PlayerId = pWorld->m_Accelerator.m_pGrid->m_pTeeGrid[MapIndex];
            while (PlayerId >= 0) {
              SCharacterCore *pEntity = &pWorld->m_pCharacters[PlayerId];

              if (pEntity != pCore && !pEntity->m_Solo && !pCore->m_Solo) {
                mvec2 ClosestPoint;
                if (closest_point_on_line(StartPos, EndPos, pEntity->m_Pos, &ClosestPoint)) {
                  if (vdistance(pEntity->m_Pos, ClosestPoint) < PHYSICALSIZE + 2.0f) {
                    float dist = vdistance(StartPos, pEntity->m_Pos);
                    if (dist < ClosestDist) {
                      ClosestDist = dist;
                      pCore->m_HookedPlayer = pEntity->m_Id;
                    }
                  }
                }
              }
              PlayerId = pWorld->m_Accelerator.m_pTeeList[PlayerId].m_Child;
            }
          }
        }

        if (CurrentX == EndX && CurrentY == EndY) {
          break;
        }

        if (tMaxX < tMaxY) {
          tMaxX += tDeltaX;
          CurrentX += StepX;
        } else {
          tMaxY += tDeltaY;
          CurrentY += StepY;
        }
      }

      if (pCore->m_HookedPlayer != -1) {
        pCore->m_HookState = HOOK_GRABBED;
      }
    }

    if (pCore->m_HookState == HOOK_FLYING) {
      if (GoingToHitGround) {
        pCore->m_HookState = HOOK_GRABBED;
      } else if (GoingToRetract) {
        pCore->m_HookState = HOOK_RETRACT_START;
      }
      int NumOuts = pCore->m_pCollision->m_aNumTeleOuts[teleNr];
      if (GoingThroughTele && NumOuts > 0) {
        pCore->m_HookedPlayer = -1;
        mvec2 TargetDirection = vnormalize(vec2_init(pCore->m_Input.m_TargetX, pCore->m_Input.m_TargetY));
        pCore->m_NewHook = true;
        pCore->m_HookPos =
            vvadd(pCore->m_pCollision->m_apTeleOuts[teleNr][pCore->m_Input.m_TeleOut % NumOuts], vfmul(TargetDirection, PHYSICALSIZE * 1.5f));
        pCore->m_HookDir = TargetDirection;
        pCore->m_HookTeleBase = pCore->m_HookPos;
      } else {
        pCore->m_HookPos = NewPos;
      }
    }
  }

  if (pCore->m_HookState == HOOK_GRABBED) {
    if (pCore->m_HookedPlayer != -1) {
      SCharacterCore *pCharCore = &pCore->m_pWorld->m_pCharacters[pCore->m_HookedPlayer];
      if (pCharCore && pCore->m_Id != -1)
        pCore->m_HookPos = pCharCore->m_Pos;
      else {
        pCore->m_HookedPlayer = -1;
        pCore->m_HookState = HOOK_RETRACTED;
        pCore->m_HookPos = pCore->m_Pos;
      }
    } else if (vsqdistance(pCore->m_HookPos, pCore->m_Pos) > 46 * 46) {
      mvec2 HookVel = vfmul(vnormalize_nomask(vvsub(pCore->m_HookPos, pCore->m_Pos)), pCore->m_pTuning->m_HookDragAccel);
      if (vgety(HookVel) > 0)
        HookVel = vsety(HookVel, vgety(HookVel) * 0.3f);
      if (pCore->m_Input.m_Direction != 0 && compare_sign_bits(vgetx(HookVel), pCore->m_Input.m_Direction))
        HookVel = vsetx(HookVel, vgetx(HookVel) * 0.95f);
      else
        HookVel = vsetx(HookVel, vgetx(HookVel) * 0.75f);

      mvec2 NewVel = vvadd(pCore->m_Vel, HookVel);

      const float NewVelLength = vsqlength(NewVel);
      if (NewVelLength < pCore->m_pTuning->m_HookDragSpeed * pCore->m_pTuning->m_HookDragSpeed || NewVelLength < vsqlength(pCore->m_Vel))
        pCore->m_Vel = NewVel;
    }

    pCore->m_HookTick++;
    if (pCore->m_HookedPlayer != -1 &&
        (pCore->m_HookTick > GAME_TICK_SPEED + GAME_TICK_SPEED / 5 || (pCore->m_HookedPlayer >= pCore->m_pWorld->m_NumCharacters))) {
      pCore->m_HookedPlayer = -1;
      pCore->m_HookState = HOOK_RETRACTED;
      pCore->m_HookPos = pCore->m_Pos;
    }
  }
}

void cc_remove_ninja(SCharacterCore *pCore) {
  pCore->m_Ninja.m_ActivationDir = vec2_init(0, 0);
  pCore->m_Ninja.m_ActivationTick = 0;
  pCore->m_Ninja.m_CurrentMoveTime = 0;
  pCore->m_Ninja.m_OldVelAmount = 0;
  pCore->m_ActiveWeapon = pCore->m_LastWeapon;
  pCore->m_aWeaponGot[WEAPON_NINJA] = false;
}

void cc_take_damage(SCharacterCore *pCore, mvec2 Force) {
  pCore->m_Vel = clamp_vel(pCore->m_MoveRestrictions, vvadd(pCore->m_Vel, Force));
  // printf("Took (%.2f, %.2f) damage to me (%.2f, %.2f). vel is now (%.2f, %.2f)\n", vgetx(Force),
  // vgety(Force), vgetx(pCore->m_Pos), vgety(pCore->m_Pos), vgetx(pCore->m_Vel), vgety(pCore->m_Vel));
}

void cc_handle_ninja(SCharacterCore *pCore) {

  if ((pCore->m_pWorld->m_GameTick - pCore->m_Ninja.m_ActivationTick) > (NINJA_DURATION * GAME_TICK_SPEED / 1000)) {
    // time's up, return
    cc_remove_ninja(pCore);
    return;
  }

  if (pCore->m_ActiveWeapon != WEAPON_NINJA)
    pCore->m_LastWeapon = pCore->m_ActiveWeapon;

  // force ninja Weapon
  pCore->m_ActiveWeapon = WEAPON_NINJA;

  pCore->m_Ninja.m_CurrentMoveTime--;

  if (pCore->m_Ninja.m_CurrentMoveTime == 0) {
    // reset velocity
    pCore->m_Vel = vfmul(pCore->m_Ninja.m_ActivationDir, pCore->m_Ninja.m_OldVelAmount);
  }

  if (pCore->m_Ninja.m_CurrentMoveTime > 0) {
    // Set velocity
    pCore->m_Vel = vfmul(pCore->m_Ninja.m_ActivationDir, NINJA_VELOCITY);
    mvec2 OldPos = pCore->m_Pos;
    mvec2 GroundElasticity = vec2_init(pCore->m_pTuning->m_GroundElasticityX, pCore->m_pTuning->m_GroundElasticityY);

    {
      bool _;
      move_box(pCore->m_pCollision, pCore->m_Pos, pCore->m_Vel, &pCore->m_Pos, &pCore->m_Vel, GroundElasticity, &_);
      cc_calc_indices(pCore);
    }

    pCore->m_Vel = vec2_init(0, 0);

    // check if we Hit anything along the way
    {
      float Radius = PHYSICALSIZE * 2.0f;

      // check that we're not in solo part
      if (pCore->m_Solo)
        return;

      for (int i = 0; i < pCore->m_pWorld->m_NumCharacters; ++i) {
        if (vdistance(OldPos, pCore->m_Pos) < Radius + PHYSICALSIZE) {
          SCharacterCore *pChr = &pCore->m_pWorld->m_pCharacters[i];
          if (pChr == pCore)
            continue;

          if (pChr->m_Solo)
            return;

          // make sure we haven't Hit this object before
          bool AlreadyHit = false;
          for (int j = 0; j < pCore->m_NumObjectsHit; j++) {
            if (pCore->m_aHitObjects[j] == pChr->m_Id)
              AlreadyHit = true;
          }
          if (AlreadyHit)
            continue;

          // check so we are sufficiently close
          if (vdistance(pChr->m_Pos, pCore->m_Pos) > (PHYSICALSIZE * 2.0f))
            continue;

          // set his velocity to fast upward (for now)
          if (pCore->m_NumObjectsHit < 10)
            pCore->m_aHitObjects[pCore->m_NumObjectsHit++] = pChr->m_Id;

          cc_take_damage(pChr, vec2_init(0, -10.f));
        }
      }
    }

    return;
  }
}

void cc_handle_jetpack(SCharacterCore *pCore) {
  if (!(pCore->m_Input.m_Fire & 1) || pCore->m_FreezeTime)
    return;

  const mvec2 Direction = vnormalize(vec2_init(pCore->m_Input.m_TargetX, pCore->m_Input.m_TargetY));
  float Strength = pCore->m_pTuning->m_JetpackStrength;
  cc_take_damage(pCore, vfmul(Direction, -(Strength / 100.f / 6.11f)));
}

void cc_do_weapon_switch(SCharacterCore *pCore) {
  uint8_t WantedWeapon = imin(pCore->m_Input.m_WantedWeapon, NUM_WEAPONS - 1);
  if (WantedWeapon != pCore->m_ActiveWeapon && pCore->m_aWeaponGot[WantedWeapon])
    pCore->m_QueuedWeapon = WantedWeapon;

  if (!pCore->m_aWeaponGot[pCore->m_QueuedWeapon] || pCore->m_ReloadTimer != 0 || pCore->m_aWeaponGot[WEAPON_NINJA])
    return;
  pCore->m_LastWeapon = pCore->m_ActiveWeapon;
  pCore->m_ActiveWeapon = pCore->m_QueuedWeapon;
}

void wc_remove_entity(SWorldCore *pWorld, SEntity *pEnt);

void cc_fire_weapon(SCharacterCore *pCore) {
  if (pCore->m_aWeaponGot[pCore->m_QueuedWeapon] && pCore->m_ReloadTimer == 0 && !pCore->m_aWeaponGot[WEAPON_NINJA]) {
    pCore->m_LastWeapon = pCore->m_ActiveWeapon;
    pCore->m_ActiveWeapon = pCore->m_QueuedWeapon;
  }
  if (pCore->m_FreezeTime)
    return;
  // don't fire hammer when player is deep and sv_deepfly is disabled
  if (!pCore->m_pWorld->m_pConfig->m_SvDeepfly && pCore->m_ActiveWeapon == WEAPON_HAMMER && pCore->m_DeepFrozen)
    return;

  // check if we gonna fire
  bool WillFire = !pCore->m_PrevFire && pCore->m_Input.m_Fire;
  if (pCore->m_Input.m_Fire & 1) {
    if (pCore->m_ActiveWeapon >= WEAPON_SHOTGUN && pCore->m_ActiveWeapon <= WEAPON_LASER)
      WillFire = true;
    else if (pCore->m_Jetpack && pCore->m_ActiveWeapon == WEAPON_GUN)
      WillFire = true;
    else if (pCore->m_FrozenLastTick)
      WillFire = true;
  }

  if (!WillFire)
    return;

  const mvec2 Direction = vnormalize_nomask(vec2_init(pCore->m_Input.m_TargetX, pCore->m_Input.m_TargetY));
  const mvec2 ProjStartPos = vvadd(pCore->m_Pos, vfmul(Direction, PHYSICALSIZE * 0.75f));
  if (vgetx(ProjStartPos) < 0 || vgety(ProjStartPos) < 0 || vgetx(ProjStartPos) >= pCore->m_pCollision->m_MapData.width * 32 ||
      vgety(ProjStartPos) >= pCore->m_pCollision->m_MapData.height * 32) {
    return;
  }

  pCore->m_AttackTick = pCore->m_pWorld->m_GameTick;

  switch (pCore->m_ActiveWeapon) {
  case WEAPON_HAMMER: {
    // reset objects Hit
    pCore->m_NumObjectsHit = 0;
    if (pCore->m_pWorld->m_NumCharacters <= 1)
      break;
    if (pCore->m_HammerHitDisabled)
      break;
    if (pCore->m_Solo)
      break;

    int Hits = 0;
    for (int i = 0; i < pCore->m_pWorld->m_NumCharacters; ++i) {
      if (vdistance(pCore->m_pWorld->m_pCharacters[i].m_Pos, ProjStartPos) < HALFPHYSICALSIZE + PHYSICALSIZE) {
        SCharacterCore *pTarget = &pCore->m_pWorld->m_pCharacters[i];

        if (pTarget == pCore || pTarget->m_Solo)
          continue;

        mvec2 Dir;
        if (vsqdistance(pTarget->m_Pos, pCore->m_Pos) > 0.0f)
          Dir = vnormalize(vvsub(pTarget->m_Pos, pCore->m_Pos));
        else
          Dir = vec2_init(0.f, -1.f);

        float Strength = pCore->m_pTuning->m_HammerStrength;

        mvec2 Temp = vvadd(pTarget->m_Vel, vfmul(vnormalize(vvadd(Dir, vec2_init(0.f, -1.1f))), 10.0f));
        Temp = vvsub(clamp_vel(pTarget->m_MoveRestrictions, Temp), pTarget->m_Vel);

        mvec2 Force = vfmul(vvadd(vec2_init(0.f, -1.0f), Temp), Strength);

        cc_take_damage(pTarget, Force);
        cc_unfreeze(pTarget);

        Hits++;
      }
    }

    // if we Hit anything, we have to wait for the reload
    if (Hits) {
      float FireDelay = pCore->m_pTuning->m_HammerHitFireDelay;
      pCore->m_ReloadTimer = FireDelay * GAME_TICK_SPEED / 1000;
    }
    break;
  }

  case WEAPON_GUN: {
    if (pCore->m_HasTelegunGun) {
      const int Lifetime = (int)(GAME_TICK_SPEED * pCore->m_pTuning->m_GunLifetime);
      SProjectile *pNewProj = malloc(sizeof(SProjectile));
      prj_init(pNewProj, pCore->m_pWorld, WEAPON_GUN, pCore->m_Id, ProjStartPos, Direction, Lifetime, false, false, 0, 0);
      wc_insert_entity(pCore->m_pWorld, (SEntity *)pNewProj);
    }
    break;
  }

  case WEAPON_SHOTGUN: {
    const float LaserReach = pCore->m_pTuning->m_LaserReach;
    SLaser *pNewLaser = malloc(sizeof(SLaser));
    lsr_init(pNewLaser, pCore->m_pWorld, WEAPON_SHOTGUN, pCore->m_Id, pCore->m_Pos, Direction, LaserReach);
    wc_insert_entity(pCore->m_pWorld, (SEntity *)pNewLaser);
    break;
  }

  case WEAPON_GRENADE: {
    const int Lifetime = (int)(GAME_TICK_SPEED * pCore->m_pTuning->m_GrenadeLifetime);
    SProjectile *pNewProj = malloc(sizeof(SProjectile));
    prj_init(pNewProj, pCore->m_pWorld, WEAPON_GRENADE, pCore->m_Id, ProjStartPos, Direction, Lifetime, false, true, 0, 0);
    wc_insert_entity(pCore->m_pWorld, (SEntity *)pNewProj);
    break;
  }

  case WEAPON_LASER: {
    const float LaserReach = pCore->m_pTuning->m_LaserReach;
    SLaser *pNewLaser = malloc(sizeof(SLaser));
    lsr_init(pNewLaser, pCore->m_pWorld, WEAPON_LASER, pCore->m_Id, pCore->m_Pos, Direction, LaserReach);
    wc_insert_entity(pCore->m_pWorld, (SEntity *)pNewLaser);
    break;
  }

  case WEAPON_NINJA: {
    // reset Hit objects
    pCore->m_NumObjectsHit = 0;

    pCore->m_Ninja.m_ActivationDir = Direction;
    pCore->m_Ninja.m_CurrentMoveTime = (NINJA_MOVETIME * GAME_TICK_SPEED) / 1000;
    pCore->m_Ninja.m_OldVelAmount = vlength(pCore->m_Vel);
    break;
  }
  default:
    __builtin_unreachable();
  }

  // reloadtimer can be changed earlier by hammer so check again
  if (!pCore->m_ReloadTimer) {
    pCore->m_ReloadTimer = (*(&pCore->m_pTuning->m_HammerFireDelay + pCore->m_ActiveWeapon) * GAME_TICK_SPEED) / 1000;
  }
}

void cc_handle_weapons(SCharacterCore *pCore) {
  if (pCore->m_ActiveWeapon == WEAPON_NINJA)
    cc_handle_ninja(pCore);
  if (pCore->m_Jetpack && pCore->m_ActiveWeapon == WEAPON_GUN)
    cc_handle_jetpack(pCore);
  if (pCore->m_ReloadTimer) {
    --pCore->m_ReloadTimer;
    return;
  }
  if (!pCore->m_ReloadTimer)
    cc_fire_weapon(pCore);
}

void cc_tick(SCharacterCore *pCore) {
  if (pCore->m_RespawnDelay)
    --pCore->m_RespawnDelay;

  cc_tick_deferred(pCore);

  // handle Weapons
  cc_handle_weapons(pCore);

  cc_ddrace_postcore_tick(pCore);

  pCore->m_PrevPos = pCore->m_Pos;
  if (pCore->m_HitNum > 0)
    --pCore->m_HitNum;
}

void cc_on_input(SCharacterCore *pCore, const SPlayerInput *pNewInput) {
  // kill trigger
  if (!pCore->m_RespawnDelay && get_flag_kill(pNewInput))
    cc_die(pCore);

  pCore->m_Input = *pNewInput;
  if (pCore->m_Input.m_TargetX == 0 && pCore->m_Input.m_TargetY == 0)
    pCore->m_Input.m_TargetY = -1;

  cc_do_weapon_switch(pCore);
  if (!pCore->m_ReloadTimer)
    cc_fire_weapon(pCore);

  pCore->m_PrevFire = pCore->m_Input.m_Fire;
}

// }}}

// WorldCore functions {{{

void init_switchers(SWorldCore *pCore, int HighestSwitchNumber) {
  if (HighestSwitchNumber > 0) {
    free(pCore->m_pSwitches);
    pCore->m_NumSwitches = HighestSwitchNumber + 1;
    pCore->m_pSwitches = malloc((size_t)pCore->m_NumSwitches * sizeof(SSwitch));
  } else {
    free(pCore->m_pSwitches);
    pCore->m_pSwitches = NULL;
    return;
  }

  for (int i = 0; i < pCore->m_NumSwitches; ++i) {
    pCore->m_pSwitches[i] = (SSwitch){.m_Initial = true, .m_Status = true, .m_EndTick = 0, .m_Type = 0, .m_LastUpdateTick = 0};
  }
}

// NOTE: spawn points are not the same as in ddnet. other players will not be
// respected
bool wc_next_spawn(SWorldCore *pCore, mvec2 *pOutPos, int Id) {
  if (!pCore->m_pCollision->m_pSpawnPoints)
    return false;
  *pOutPos = vfadd(vfmul(pCore->m_pCollision->m_pSpawnPoints[Id % pCore->m_pCollision->m_NumSpawnPoints], 32), 16);
  return true;
}

// static inline unsigned int fast_rand_u32(unsigned int *state) {
//   unsigned int x = *state;
//   x ^= x << 13;
//   x ^= x >> 17;
//   x ^= x << 5;
//   *state = x;
//   return x;
// }
//
// bool wc_next_spawn(SWorldCore *pCore, mvec2 *pOutPos, int Id) {
//   if (!pCore->m_pCollision->m_pSpawnPoints)
//     return false;
//
//   unsigned int state = Id ^ pCore->m_GameTick;
//   int Idx = fast_rand_u32(&state) % ((pCore->m_pCollision->m_MapData.width - 1) * (pCore->m_pCollision->m_MapData.height - 1));
//   while (pCore->m_pCollision->m_pTileInfos[Idx] & INFO_ISSOLID)
//     Idx = fast_rand_u32(&state) % ((pCore->m_pCollision->m_MapData.width - 1) * (pCore->m_pCollision->m_MapData.height - 1));
//
//   int x = Idx % pCore->m_pCollision->m_MapData.width;
//   int y = Idx / pCore->m_pCollision->m_MapData.width;
//   *pOutPos = vfadd(vfmul(vec2_init(x, y), 32), 16);
//   return true;
// }

void wc_release_hooked(SWorldCore *pCore, int Id) {
  for (int i = 0; i < pCore->m_NumCharacters; ++i)
    if (pCore->m_pCharacters[i].m_HookedPlayer == Id)
      cc_release_hook(&pCore->m_pCharacters[pCore->m_pCharacters[i].m_HookedPlayer]);
}

bool wc_on_entity(SWorldCore *pCore, int Index, int x, int y, int Layer, int Flags, int Number) {
  const mvec2 Pos = vec2_init((x * 32.0f) + 16.0f, (y * 32.0f) + 16.0f);

  int aSides[8];
  aSides[0] = entity(pCore->m_pCollision, x, y + 1, Layer);
  aSides[1] = entity(pCore->m_pCollision, x + 1, y + 1, Layer);
  aSides[2] = entity(pCore->m_pCollision, x + 1, y, Layer);
  aSides[3] = entity(pCore->m_pCollision, x + 1, y - 1, Layer);
  aSides[4] = entity(pCore->m_pCollision, x, y - 1, Layer);
  aSides[5] = entity(pCore->m_pCollision, x - 1, y - 1, Layer);
  aSides[6] = entity(pCore->m_pCollision, x - 1, y, Layer);
  aSides[7] = entity(pCore->m_pCollision, x - 1, y + 1, Layer);

  if (Index == ENTITY_DOOR) {
    for (int i = 0; i < 8; i++) {
      if (aSides[i] >= ENTITY_LASER_SHORT && aSides[i] <= ENTITY_LASER_LONG) {
        // TODO: DOORS
        // new CDoor(&GameServer()->m_World, // GameWorld
        //           Pos,                    // Pos
        //           pi / 4 * i,             // Rotation
        //           32 * 3 + 32 * (aSides[i] - ENTITY_LASER_SHORT) * 3, //
        //           Length Number // Number
        // );
      }
    }
  } else if (Index == ENTITY_CRAZY_SHOTGUN_EX) {
    int Dir;
    if (!Flags)
      Dir = 0;
    else if (Flags == ROTATION_90)
      Dir = 1;
    else if (Flags == ROTATION_180)
      Dir = 2;
    else
      Dir = 3;
    float Deg = Dir * (PI / 2);
    SProjectile *pBullet = malloc(sizeof(SProjectile));
    prj_init(pBullet, pCore,
             WEAPON_SHOTGUN,                // Type
             -1,                            // Owner
             Pos,                           // Pos
             vec2_init(sin(Deg), cos(Deg)), // Dir
             -2,                            // Span
             true,                          // Freeze
             true,                          // Explosive
             Layer, Number);
    pBullet->m_Bouncing = 2 - (Dir % 2);
    wc_insert_entity(pCore, (SEntity *)pBullet);
  } else if (Index == ENTITY_CRAZY_SHOTGUN) {
    int Dir;
    if (!Flags)
      Dir = 0;
    else if (Flags == (TILEFLAG_ROTATE))
      Dir = 1;
    else if (Flags == (TILEFLAG_XFLIP | TILEFLAG_YFLIP))
      Dir = 2;
    else
      Dir = 3;
    float Deg = Dir * (PI / 2);
    SProjectile *pBullet = malloc(sizeof(SProjectile));
    prj_init(pBullet, pCore,
             WEAPON_SHOTGUN,                // Type
             -1,                            // Owner
             Pos,                           // Pos
             vec2_init(sin(Deg), cos(Deg)), // Dir
             -2,                            // Span
             true,                          // Freeze
             false,                         // Explosive
             Layer, Number);
    pBullet->m_Bouncing = 2 - (Dir % 2);
    wc_insert_entity(pCore, (SEntity *)pBullet);
  }

  if (Index >= ENTITY_LASER_FAST_CCW && Index <= ENTITY_LASER_FAST_CW) {
    // TODO: IMPLEMENT LIGHTS
    // int aSides2[8];
    // aSides2[0] = entity(pCore->m_pCollision, x, y + 2, Layer);
    // aSides2[1] = entity(pCore->m_pCollision, x + 2, y + 2, Layer);
    // aSides2[2] = entity(pCore->m_pCollision, x + 2, y, Layer);
    // aSides2[3] = entity(pCore->m_pCollision, x + 2, y - 2, Layer);
    // aSides2[4] = entity(pCore->m_pCollision, x, y - 2, Layer);
    // aSides2[5] = entity(pCore->m_pCollision, x - 2, y - 2, Layer);
    // aSides2[6] = entity(pCore->m_pCollision, x - 2, y, Layer);
    // aSides2[7] = entity(pCore->m_pCollision, x - 2, y + 2, Layer);
    // int Ind = Index - ENTITY_LASER_STOP;
    // int M;
    // if (Ind < 0) {
    //   Ind = -Ind;
    //   M = 1;
    // } else if (Ind == 0)
    //   M = 0;
    // else
    //   M = -1;
    // float AngularSpeed = 0.0f;
    // if (Ind == 0)
    //   AngularSpeed = 0.0f;
    // else if (Ind == 1)
    //   AngularSpeed = PI / 360;
    // else if (Ind == 2)
    //   AngularSpeed = PI / 180;
    // else if (Ind == 3)
    //   AngularSpeed = PI / 90;
    // AngularSpeed *= M;
    // for (int i = 0; i < 8; i++) {
    //   if (aSides[i] >= ENTITY_LASER_SHORT && aSides[i] <=
    //   ENTITY_LASER_LONG)
    //   {
    //     CLight *pLight = new CLight(
    //         &GameServer()->m_World, Pos, pi / 4 * i,
    //         32 * 3 + 32 * (aSides[i] - ENTITY_LASER_SHORT) * 3, Layer,
    //         Number);
    //     pLight->m_AngularSpeed = AngularSpeed;
    //     if (aSides2[i] >= ENTITY_LASER_C_SLOW &&
    //         aSides2[i] <= ENTITY_LASER_C_FAST) {
    //       pLight->m_Speed = 1 + (aSides2[i] - ENTITY_LASER_C_SLOW) * 2;
    //       pLight->m_CurveLength = pLight->m_Length;
    //     } else if (aSides2[i] >= ENTITY_LASER_O_SLOW &&
    //                aSides2[i] <= ENTITY_LASER_O_FAST) {
    //       pLight->m_Speed = 1 + (aSides2[i] - ENTITY_LASER_O_SLOW) * 2;
    //       pLight->m_CurveLength = 0;
    //     } else
    //       pLight->m_CurveLength = pLight->m_Length;
    //   }
    // }
    // TODO: implement draggers + plasma
  } else if (Index >= ENTITY_DRAGGER_WEAK && Index <= ENTITY_DRAGGER_STRONG) {
    // new CDragger(&GameServer()->m_World, Pos, Index - ENTITY_DRAGGER_WEAK +
    // 1,
    //              false, Layer, Number);
  } else if (Index >= ENTITY_DRAGGER_WEAK_NW && Index <= ENTITY_DRAGGER_STRONG_NW) {
    // new CDragger(&GameServer()->m_World, Pos,
    //              Index - ENTITY_DRAGGER_WEAK_NW + 1, true, Layer, Number);
  } else if (Index == ENTITY_PLASMAE) {
    // new CGun(&GameServer()->m_World, Pos, false, true, Layer, Number);
  } else if (Index == ENTITY_PLASMAF) {
    // new CGun(&GameServer()->m_World, Pos, true, false, Layer, Number);
  } else if (Index == ENTITY_PLASMA) {
    // new CGun(&GameServer()->m_World, Pos, true, true, Layer, Number);
  } else if (Index == ENTITY_PLASMAU) {
    // new CGun(&GameServer()->m_World, Pos, false, false, Layer, Number);
  }

  return false;
}

void wc_create_all_entities(SWorldCore *pCore) {
  for (int y = 0; y < pCore->m_pCollision->m_MapData.height; y++) {
    for (int x = 0; x < pCore->m_pCollision->m_MapData.width; x++) {
      const int Index = pCore->m_pCollision->m_pWidthLookup[y] + x;

      // Game layer
      {
        const int GameIndex = pCore->m_pCollision->m_MapData.game_layer.data[Index];
        if (GameIndex == TILE_NPC) {
          pCore->m_pTunings[0].m_PlayerCollision = 0;
        } else if (GameIndex == TILE_EHOOK) {
          pCore->m_pConfig->m_SvEndlessDrag = 1;
        } else if (GameIndex == TILE_NOHIT) {
          pCore->m_pConfig->m_SvHit = 0;
        } else if (GameIndex == TILE_NPH) {
          pCore->m_pTunings[0].m_PlayerHooking = 0;
        } else if (GameIndex >= ENTITY_OFFSET) {
          wc_on_entity(pCore, GameIndex - ENTITY_OFFSET, x, y, LAYER_GAME, pCore->m_pCollision->m_MapData.game_layer.flags[Index], 0);
        }
      }

      if (pCore->m_pCollision->m_MapData.front_layer.data) {
        const int FrontIndex = pCore->m_pCollision->m_MapData.front_layer.data[Index];
        if (FrontIndex == TILE_NPC) {
          pCore->m_pTunings[0].m_PlayerCollision = 0;
        } else if (FrontIndex == TILE_EHOOK) {
          pCore->m_pConfig->m_SvEndlessDrag = 1;
        } else if (FrontIndex == TILE_NOHIT) {
          pCore->m_pConfig->m_SvHit = 0;
        } else if (FrontIndex == TILE_NPH) {
          pCore->m_pTunings[0].m_PlayerHooking = 0;
        } else if (FrontIndex >= ENTITY_OFFSET) {
          wc_on_entity(pCore, FrontIndex - ENTITY_OFFSET, x, y, LAYER_FRONT, pCore->m_pCollision->m_MapData.front_layer.flags[Index], 0);
        }
      }

      if (pCore->m_pCollision->m_MapData.switch_layer.type) {
        const int SwitchType = pCore->m_pCollision->m_MapData.switch_layer.type[Index];
        if (SwitchType >= ENTITY_OFFSET) {
          wc_on_entity(pCore, SwitchType - ENTITY_OFFSET, x, y, LAYER_SWITCH, pCore->m_pCollision->m_MapData.switch_layer.flags[Index],
                       pCore->m_pCollision->m_MapData.switch_layer.number[Index]);
        }
      }
    }
  }
}

STeeGrid tg_empty(void) { return (STeeGrid){0}; }
void tg_init(STeeGrid *pGrid, int width, int height) {
  free(pGrid->m_pTeeGrid);
  pGrid->m_pTeeGrid = malloc(sizeof(int) * width * height);
  memset(pGrid->m_pTeeGrid, -1, sizeof(int) * width * height);
  pGrid->hash = 0; // gets set by the last used world
}

void tg_destroy(STeeGrid *pGrid) {
  free(pGrid->m_pTeeGrid);
  *pGrid = tg_empty();
}

void wc_init(SWorldCore *pCore, SCollision *pCollision, STeeGrid *pGrid, SConfig *pConfig) {
  memset(pCore, 0, sizeof(SWorldCore));
  pCore->m_pCollision = pCollision;
  pCore->m_Accelerator.m_pGrid = pGrid;
  pCore->m_Accelerator.hash = ((uint64_t)rand() << 32) | rand();
  pCore->m_pConfig = pConfig;

  init_switchers(pCore, pCollision->m_HighestSwitchNumber);

  pCore->m_pTunings = pCollision->m_aTuningList;

  wc_create_all_entities(pCore);
}

void wc_free(SWorldCore *pCore) {
  for (int i = 0; i < NUM_WORLD_ENTTYPES; ++i) {
    SEntity *pEntity = pCore->m_apFirstEntityTypes[i];
    while (pEntity) {
      SEntity *pFree = pEntity;
      pEntity = pEntity->m_pNextTypeEntity;
      free(pFree);
    }
  }
  free(pCore->m_pSwitches);
  free(pCore->m_Accelerator.m_pTeeList);
  free(pCore->m_pCharacters);
  memset(pCore, 0, sizeof(SWorldCore));
}

static void wc_clear_grid(SWorldCore *pCore) {
  // clear grid
  memset(pCore->m_Accelerator.m_pGrid->m_pTeeGrid, -1, pCore->m_pCollision->m_MapData.width * pCore->m_pCollision->m_MapData.height * sizeof(int));
  // hook it up to our own things
  for (int i = 0; i < pCore->m_NumCharacters; ++i) {
    STeeLink *pChar = &pCore->m_Accelerator.m_pTeeList[i];
    if (pChar->m_Parent == -1)
      pCore->m_Accelerator.m_pGrid->m_pTeeGrid[pChar->m_Tile] = i;
  }
}

static void wc_accelerator_tick(SWorldCore *pCore) {
  if (pCore->m_Accelerator.hash != pCore->m_Accelerator.m_pGrid->hash) {
    wc_clear_grid(pCore);
    pCore->m_Accelerator.m_pGrid->hash = pCore->m_Accelerator.hash;
  }

  // set up accelerator
  for (int i = 0; i < pCore->m_NumCharacters; ++i) {
    SCharacterCore *pChar = &pCore->m_pCharacters[i];
    STeeLink *pLink = &pCore->m_Accelerator.m_pTeeList[pChar->m_Id];
    int PrevIdx = pLink->m_Tile;
    int Idx = ((int)vgety(pChar->m_Pos) >> 5) * pChar->m_pCollision->m_MapData.width + ((int)vgetx(pChar->m_Pos) >> 5);
    if (PrevIdx == Idx)
      continue;

    pLink = &pCore->m_Accelerator.m_pTeeList[pChar->m_Id];

    // remove ourselves from the previous index
    if (pLink->m_Parent >= 0) {
      pCore->m_Accelerator.m_pTeeList[pLink->m_Parent].m_Child = pLink->m_Child;
    }
    if (pLink->m_Child >= 0) {
      pCore->m_Accelerator.m_pTeeList[pLink->m_Child].m_Parent = pLink->m_Parent;
    }

    // only update grid head if we were the head
    if (pCore->m_Accelerator.m_pGrid->m_pTeeGrid[PrevIdx] == (int32_t)pLink->m_TeeId) {
      if (pLink->m_Child >= 0)
        pCore->m_Accelerator.m_pGrid->m_pTeeGrid[PrevIdx] = pLink->m_Child;
      else
        pCore->m_Accelerator.m_pGrid->m_pTeeGrid[PrevIdx] = -1;
    }

    if (pLink->m_Parent < 0 && pLink->m_Child < 0)
      pCore->m_Accelerator.m_pGrid->m_pTeeGrid[PrevIdx] = -1;

    // add ourselves onto the current index
    // move ourselves into the top of the list at our grid spot
    pLink->m_Tile = Idx;
    pLink->m_Parent = -1;
    pLink->m_Child = -1;
    if (pCore->m_Accelerator.m_pGrid->m_pTeeGrid[Idx] >= 0) {
      STeeLink *pTopLink = &pCore->m_Accelerator.m_pTeeList[pCore->m_Accelerator.m_pGrid->m_pTeeGrid[Idx]];
      if (pTopLink != pLink) {
        pLink->m_Child = pTopLink->m_TeeId;
        pTopLink->m_Parent = pLink->m_TeeId;
      }
    }
    pCore->m_Accelerator.m_pGrid->m_pTeeGrid[Idx] = pChar->m_Id;
  }
}

void wc_tick(SWorldCore *pCore) {
  ++pCore->m_GameTick;

  // Tick entities

  // Tick projectiles
  SEntity *pEntity = pCore->m_apFirstEntityTypes[WORLD_ENTTYPE_PROJECTILE];
  while (pEntity) {
    prj_tick((SProjectile *)pEntity);
    pEntity = pEntity->m_pNextTypeEntity;
  }

  // TODO: do lasers!!! aka. like 10 different entities that all identify as
  // lasers
  // Tick lasers
  pEntity = pCore->m_apFirstEntityTypes[WORLD_ENTTYPE_LASER];
  while (pEntity) {
    lsr_tick((SLaser *)pEntity);
    pEntity = pEntity->m_pNextTypeEntity;
  }

  for (int i = 0; i < pCore->m_NumCharacters; ++i)
    cc_do_pickup(&pCore->m_pCharacters[i]);

  // Tick characters
  if (pCore->m_NumCharacters > 1)
    wc_accelerator_tick(pCore);
  for (int i = 0; i < pCore->m_NumCharacters; ++i)
    cc_pre_tick(&pCore->m_pCharacters[i]);
  for (int i = 0; i < pCore->m_NumCharacters; ++i)
    cc_tick(&pCore->m_pCharacters[i]);

  // Do tick deferred
  // funny thing no other entities than the character actually have a deferred
  // tick function lol
  for (int i = 0; i < pCore->m_NumCharacters; ++i)
    cc_world_tick_deferred(&pCore->m_pCharacters[i]);

  // Remove all entities that are marked for destroy
  for (int i = 0; i < NUM_WORLD_ENTTYPES; ++i) {
    SEntity *pEntity = pCore->m_apFirstEntityTypes[i];
    while (pEntity) {
      SEntity *pFree = pEntity;
      pEntity = pEntity->m_pNextTypeEntity;
      if (pFree->m_MarkedForDestroy) {
        wc_remove_entity(pCore, pFree);
        free(pFree);
      }
    }
  }
}

SCharacterCore *wc_add_character(SWorldCore *pWorld, int Num) {
  if (Num <= 0) {
    return NULL; // nothing to add
  }

  const int OldSize = pWorld->m_NumCharacters;
  const int NewSize = OldSize + Num;

  // Resize both arrays
  SCharacterCore *pNewArray = realloc(pWorld->m_pCharacters, (size_t)NewSize * sizeof(SCharacterCore));
  STeeLink *pNewTeeLinkArray = realloc(pWorld->m_Accelerator.m_pTeeList, (size_t)NewSize * sizeof(STeeLink));

  if (!pNewArray || !pNewTeeLinkArray) {
    // If either failed, clean up the one that succeeded
    // to prevent memory leaks
    if (pNewArray) {
      pWorld->m_pCharacters = pNewArray;
    }
    if (pNewTeeLinkArray) {
      pWorld->m_Accelerator.m_pTeeList = pNewTeeLinkArray;
    }
    return NULL;
  }

  pWorld->m_pCharacters = pNewArray;
  pWorld->m_Accelerator.m_pTeeList = pNewTeeLinkArray;

  wc_clear_grid(pWorld);

  // Loop for each new character
  for (int i = 0; i < Num; i++) {
    SCharacterCore *pChar = &pWorld->m_pCharacters[OldSize + i];

    cc_init(pChar, pWorld);
    pChar->m_Id = OldSize + i;
    pWorld->m_Accelerator.m_pTeeList[pChar->m_Id].m_TeeId = pChar->m_Id;

    mvec2 SpawnPos;
    if (!wc_next_spawn(pWorld, &SpawnPos, pChar->m_Id)) {
      // If no spawn is available, bail out early.
      // Already created chars remain.
      pWorld->m_NumCharacters = OldSize + i;
      return NULL;
    }

    pChar->m_Pos = SpawnPos;
    pChar->m_PrevPos = SpawnPos;
    cc_calc_indices(pChar);

    // Add to grid list structure
    STeeLink *pLink = &pWorld->m_Accelerator.m_pTeeList[pChar->m_Id];
    int Idx = ((int)vgety(pChar->m_Pos) >> 5) * pChar->m_pCollision->m_MapData.width + ((int)vgetx(pChar->m_Pos) >> 5);
    int TopTee = pWorld->m_Accelerator.m_pGrid->m_pTeeGrid[Idx];

    if (TopTee >= 0) {
      STeeLink *pTopLink = &pWorld->m_Accelerator.m_pTeeList[TopTee];
      pLink->m_Child = pTopLink->m_TeeId;
      pTopLink->m_Parent = pLink->m_TeeId;
    } else {
      pLink->m_Child = -1;
    }

    pWorld->m_Accelerator.m_pGrid->m_pTeeGrid[Idx] = pChar->m_Id;
    pLink->m_Parent = -1;
    pLink->m_Tile = Idx;
  }

  pWorld->m_NumCharacters = NewSize;

  // Return the very *first* of the new characters
  return &pWorld->m_pCharacters[OldSize];
}

// TODO: remove ourselves from the grid linked list tater potator structure
void wc_remove_character(SWorldCore *pWorld, int CharacterId) {
  if (!pWorld || CharacterId < 0 || CharacterId >= pWorld->m_NumCharacters)
    return;

  wc_clear_grid(pWorld);
  SCharacterCore *pChars = pWorld->m_pCharacters;
  int lastIdx = pWorld->m_NumCharacters - 1;

  if (CharacterId != lastIdx) {
    pChars[CharacterId] = pChars[lastIdx];
    pChars[CharacterId].m_Id = CharacterId;
  }

  pWorld->m_NumCharacters--;

  if (pWorld->m_NumCharacters == 0) {
    free(pWorld->m_pCharacters);
    pWorld->m_pCharacters = NULL;
  } else {
    SCharacterCore *pNewArray = realloc(pWorld->m_pCharacters, (size_t)pWorld->m_NumCharacters * sizeof(SCharacterCore));
    if (pNewArray) {
      pWorld->m_pCharacters = pNewArray;
    }
    // if realloc fails, just keep the larger buffer
  }
}

void wc_create_explosion(SWorldCore *pWorld, mvec2 Pos, int Owner) {
#define EXPLOSION_RADIUS 135.0f
#define EXPLOSION_INNER_RADIUS 48.0f
  for (int i = 0; i < pWorld->m_NumCharacters; i++) {
    SCharacterCore *pChr = &pWorld->m_pCharacters[i];
    mvec2 Diff = vvsub(pChr->m_Pos, Pos);
    float l = vlength(Diff);
    if (l >= EXPLOSION_RADIUS + PHYSICALSIZE)
      continue;
    mvec2 ForceDir = vec2_init(0, 1);
    if (l)
      ForceDir = vnormalize_nomask(Diff);
    l = 1 - fclamp((l - EXPLOSION_INNER_RADIUS) / (EXPLOSION_RADIUS - EXPLOSION_INNER_RADIUS), 0.0f, 1.0f);
    float Strength;
    if (Owner != -1)
      Strength = pWorld->m_pCharacters[Owner].m_pTuning->m_ExplosionStrength;
    else
      Strength = pWorld->m_pTunings[0].m_ExplosionStrength;

    float Dmg = Strength * l;
    if (!(int)Dmg)
      continue;

    pChr->m_HitNum += Dmg;
    if (!pWorld->m_pCharacters[Owner].m_GrenadeHitDisabled || Owner == pChr->m_Id) {
      if (pChr->m_Solo && Owner != pChr->m_Id)
        continue;
      cc_take_damage(pChr, vfmul(ForceDir, Dmg * 2));
    }
  }
}

SCharacterCore *wc_intersect_character(SWorldCore *pWorld, mvec2 Pos0, mvec2 Pos1, float Radius, mvec2 *pNewPos, const SCharacterCore *pNotThis,
                                       const SCharacterCore *pThisOnly) {
  float ClosestLen = vdistance(Pos0, Pos1) * 100.0f;
  SCharacterCore *pClosest = NULL;

  for (int i = 0; i < pWorld->m_NumCharacters; ++i) {
    SCharacterCore *pEntity = &pWorld->m_pCharacters[i];
    if (pEntity == pNotThis)
      continue;

    if (pThisOnly && pEntity != pThisOnly)
      continue;

    mvec2 IntersectPos;
    if (closest_point_on_line(Pos0, Pos1, pEntity->m_Pos, &IntersectPos)) {
      float Len = vdistance(pEntity->m_Pos, IntersectPos);
      if (Len < PHYSICALSIZE + Radius) {
        Len = vdistance(Pos0, IntersectPos);
        if (Len < ClosestLen) {
          *pNewPos = IntersectPos;
          ClosestLen = Len;
          pClosest = pEntity;
        }
      }
    }
  }

  return pClosest;
}

void wc_insert_entity(SWorldCore *pWorld, SEntity *pEnt) {
  pEnt->m_pWorld = pWorld;
  pEnt->m_pCollision = pWorld->m_pCollision;
  if (pWorld->m_apFirstEntityTypes[pEnt->m_ObjType])
    pWorld->m_apFirstEntityTypes[pEnt->m_ObjType]->m_pPrevTypeEntity = pEnt;
  pEnt->m_pNextTypeEntity = pWorld->m_apFirstEntityTypes[pEnt->m_ObjType];
  pEnt->m_pPrevTypeEntity = NULL;
  pWorld->m_apFirstEntityTypes[pEnt->m_ObjType] = pEnt;
}

void wc_remove_entity(SWorldCore *pWorld, SEntity *pEnt) {
  if (!pEnt->m_pNextTypeEntity && !pEnt->m_pPrevTypeEntity && pWorld->m_apFirstEntityTypes[pEnt->m_ObjType] != pEnt)
    return;

  if (pEnt->m_pPrevTypeEntity)
    pEnt->m_pPrevTypeEntity->m_pNextTypeEntity = pEnt->m_pNextTypeEntity;
  else
    pWorld->m_apFirstEntityTypes[pEnt->m_ObjType] = pEnt->m_pNextTypeEntity;
  if (pEnt->m_pNextTypeEntity)
    pEnt->m_pNextTypeEntity->m_pPrevTypeEntity = pEnt->m_pPrevTypeEntity;

  if (pWorld->m_pNextTraverseEntity == pEnt)
    pWorld->m_pNextTraverseEntity = pEnt->m_pNextTypeEntity;

  pEnt->m_pNextTypeEntity = NULL;
  pEnt->m_pPrevTypeEntity = NULL;
}

void wc_copy_world(SWorldCore *__restrict__ pTo, SWorldCore *__restrict__ pFrom) {
  pTo->m_GameTick = pFrom->m_GameTick;
  pTo->m_pCollision = pFrom->m_pCollision;
  pTo->m_pConfig = pFrom->m_pConfig;
  pTo->m_pTunings = pFrom->m_pTunings;
  pTo->m_Accelerator.m_pGrid = pFrom->m_Accelerator.m_pGrid;
  // TODO: fix, this is very bad:
  pTo->m_Accelerator.hash = ((uint64_t)rand() << 32) | rand();

  // delete old entities
  for (int i = 0; i < NUM_WORLD_ENTTYPES; ++i) {
    SEntity *pEntity = pTo->m_apFirstEntityTypes[i];
    while (pEntity) {
      SEntity *pFree = pEntity;
      pEntity = pEntity->m_pNextTypeEntity;
      free(pFree);
    }
    pTo->m_apFirstEntityTypes[i] = NULL;
  }

  // insert new entities
#pragma clang loop unroll(full)
  for (int i = 0; i < NUM_WORLD_ENTTYPES; ++i) {
    SEntity *pEntity = pFrom->m_apFirstEntityTypes[i];
    while (pEntity) {
      switch (i) {
      case WORLD_ENTTYPE_PROJECTILE: {
        SEntity *pNew = malloc(sizeof(SProjectile));
        memcpy(pNew, pEntity, sizeof(SProjectile));
        wc_insert_entity(pTo, pNew);
        break;
      }
      case WORLD_ENTTYPE_LASER: {
        SEntity *pNew = malloc(sizeof(SLaser));
        memcpy(pNew, pEntity, sizeof(SLaser));
        wc_insert_entity(pTo, pNew);
        break;
      }
      }
      pEntity = pEntity->m_pNextTypeEntity;
    }
  }

  // copy characters and tee links
  if (pTo->m_NumCharacters != pFrom->m_NumCharacters) {
    free(pTo->m_Accelerator.m_pTeeList);
    free(pTo->m_pCharacters);
    pTo->m_NumCharacters = pFrom->m_NumCharacters;
    pTo->m_Accelerator.m_pTeeList = malloc(pTo->m_NumCharacters * sizeof(STeeLink));
    pTo->m_pCharacters = malloc(pTo->m_NumCharacters * sizeof(SCharacterCore));
  }

  for (int i = 0; i < pTo->m_NumCharacters; ++i)
    pTo->m_Accelerator.m_pTeeList[i] = pFrom->m_Accelerator.m_pTeeList[i];
  for (int i = 0; i < pTo->m_NumCharacters; ++i) {
    pTo->m_pCharacters[i] = pFrom->m_pCharacters[i];
    pTo->m_pCharacters[i].m_pCollision = pTo->m_pCollision;
    pTo->m_pCharacters[i].m_pWorld = pTo;
  }

  // copy switches
  if (pTo->m_NumSwitches != pFrom->m_NumSwitches) {
    free(pTo->m_pSwitches);
    pTo->m_NumSwitches = pFrom->m_NumSwitches;
    pTo->m_pSwitches = malloc(pTo->m_NumSwitches * sizeof(SSwitch));
  }
  for (int i = 0; i < pTo->m_NumSwitches; ++i)
    pTo->m_pSwitches[i] = pFrom->m_pSwitches[i];
}

SWorldCore wc_empty() { return (SWorldCore){0}; }

// }}}

#undef CLIP
