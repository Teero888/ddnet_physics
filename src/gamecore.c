#include "settings.h"
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

#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define THREAD_LOCAL __thread
#else
#define THREAD_LOCAL
#endif

static uint64_t next_world_hash(void) {
  static THREAD_LOCAL uint64_t s_Hash = 0;

  if (s_Hash == 0) {
    s_Hash = ((uint64_t)rand() << 45) ^ ((uint64_t)rand() << 30) ^ ((uint64_t)rand() << 15) ^ (uint64_t)rand();

    if (s_Hash == 0) {
      s_Hash = 1;
    }
  }
  return ++s_Hash;
}

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

static inline void emit_sound(SWorldCore *pWorld, mvec2 Pos, ESoundType Sound, int ClientId) {
  if (pWorld->sound)
    pWorld->sound(Pos, Sound, ClientId, pWorld->user_data);
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

// Applies the sign convention DDNet's speedup angle branches imply, so that a
// dot product of two such vectors equals its cos(angle difference) * length.
// x > 0.0000001f mirrors y; x < 0 is left alone; small non-negative x negates
// both (that branch reaches atanf(y/x) with a huge ratio, which lands half a
// turn away). See the call site in cc_handle_skippable_tiles.
static inline mvec2 speedup_signed(mvec2 v) {
  const float x = vgetx(v), y = vgety(v);
  if (x > 0.0000001f)
    return vec2_init(x, -y);
  if (x < 0.0f)
    return v;
  return vec2_init(-x, -y);
}

// True when either endpoint of the segment lies outside the map clip box, i.e.
// when the Liang-Barsky CLIP block below could actually reduce t1. Two packed
// compares instead of four divisions on the overwhelmingly common in-bounds path.
static inline bool segment_leaves_map(const SCollision *__restrict__ pCollision, mvec2 A, mvec2 B) {
  const mvec2 Lo = _mm_setzero_ps();
  const mvec2 Hi = pCollision->m_MapClipMax;
  const mvec2 Outside = _mm_or_ps(_mm_or_ps(_mm_cmplt_ps(A, Lo), _mm_cmpgt_ps(A, Hi)), _mm_or_ps(_mm_cmplt_ps(B, Lo), _mm_cmpgt_ps(B, Hi)));
  return (_mm_movemask_ps(Outside) & 3) != 0;
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
bool lsr_hit_character(SLaser *pLaser, mvec2 From, mvec2 To) {
  mvec2 At;
  SCharacterCore *pOwnerChar = pLaser->m_Owner >= 0 && pLaser->m_Owner < pLaser->m_Base.m_pWorld->m_NumCharacters
                                   ? &pLaser->m_Base.m_pWorld->m_pCharacters[pLaser->m_Owner]
                                   : NULL;
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
  if (!cc_take_damage(pHit, vec2_init(0, 0), 0))
    return true;
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
  // Same skip as the hook segment in cc_pre_tick: with both endpoints inside the
  // clip box the four CLIP evaluations cannot change t1, so they are pure cost.
  if (segment_leaves_map(pLaser->m_Base.m_pCollision, From, To)) {
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

    // we only care about moving the end point inside, so use t1.
    if (t1 != 1.0f)
      To = vec2_init(x0 + dx * t1, y0 + dy * t1);
  }

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
        emit_sound(pLaser->m_Base.m_pWorld, pLaser->m_Base.m_Pos, SOUND_TYPE_LASER_BOUNCE, pLaser->m_Owner);
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
  if (pLaser->m_Base.m_MarkedForDestroy)
    return;
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
  if (Owner >= 0 && Owner < pGameWorld->m_NumCharacters)
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
  // Marked between ticks -- cc_on_input's kill trigger reaches cc_die outside
  // wc_tick, and the sweep that frees this only runs at the end of the tick.
  if (pProj->m_Base.m_MarkedForDestroy)
    return;
  pProj->m_Base.m_Spawned = false;
  float Pt = (pProj->m_Base.m_pWorld->m_GameTick - pProj->m_StartTick - 1) / (float)GAME_TICK_SPEED;
  float Ct = (pProj->m_Base.m_pWorld->m_GameTick - pProj->m_StartTick) / (float)GAME_TICK_SPEED;
  mvec2 PrevPos = prj_get_pos(pProj, Pt);
  mvec2 CurPos = prj_get_pos(pProj, Ct);
  mvec2 ColPos;
  mvec2 NewPos;

  if (pProj->m_Base.m_pWorld->particle && pProj->m_Type == WEAPON_GRENADE)
    pProj->m_Base.m_pWorld->particle(PrevPos, PARTICLE_TYPE_SMOKE, pProj->m_Owner, pProj->m_Base.m_pWorld->user_data);

  if (vgetx(CurPos) < 0 || vgety(CurPos) < 0 || (int)(vgetx(CurPos) + 0.5) >> 5 >= pProj->m_Base.m_pCollision->m_MapData.width ||
      (int)(vgety(CurPos) + 0.5) >> 5 >= pProj->m_Base.m_pCollision->m_MapData.height) {
    pProj->m_Base.m_MarkedForDestroy = true;
    return;
  }
  bool Collide = intersect_line(pProj->m_Base.m_pCollision, PrevPos, CurPos, &ColPos, &NewPos);
  SCharacterCore *pOwnerChar = NULL;

  // A dying owner takes its shots with it (cc_die -> wc_remove_entities_from_player
  // under sv_destroy_bullets_on_death), so anything still flying here belongs to a
  // character that is still in the world.
  if (pProj->m_Owner >= 0 && pProj->m_Owner < pProj->m_Base.m_pWorld->m_NumCharacters)
    pOwnerChar = &pProj->m_Base.m_pWorld->m_pCharacters[pProj->m_Owner];

  SCharacterCore *pTargetChr = NULL;

  if (pOwnerChar ? !pOwnerChar->m_GrenadeHitDisabled : pProj->m_Base.m_pWorld->m_pConfig->m_SvHit)
    pTargetChr = wc_intersect_character(pProj->m_Base.m_pWorld, PrevPos, ColPos, 6.0f, &ColPos, pOwnerChar, NULL);

  if (pProj->m_LifeSpan > -1)
    pProj->m_LifeSpan--;

  if (Collide || (pTargetChr && (pOwnerChar ? !pOwnerChar->m_GrenadeHitDisabled
                                            : pProj->m_Base.m_pWorld->m_pConfig->m_SvHit || pProj->m_Owner == -1 || pTargetChr == pOwnerChar))) {
    if (pProj->m_Explosive && (!pTargetChr || (pTargetChr && (!pProj->m_Freeze || (pProj->m_Type == WEAPON_SHOTGUN && Collide))))) {
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
    } else if (pTargetChr) {
      if (cc_take_damage(pTargetChr, vec2_init(0, 0), 0))
        pTargetChr->m_Vel = clamp_vel(pTargetChr->m_MoveRestrictions, pTargetChr->m_Vel);
    }

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
    if (!pProj->m_Bouncing && !pProj->m_Freeze) {
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
  // Both (int)component >> 5 conversions in one convert/shift pair, and
  // m_BlockPos (two adjacent 32-bit fields) written with a single 8-byte store.
  const __m128i Cell = _mm_srai_epi32(_mm_cvttps_epi32(pCore->m_Pos), 5);
  const int x = _mm_cvtsi128_si32(Cell);
  const int y = _mm_extract_epi32(Cell, 1);
  _mm_storel_epi64((__m128i *)&pCore->m_BlockPos, Cell);
  const int Idx = y * pCore->m_pCollision->m_MapData.width + x;
  pCore->m_BlockIdx = Idx;
  pCore->m_BlockInfo = pCore->m_pCollision->m_pTileInfos[Idx];
}

static SPickupCooldownList *cc_pickup_cooldown_list(SCharacterCore *pCore) {
  SWorldCore *pWorld = pCore->m_pWorld;
  if (!pWorld->m_pPickupCooldowns) {
    pWorld->m_pPickupCooldowns = calloc((size_t)pWorld->m_NumCharacters, sizeof(SPickupCooldownList));
    if (!pWorld->m_pPickupCooldowns)
      return NULL;
  }
  return &pWorld->m_pPickupCooldowns[pCore->m_Id];
}

static bool cc_pickup_on_cooldown(SCharacterCore *pCore, int Key) {
  if (!pCore->m_pWorld->m_pPickupCooldowns)
    return false;
  SPickupCooldownList *pList = &pCore->m_pWorld->m_pPickupCooldowns[pCore->m_Id];

  for (int Index = 0; Index < pList->m_NumEntries;) {
    SPickupCooldown *pCooldown = &pList->m_pEntries[Index];
    if (pCore->m_pWorld->m_GameTick > pCooldown->m_EndTick) {
      pList->m_pEntries[Index] = pList->m_pEntries[--pList->m_NumEntries];
      continue;
    }
    if (pCooldown->m_Key == Key)
      return true;
    ++Index;
  }
  return false;
}

static void cc_set_pickup_cooldown(SCharacterCore *pCore, int Key) {
  SPickupCooldownList *pList = cc_pickup_cooldown_list(pCore);
  if (!pList)
    return;

  if (pList->m_NumEntries == pList->m_Capacity) {
    const int NewCapacity = pList->m_Capacity ? pList->m_Capacity * 2 : 8;
    SPickupCooldown *pNewEntries = realloc(pList->m_pEntries, (size_t)NewCapacity * sizeof(SPickupCooldown));
    if (!pNewEntries)
      return;
    pList->m_pEntries = pNewEntries;
    pList->m_Capacity = NewCapacity;
  }
  pList->m_pEntries[pList->m_NumEntries++] = (SPickupCooldown){.m_Key = Key, .m_EndTick = pCore->m_pWorld->m_GameTick + 15 * GAME_TICK_SPEED};
}

void cc_do_pickup(SCharacterCore *pCore) {
  if (!(pCore->m_BlockInfo & INFO_PICKUPNEXT))
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
        const int MapIndex = Idx + iclamp(ix + dx, 0, Width - 1);
        // NOTE: doing a copy here should be faster since it is only 3 bytes
        const SPickup Pickup = !i ? pCore->m_pCollision->m_pPickups[MapIndex] : pCore->m_pCollision->m_pFrontPickups[MapIndex];
        if (Pickup.m_Type < 0)
          continue;
        if (pCore->m_pWorld->m_UniqueRace &&
            ((Pickup.m_Type == POWERUP_WEAPON && Pickup.m_Subtype != WEAPON_GRENADE) || Pickup.m_Type == POWERUP_NINJA))
          continue;
        if (vdistance(pCore->m_Pos, vec2_init(((ix + dx) * 32) + 16, ((iy + dy) * 32) + 16)) >= 48)
          continue;
        if (Pickup.m_Number > 0 && !pCore->m_pWorld->m_pSwitches[Pickup.m_Number].m_Status)
          continue;
        const bool HealthAndAmmo = pCore->m_pWorld->m_pConfig->m_SvHealthAndAmmo;
        const int CooldownKey = MapIndex * 2 + i;
        if (HealthAndAmmo && cc_pickup_on_cooldown(pCore, CooldownKey))
          continue;

        bool Consumed = false;

        switch (Pickup.m_Type) {
        case POWERUP_HEALTH:
          if (HealthAndAmmo) {
            if (pCore->m_Health < 10) {
              ++pCore->m_Health;
              Consumed = true;
            }
          } else {
            cc_freeze(pCore, pCore->m_pWorld->m_pConfig->m_SvFreezeDelay);
          }
          break;

        case POWERUP_ARMOR:
          if (HealthAndAmmo) {
            if (pCore->m_Armor < 10) {
              ++pCore->m_Armor;
              Consumed = true;
            }
            break;
          }
#pragma clang loop unroll(full)
          for (int j = WEAPON_SHOTGUN; j < NUM_WEAPONS; j++) {
            pCore->m_aWeaponGot[j] = false;
            pCore->m_aWeaponAmmo[j] = 0;
          }
          pCore->m_Ninja.m_ActivationDir = vec2_init(0, 0);
          pCore->m_Ninja.m_ActivationTick = -500;
          pCore->m_Ninja.m_CurrentMoveTime = 0;
          if (pCore->m_ActiveWeapon >= WEAPON_SHOTGUN)
            pCore->m_ActiveWeapon = WEAPON_HAMMER;
          break;

        case POWERUP_ARMOR_SHOTGUN:
          pCore->m_aWeaponGot[WEAPON_SHOTGUN] = false;
          pCore->m_aWeaponAmmo[WEAPON_SHOTGUN] = 0;
          if (pCore->m_ActiveWeapon == WEAPON_SHOTGUN)
            pCore->m_ActiveWeapon = WEAPON_HAMMER;
          break;

        case POWERUP_ARMOR_GRENADE:
          pCore->m_aWeaponGot[WEAPON_GRENADE] = false;
          pCore->m_aWeaponAmmo[WEAPON_GRENADE] = 0;
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
          pCore->m_aWeaponAmmo[WEAPON_LASER] = 0;
          if (pCore->m_ActiveWeapon == WEAPON_LASER)
            pCore->m_ActiveWeapon = WEAPON_HAMMER;
          break;

        case POWERUP_WEAPON:
          if (!HealthAndAmmo || !pCore->m_aWeaponGot[Pickup.m_Subtype] || pCore->m_aWeaponAmmo[Pickup.m_Subtype] != 10) {
            pCore->m_aWeaponGot[Pickup.m_Subtype] = true;
            pCore->m_aWeaponAmmo[Pickup.m_Subtype] = HealthAndAmmo && Pickup.m_Subtype == WEAPON_GRENADE ? 10 : -1;
            Consumed = HealthAndAmmo;
          }
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
        if (Consumed)
          cc_set_pickup_cooldown(pCore, CooldownKey);
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
  pCore->m_TuningBlockIdx = -1;
  pCore->m_pTuning = &pWorld->m_pTunings[0];
  pCore->m_aWeaponGot[0] = true;
  pCore->m_aWeaponGot[1] = true;
  pCore->m_aWeaponAmmo[WEAPON_HAMMER] = -1;
  pCore->m_aWeaponAmmo[WEAPON_GUN] = -1;
  pCore->m_ActiveWeapon = WEAPON_GUN;
  pCore->m_Input.m_TargetY = -1;
  pCore->m_SpawnGeneration = 1;

  pCore->m_StartTick = -1;
  pCore->m_FinishTick = -1;
  pCore->m_RaceTime = -1.0f;
  pCore->m_LastTimeCp = -1;
  pCore->m_FastcapStartTeam = -1;
  pCore->m_DamageTick = -GAME_TICK_SPEED;

  if (pWorld->m_UniqueRace) {
    pCore->m_Health = 10;
    pCore->m_Armor = pWorld->m_pConfig->m_SvHealthAndAmmo && !pWorld->m_pConfig->m_SvFastcap ? 0 : 10;
    if (pWorld->m_pConfig->m_SvFastcap && !pWorld->m_pConfig->m_SvNoWeapons) {
      pCore->m_aWeaponGot[WEAPON_GRENADE] = true;
      pCore->m_aWeaponAmmo[WEAPON_GRENADE] = 10;
      pCore->m_ActiveWeapon = WEAPON_GRENADE;
    }
  }

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

void cc_quantize(SCharacterCore *pCore) {
  // Common constants
  __m128 half = _mm_set1_ps(0.5f);
  __m128 zero = _mm_setzero_ps();
  __m128 scale = _mm_set1_ps(256.0f);
  // 256 is a power of two, so 1/256 is exact and multiplying by it gives
  // bit-identical results to dividing, without the ~14 cycle divps.
  __m128 inv_scale = _mm_set1_ps(1.0f / 256.0f);

  // Quantize m_Pos
  __m128 pos = pCore->m_Pos;
  __m128 pos_plus_half = _mm_add_ps(pos, half);
  __m128i pos_int = _mm_cvttps_epi32(pos_plus_half);
  __m128 pos_rounded = _mm_cvtepi32_ps(pos_int);
  pCore->m_Pos = pos_rounded;

  // Quantize m_Vel
  __m128 vel = pCore->m_Vel;
  __m128 vel_scaled = _mm_mul_ps(vel, scale);
  __m128 adjusted_vel = _mm_blendv_ps(_mm_sub_ps(vel_scaled, half), _mm_add_ps(vel_scaled, half), _mm_cmpge_ps(vel_scaled, zero));
  __m128i vel_int = _mm_cvttps_epi32(adjusted_vel);
  __m128 vel_rounded = _mm_cvtepi32_ps(vel_int);
  pCore->m_Vel = _mm_mul_ps(vel_rounded, inv_scale);

  // While the hook is idle both of these are already snapped: m_HookPos was
  // assigned the previous tick's already-quantised m_Pos back in cc_pre_tick, and
  // m_HookDir is only ever written when the hook fires or teleports. Quantisation
  // is idempotent, so re-snapping them every tick is pure work.
  if (pCore->m_HookState != HOOK_IDLE && pCore->m_HookState != HOOK_RETRACTED) {
    // Quantize m_HookPos
    __m128 hook_pos = pCore->m_HookPos;
    __m128 hook_pos_plus_half = _mm_add_ps(hook_pos, half);
    __m128i hook_pos_int = _mm_cvttps_epi32(hook_pos_plus_half);
    __m128 hook_pos_rounded = _mm_cvtepi32_ps(hook_pos_int);
    pCore->m_HookPos = hook_pos_rounded;
    // Quantize m_HookDir
    __m128 hook_dir = pCore->m_HookDir;
    __m128 hook_dir_scaled = _mm_mul_ps(hook_dir, scale);
    __m128 adjusted_hook_dir =
        _mm_blendv_ps(_mm_sub_ps(hook_dir_scaled, half), _mm_add_ps(hook_dir_scaled, half), _mm_cmpge_ps(hook_dir_scaled, zero));
    __m128i hook_dir_int = _mm_cvttps_epi32(adjusted_hook_dir);
    __m128 hook_dir_rounded = _mm_cvtepi32_ps(hook_dir_int);
    pCore->m_HookDir = _mm_mul_ps(hook_dir_rounded, inv_scale);
  }

  cc_calc_indices(pCore);
}

void wc_release_hooked(SWorldCore *pCore, int Id);
void cc_die(SCharacterCore *pCore) {
  int Id = pCore->m_Id;
  const uint32_t SpawnGeneration = pCore->m_SpawnGeneration;
  mvec2 PrevPos = pCore->m_Pos;
  if (pCore->m_pWorld->m_pPickupCooldowns && Id >= 0 && Id < pCore->m_pWorld->m_NumCharacters) {
    SPickupCooldownList *pList = &pCore->m_pWorld->m_pPickupCooldowns[Id];
    free(pList->m_pEntries);
    *pList = (SPickupCooldownList){0};
  }
  cc_init(pCore, pCore->m_pWorld);
  pCore->m_SpawnGeneration = SpawnGeneration + 1;

  mvec2 SpawnPos;
  if (wc_next_spawn(pCore->m_pWorld, &SpawnPos, Id)) {
    if (pCore->m_pWorld->particle)
      pCore->m_pWorld->particle(PrevPos, PARTICLE_TYPE_PLAYER_DEATH, Id, pCore->m_pWorld->user_data);
    if (pCore->m_pWorld->particle)
      pCore->m_pWorld->particle(SpawnPos, PARTICLE_TYPE_PLAYER_SPAWN, Id, pCore->m_pWorld->user_data);
    emit_sound(pCore->m_pWorld, PrevPos, SOUND_TYPE_PLAYER_DIE, Id);
    emit_sound(pCore->m_pWorld, SpawnPos, SOUND_TYPE_PLAYER_SPAWN, Id);

    pCore->m_Pos = SpawnPos;
    pCore->m_PrevPos = SpawnPos;
    cc_calc_indices(pCore);
  }
  wc_release_hooked(pCore->m_pWorld, Id);
  if (pCore->m_pWorld->m_pConfig->m_SvDestroyBulletsOnDeath)
    wc_remove_entities_from_player(pCore->m_pWorld, Id);

  pCore->m_RespawnDelay = 25;
  pCore->m_ReloadTimer = 10;
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
  // The velramp test used to depend on the square root, putting ~15 cycles of
  // sqrtss latency at the head of every tick's dependency chain for a branch that
  // is almost never taken (it needs |Vel| * 50 >= VelrampStart, i.e. |Vel| >= 11).
  // Testing in squared space resolves the branch straight out of the dot product;
  // m_VelMag is only ever read by callers, so its square root now retires in
  // parallel instead of gating everything behind it.
  const float SqVel = vsqlength(pCore->m_Vel);
  const float VelrampStart = pCore->m_pTuning->m_VelrampStart;
  pCore->m_VelMag = sqrtf(SqVel);
  float OldVel = vgetx(pCore->m_Vel);

  float RampValue = 1.f;
  if (SqVel * (50.0f * 50.0f) >= VelrampStart * VelrampStart) {
    float t = pCore->m_VelMag * 50 - VelrampStart;
    RampValue = fast_expf(-t * pCore->m_pTuning->m_VelrampValue);
  }
  pCore->m_VelRamp = RampValue;

  OldVel = OldVel * RampValue;
  pCore->m_Vel = vsetx(pCore->m_Vel, OldVel);

  mvec2 NewPos = pCore->m_Pos;
  bool Grounded = false;
  mvec2 MaxNewPos = vvadd(NewPos, pCore->m_Vel);

  // OOB of the map
  const mvec2 OutOfBounds =
      _mm_or_ps(_mm_cmplt_ps(MaxNewPos, _mm_set1_ps(HALFPHYSICALSIZE + 2)), _mm_cmpge_ps(MaxNewPos, pCore->m_pCollision->m_MapMaxPos));
  if (_mm_movemask_ps(OutOfBounds) & 3) {
    cc_die(pCore);
    return;
  }

  pCore->m_Vel = vvclamp(pCore->m_Vel, vec2_init(-4 * 32, -4 * 32), vec2_init(4 * 32, 4 * 32));

  move_box(pCore->m_pCollision, NewPos, pCore->m_Vel, &NewPos, &pCore->m_Vel, &Grounded);

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
  SCharacterCore *pOther = &pCore->m_pWorld->m_pCharacters[Id];
  if (pCore->m_Solo || pOther->m_Solo)
    return;
  if ((pCore->m_CollisionDisabled || pOther->m_CollisionDisabled || !pCore->m_pTuning->m_PlayerCollision || !pOther->m_pTuning->m_PlayerCollision))
    return;

  mvec2 Pos = pOther->m_Pos;
  float Distance = vdistance(pCore->m_Pos, Pos);
  if (Distance > 0) {
    mvec2 Dir = vnormalize(vvsub(pCore->m_Pos, Pos));

    if (Distance < PHYSICALSIZE * 1.25f) {
      float a = (PHYSICALSIZE * 1.45f - Distance);
      float Velocity = 0.5f;

      if (vlength(pCore->m_Vel) > 0.0001f)
        Velocity = 1 - (vdot(vnormalize_nomask(pCore->m_Vel), Dir) + 1) / 2;

      pCore->m_Vel = vvadd(pCore->m_Vel, vfmul(Dir, a * (Velocity * 0.75f * 0.85f)));
      ++*pCollisions;
    }
  } else {
    if (vgetx(pCore->m_PrevPos) == vgetx(pCore->m_Pos) && vgety(pCore->m_PrevPos) == vgety(pCore->m_Pos)) {
      unsigned int seed = (uint32_t)((uint32_t)pCore->m_Id + (uint32_t)Id ^ 0x1234567) ^ (uint32_t)pCore->m_pWorld->m_GameTick;
      pCore->m_Vel = vvadd(pCore->m_Vel, vec2_init((fast_rand(&seed) - fast_rand(&seed)) * 0.5f, (fast_rand(&seed) - fast_rand(&seed)) * 0.5f));
    } else {
      pCore->m_Vel = vvadd(pCore->m_Vel, vnormalize_nomask(vvsub(pCore->m_PrevPos, pCore->m_Pos)));
    }
  }
}

void cc_tick_deferred(SCharacterCore *pCore) {
  if (pCore->m_pWorld->m_NumCharacters > 1) {
    int Num = 0;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        // TODO: use the block idx +- 1 +- map_width
        int Idx = (((int)vgety(pCore->m_Pos) >> 5) + dy) * pCore->m_pCollision->m_MapData.width + (((int)vgetx(pCore->m_Pos) >> 5) + dx);
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

  if (pCore->m_TuningBlockIdx != pCore->m_BlockIdx) {
    pCore->m_TuningBlockIdx = pCore->m_BlockIdx;
    pCore->m_pTuning = &pCore->m_pWorld->m_pTunings[is_tune(pCore->m_pCollision, pCore->m_BlockIdx)];
  }
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

  if (pCore->m_pCollision->m_pTileInfos[Index] & INFO_ISSPEEDUP) {
    mvec2 Direction, TempVel = pCore->m_Vel;
    int Force, Type, MaxSpeed = 0;
    get_speedup(pCore->m_pCollision, Index, &Direction, &Force, &MaxSpeed, &Type);

    if (Type == TILE_SPEED_BOOST_OLD) {
      float SpeedLeft;
      if (Force == 255 && MaxSpeed) {
        pCore->m_Vel = vfmul(Direction, ((float)MaxSpeed / 5.f));
      } else {
        if (MaxSpeed > 0 && MaxSpeed < 5)
          MaxSpeed = 5;
        if (MaxSpeed > 0) {
          SpeedLeft = MaxSpeed / 5.0f - vdot(speedup_signed(Direction), speedup_signed(TempVel));
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
        // Folded at init: depends only on the velramp tunings, and the logf() was
        // being paid on every boost tile hit.
        MaxSpeed = pCore->m_pTuning->m_SpeedupDefaultMaxSpeed;
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
  memset(pCore->m_aWeaponAmmo + WEAPON_SHOTGUN, 0, 4);
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
  if (pCore->m_pWorld->m_UniqueRace && pCore->m_StartTick != -1) {
    --pCore->m_StartTime;
    pCore->m_StartTickOffset -= pCore->m_TileFraction;
  }
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
  if (pCore->m_pWorld->m_UniqueRace && pCore->m_StartTick != -1) {
    --pCore->m_StartTime;
    pCore->m_StartTickOffset -= pCore->m_TileFraction;
  }

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
      if (pCore->m_pWorld->m_UniqueRace && pCore->m_StartTick != -1) {
        --pCore->m_StartTime;
        pCore->m_StartTickOffset -= pCore->m_TileFraction;
      }

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
      if (pCore->m_pWorld->m_UniqueRace && pCore->m_StartTick != -1) {
        --pCore->m_StartTime;
        pCore->m_StartTickOffset -= pCore->m_TileFraction;
      }

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

void cc_handle_tiles(SCharacterCore *pCore, int Index, float FractionOfTick) {
  int MapIndex = Index;
  pCore->m_IsInFreeze = false;
  pCore->m_TileFraction = FractionOfTick;

  if (Index < 0) {
    pCore->m_LastRefillJumps = false;
    pCore->m_LastPenalty = false;
    pCore->m_LastBonus = false;
    return;
  }

  int TileIndex = get_tile_index(pCore->m_pCollision, MapIndex);
  int TileFIndex = pCore->m_pCollision->m_MapData.front_layer.data ? get_front_tile_index(pCore->m_pCollision, MapIndex) : 0;

  const int TimeCpTiles[2] = {TileIndex, TileFIndex};
  for (int Layer = 0; Layer < 2; ++Layer) {
    const int Tile = TimeCpTiles[Layer];
    if (Tile < TILE_TIME_CHECKPOINT_FIRST || Tile > TILE_TIME_CHECKPOINT_LAST || pCore->m_StartTick < 0)
      continue;
    const int TimeCp = Tile - TILE_TIME_CHECKPOINT_FIRST;
    const uint32_t Bit = (uint32_t)1 << TimeCp;
    if (pCore->m_TimeCpMask & Bit)
      continue;
    pCore->m_TimeCpMask |= Bit;
    pCore->m_LastTimeCp = TimeCp;
    pCore->m_aTimeCp[TimeCp] =
        ((float)pCore->m_pWorld->m_GameTick + FractionOfTick - (float)pCore->m_StartTime - pCore->m_StartTickOffset) / GAME_TICK_SPEED;
  }

  // teleport checkpoint
  if (pCore->m_pCollision->m_MapData.tele_layer.type) {
    int TeleCheckpoint = is_tele_checkpoint(pCore->m_pCollision, MapIndex);
    if (TeleCheckpoint)
      pCore->m_TeleCheckpoint = TeleCheckpoint;
  }

  // use the dispatch table for O(1) lookup. Slot 0 (and every tile without a
  // handler) holds cc_handle_tile_empty, so guarding on a non-zero tile skips a
  // hard-to-predict indirect call for air, which is what almost every sampled
  // tile is.
  if (TileIndex)
    g_apGameTileHandlers[TileIndex](pCore);
  // don't do it twice for the same tile
  if (TileFIndex && TileFIndex != TileIndex)
    g_apGameTileHandlers[TileFIndex](pCore);

  // this logic must run after the handlers
  pCore->m_LastRefillJumps = (TileIndex == TILE_REFILL_JUMPS) | (TileFIndex == TILE_REFILL_JUMPS);

  // m_IsInFreeze was cleared on entry, so this can assign the OR directly
  // instead of branching four times to maybe set it.
  pCore->m_IsInFreeze = (TileIndex == TILE_FREEZE) | (TileFIndex == TILE_FREEZE) | (TileIndex == TILE_DFREEZE) | (TileFIndex == TILE_DFREEZE);

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

    // use the dispatch table for O(1) lookup; slot 0 is cc_handle_switch_empty
    if (Type)
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
  // Same four (int)f (+1) >> 5 conversions as before, done in lanes.
  const __m128i MinI = _mm_srai_epi32(_mm_cvttps_epi32(_mm_min_ps(Start, End)), 5);
  const __m128i MaxI = _mm_srai_epi32(_mm_add_epi32(_mm_cvttps_epi32(_mm_max_ps(Start, End)), _mm_set1_epi32(1)), 5);
  const __m128i DiffI = _mm_sub_epi32(MaxI, MinI);
  const int MinX = _mm_cvtsi128_si32(MinI);
  const int MinY = _mm_extract_epi32(MinI, 1);

  return (bool)(pCollision->m_pBroadIndicesBitField[(MinY * pCollision->m_MapData.width) + MinX] &
                (uint64_t)1 << ((_mm_extract_epi32(DiffI, 1) << 3) + _mm_cvtsi128_si32(DiffI)));
}

static void cc_unique_race_start(SCharacterCore *pCore, float FractionOfTick) {
  if (pCore->m_StartTick != -1)
    return;
  pCore->m_StartTick = pCore->m_pWorld->m_GameTick;
  pCore->m_StartTime = pCore->m_StartTick;
  pCore->m_FinishTick = -1;
  pCore->m_StartTickOffset = FractionOfTick;
  pCore->m_FinishTickOffset = 0.0f;
  pCore->m_RaceTime = -1.0f;
  memset(pCore->m_aTimeCp, 0, sizeof(pCore->m_aTimeCp));
  pCore->m_TimeCpMask = 0;
  pCore->m_LastTimeCp = -1;
}

static void cc_unique_race_finish(SCharacterCore *pCore, float FractionOfTick) {
  if (pCore->m_StartTick == -1 || pCore->m_FinishTick != -1)
    return;
  pCore->m_FinishTick = pCore->m_pWorld->m_GameTick;
  pCore->m_FinishTickOffset = FractionOfTick;
  pCore->m_RaceTime = (FractionOfTick - pCore->m_StartTickOffset + (float)(pCore->m_FinishTick - pCore->m_StartTime)) / GAME_TICK_SPEED;
  if (pCore->m_pWorld->particle)
    pCore->m_pWorld->particle(pCore->m_Pos, PARTICLE_TYPE_CONFETTI, pCore->m_Id, pCore->m_pWorld->user_data);
}

static void cc_handle_fastcap(SCharacterCore *pCore, mvec2 PrevPos, mvec2 Pos) {
  if (!pCore->m_pWorld->m_pConfig->m_SvFastcap)
    return;

  const float Distance = vdistance(PrevPos, Pos);
  if (Distance == 0.0f)
    return;

  // Unique Race checks the path in one-world-unit increments. Keeping the
  // same fractions is important because they are part of the finish time.
  const int End = (int)Distance + 1;
  for (int Index = 0; Index < End; ++Index) {
    const float FractionOfTick = (float)Index / Distance;
    const mvec2 SamplePos = vvfmix(PrevPos, Pos, FractionOfTick);
    for (int Team = 0; Team < 2; ++Team) {
      const int OppositeTeam = Team == 0 ? 1 : 0;
      if (!pCore->m_pCollision->m_aFastcapFlagPresent[Team] || pCore->m_aGotFastcapFlag[Team])
        continue;
      if (vdistance(SamplePos, pCore->m_pCollision->m_aFastcapFlagPositions[Team]) >= HALFPHYSICALSIZE + PHYSICALSIZE)
        continue;

      if (!pCore->m_aGotFastcapFlag[OppositeTeam]) {
        pCore->m_FastcapStartTeam = Team;
        cc_unique_race_start(pCore, FractionOfTick);
      } else
        cc_unique_race_finish(pCore, FractionOfTick);
      pCore->m_aGotFastcapFlag[Team] = true;
    }
  }
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

  if (pCore->m_BlockInfo & (INFO_CANHITKILL | INFO_ISSPEEDUP))
    cc_handle_skippable_tiles(pCore, pCore->m_BlockIdx);

  const mvec2 PrevPos = pCore->m_PrevPos;
  const mvec2 Pos = pCore->m_Pos;
  const int Width = pCore->m_pCollision->m_MapData.width;
  cc_handle_fastcap(pCore, PrevPos, Pos);
  if (broad_indices_check(pCore->m_pCollision, PrevPos, Pos)) {
    if (pCore->m_pWorld->m_UniqueRace) {
      const float Distance = vdistance(PrevPos, Pos);
      int LastIndex = 0;
      bool HandledTile = false;
      const uint32_t SpawnGeneration = pCore->m_SpawnGeneration;
      if (Distance == 0.0f) {
        const int Index = pCore->m_BlockIdx;
        if (pCore->m_pCollision->m_pTileInfos[Index] & INFO_TILENEXT) {
          cc_handle_tiles(pCore, Index, 0.0f);
          HandledTile = true;
        }
      } else {
        const int End = (int)ceilf(Distance);
        for (int Sample = 0; Sample < End; ++Sample) {
          const float FractionOfTick = (float)Sample / Distance;
          const mvec2 SamplePos = vvfmix(PrevPos, Pos, FractionOfTick);
          const int x = iclamp((int)vgetx(SamplePos) / 32, 0, pCore->m_pCollision->m_MapData.width - 1);
          const int y = iclamp((int)vgety(SamplePos) / 32, 0, pCore->m_pCollision->m_MapData.height - 1);
          const int Index = y * Width + x;
          if (!(pCore->m_pCollision->m_pTileInfos[Index] & INFO_TILENEXT) || LastIndex == Index)
            continue;
          cc_handle_tiles(pCore, Index, FractionOfTick);
          HandledTile = true;
          LastIndex = Index;
          if (pCore->m_SpawnGeneration != SpawnGeneration)
            return;
        }
      }
      if (!HandledTile)
        cc_handle_tiles(pCore, pCore->m_BlockIdx, 1.0f);
      goto TilesHandled;
    }

    int sx = (int)vgetx(PrevPos) >> 5;
    int sy = (int)vgety(PrevPos) >> 5;
    int ex = (int)vgetx(Pos) >> 5;
    int ey = (int)vgety(Pos) >> 5;

    int x = sx;
    int y = sy;
    float dx = vgetx(Pos) - vgetx(PrevPos);
    float dy = vgety(Pos) - vgety(PrevPos);

    int StepX = (dx > 0) ? 1 : -1;
    int StepY = (dy > 0) ? 1 : -1;

    float tMaxX = (dx > 0) ? ((x + 1) * 32.0f - vgetx(PrevPos)) / dx : (vgetx(PrevPos) - x * 32.0f) / -dx;
    if (dx == 0)
      tMaxX = FLT_MAX;
    float tMaxY = (dy > 0) ? ((y + 1) * 32.0f - vgety(PrevPos)) / dy : (vgety(PrevPos) - y * 32.0f) / -dy;
    if (dy == 0)
      tMaxY = FLT_MAX;

    float tDeltaX = (dx != 0) ? fabs(32.0f / dx) : FLT_MAX;
    float tDeltaY = (dy != 0) ? fabs(32.0f / dy) : FLT_MAX;

    cc_handle_tiles(pCore, y * Width + x, 1.0f);
    while (x != ex || y != ey) {
      if (x != ex && y != ey && tMaxX == tMaxY) {
        tMaxX += tDeltaX;
        tMaxY += tDeltaY;
        x += StepX;
        y += StepY;
      } else if (x != ex && (y == ey || tMaxX < tMaxY)) {
        tMaxX += tDeltaX;
        x += StepX;
      } else {
        tMaxY += tDeltaY;
        y += StepY;
      }
      cc_handle_tiles(pCore, y * Width + x, 1.0f);
    }
  }
TilesHandled:
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

  bool Grounded = false;
  if (pCore->m_BlockInfo & INFO_CANGROUND) {
    // [x, x, y, y] + [+14, -14, +19, +19] gives the three probe coordinates in
    // one register, so the three (int)(f + 0.5f) >> 5 roundings collapse into a
    // single add/convert/shift instead of three scalar chains.
    const __m128 Spread = _mm_shuffle_ps(pCore->m_Pos, pCore->m_Pos, _MM_SHUFFLE(1, 1, 0, 0));
    const __m128 Probes = _mm_add_ps(Spread, _mm_set_ps(HALFPHYSICALSIZE + 5, HALFPHYSICALSIZE + 5, -HALFPHYSICALSIZE, HALFPHYSICALSIZE));
    const __m128i Cells = _mm_srai_epi32(_mm_cvttps_epi32(_mm_add_ps(Probes, _mm_set1_ps(0.5f))), 5);
    const int GroundRightX = _mm_cvtsi128_si32(Cells);
    const int GroundLeftX = _mm_extract_epi32(Cells, 1);
    const int GroundY = _mm_extract_epi32(Cells, 2);
    const int GroundRow = pCore->m_pCollision->m_pWidthLookup[GroundY];
    Grounded =
        (pCore->m_pCollision->m_pTileInfos[GroundRow + GroundRightX] | pCore->m_pCollision->m_pTileInfos[GroundRow + GroundLeftX]) & INFO_ISSOLID;
  }

  pCore->m_Vel = vadd_y(pCore->m_Vel, pCore->m_pTuning->m_Gravity);
  pCore->m_Grounded = Grounded;
  if (pCore->m_Input.m_Jump) {
    if (!(pCore->m_Jumped & 1)) {
      if (Grounded && (!(pCore->m_Jumped & 2) || pCore->m_Jumps != 0)) {
        pCore->m_Vel = vsety(pCore->m_Vel, -pCore->m_pTuning->m_GroundJumpImpulse);
        emit_sound(pCore->m_pWorld, pCore->m_Pos, SOUND_TYPE_PLAYER_JUMP, pCore->m_Id);
        if (pCore->m_Jumps > 1) {
          pCore->m_Jumped |= 1;
        } else {
          pCore->m_Jumped |= 3;
        }
        pCore->m_JumpedTotal = 0;
      } else if (!(pCore->m_Jumped & 2)) {
        if (pCore->m_pWorld->particle)
          pCore->m_pWorld->particle(pCore->m_Pos, PARTICLE_TYPE_AIR_JUMP, pCore->m_Id, pCore->m_pWorld->user_data);
        emit_sound(pCore->m_pWorld, pCore->m_Pos, SOUND_TYPE_PLAYER_AIRJUMP, pCore->m_Id);

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
      pCore->m_HookPos = vvadd(pCore->m_Pos, vfmul(vfmul(TargetDirection, PHYSICALSIZE), 1.5f));
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
      emit_sound(pCore->m_pWorld, pCore->m_Pos, SOUND_TYPE_HOOK_NOATTACH, pCore->m_Id);
    }
    // NOTE: this only really matters at the edge of the map but since we offset maps by 200 block idk if it actually matters. might remove this if it
    // ends up being a hot path. same for this logic in laser bounce
    //
    // It is a hot path once the tee actually moves: four CLIP evaluations, each
    // with a division, on every hook-flying tick. t1 is only ever reduced when a
    // segment endpoint leaves the clip box, so when both endpoints are inside the
    // whole block is a no-op and can be skipped. Bit-exact: the skipped path
    // provably leaves t1 == 1.0f, which is the case the code already treats as
    // "leave NewPos alone".
    if (segment_leaves_map(pCore->m_pCollision, pCore->m_HookPos, NewPos)) {
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

      if (t1 != 1.0f)
        NewPos = vec2_init(x0 + dx * t1, y0 + dy * t1);
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
          if (CurrentX != EndX) {
            tMaxX += tDeltaX;
            CurrentX += StepX;
          } else {
            tMaxX = FLT_MAX;
          }
        } else {
          if (CurrentY != EndY) {
            tMaxY += tDeltaY;
            CurrentY += StepY;
          } else {
            tMaxY = FLT_MAX;
          }
        }
      }

      if (pCore->m_HookedPlayer != -1) {
        pCore->m_HookState = HOOK_GRABBED;
        emit_sound(pCore->m_pWorld, pCore->m_Pos, SOUND_TYPE_HOOK_ATTACH_PLAYER, pCore->m_Id);
      }
    }

    if (pCore->m_HookState == HOOK_FLYING) {
      if (GoingToHitGround) {
        pCore->m_HookState = HOOK_GRABBED;
        emit_sound(pCore->m_pWorld, pCore->m_Pos, SOUND_TYPE_HOOK_ATTACH_GROUND, pCore->m_Id);
      } else if (GoingToRetract) {
        pCore->m_HookState = HOOK_RETRACT_START;
        emit_sound(pCore->m_pWorld, pCore->m_Pos, SOUND_TYPE_HOOK_NOATTACH, pCore->m_Id);
      }
      int NumOuts = pCore->m_pCollision->m_aNumTeleOuts[teleNr];
      if (GoingThroughTele && NumOuts > 0) {
        pCore->m_HookedPlayer = -1;
        mvec2 TargetDirection = vnormalize(vec2_init(pCore->m_Input.m_TargetX, pCore->m_Input.m_TargetY));
        pCore->m_NewHook = true;
        pCore->m_HookPos =
            vvadd(pCore->m_pCollision->m_apTeleOuts[teleNr][pCore->m_Input.m_TeleOut % NumOuts], vfmul(vfmul(TargetDirection, PHYSICALSIZE), 1.5f));
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

bool cc_take_damage(SCharacterCore *pCore, mvec2 Force, int Damage) {
  const mvec2 NewVelocity = vvadd(pCore->m_Vel, Force);
  pCore->m_Vel = pCore->m_pWorld->m_UniqueRace ? NewVelocity : clamp_vel(pCore->m_MoveRestrictions, NewVelocity);
  if (!pCore->m_pWorld->m_pConfig->m_SvHealthAndAmmo)
    return true;

  Damage = imax(1, Damage / 2);
  const int OldHealth = pCore->m_Health;
  const int OldArmor = pCore->m_Armor;
  if (pCore->m_Armor) {
    if (Damage > 1) {
      --pCore->m_Health;
      --Damage;
    }
    if (Damage > pCore->m_Armor) {
      Damage -= pCore->m_Armor;
      pCore->m_Armor = 0;
    } else {
      pCore->m_Armor -= Damage;
      Damage = 0;
    }
  }
  pCore->m_Health -= Damage;
  const int DamageTaken = OldHealth - pCore->m_Health + OldArmor - pCore->m_Armor;
  if (DamageTaken > 0) {
    float IndicatorAngle = 0.0f;
    pCore->m_DamageTaken++;
    if (pCore->m_pWorld->m_GameTick < pCore->m_DamageTick + GAME_TICK_SPEED / 2)
      IndicatorAngle = pCore->m_DamageTaken * 0.25f;
    else
      pCore->m_DamageTaken = 0;
    pCore->m_DamageTick = pCore->m_pWorld->m_GameTick;
    if (pCore->m_pWorld->damage_indicator)
      pCore->m_pWorld->damage_indicator(pCore->m_Pos, IndicatorAngle, DamageTaken, pCore->m_Id, pCore->m_pWorld->user_data);
  }
  if (pCore->m_Health <= 0) {
    cc_die(pCore);
    return false;
  }
  return true;
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

    {
      bool _;
      move_box(pCore->m_pCollision, pCore->m_Pos, pCore->m_Vel, &pCore->m_Pos, &pCore->m_Vel, &_);
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

          cc_take_damage(pChr, vec2_init(0, -10.f), 9);
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
  cc_take_damage(pCore, vfmul(Direction, -(Strength / 100.f / 6.11f)), 0);
}

void cc_do_weapon_switch(SCharacterCore *pCore) {
  const uint8_t WantedWeapon = imin(pCore->m_Input.m_WantedWeapon, NUM_WEAPONS - 1);
  uint64_t WeaponGot;
  memcpy(&WeaponGot, pCore->m_aWeaponGot, sizeof(WeaponGot));
  const uint8_t HasWantedWeapon = (WeaponGot >> (WantedWeapon * CHAR_BIT)) & 1;
  const uint8_t CanSwitch = (pCore->m_ReloadTimer == 0) & HasWantedWeapon & !pCore->m_aWeaponGot[WEAPON_NINJA];
  const uint8_t SwitchMask = (uint8_t)-CanSwitch;
  const uint8_t PreviousWeapon = pCore->m_ActiveWeapon;
  pCore->m_ActiveWeapon ^= (pCore->m_ActiveWeapon ^ WantedWeapon) & SwitchMask;
  if (pCore->m_ActiveWeapon != PreviousWeapon)
    emit_sound(pCore->m_pWorld, pCore->m_Pos, SOUND_TYPE_WEAPON_SWITCH, pCore->m_Id);
}

void wc_remove_entity(SWorldCore *pWorld, SEntity *pEnt);

void cc_fire_weapon(SCharacterCore *pCore) {
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
  if (pCore->m_pWorld->m_pConfig->m_SvHealthAndAmmo && pCore->m_aWeaponAmmo[pCore->m_ActiveWeapon] == 0)
    return;

  const mvec2 Direction = vnormalize_nomask(vec2_init(pCore->m_Input.m_TargetX, pCore->m_Input.m_TargetY));
  const mvec2 ProjStartPos = vvadd(pCore->m_Pos, vfmul(Direction, PHYSICALSIZE * 0.75f));
  if (vgetx(ProjStartPos) < 0 || vgety(ProjStartPos) < 0 || vgetx(ProjStartPos) >= pCore->m_pCollision->m_MapData.width * 32 ||
      vgety(ProjStartPos) >= pCore->m_pCollision->m_MapData.height * 32) {
    return;
  }

  pCore->m_AttackTick = pCore->m_pWorld->m_GameTick + 1; // cc_fire_weapon is called on on_input before the tick is increased

  static const ESoundType s_aWeaponSounds[NUM_WEAPONS] = {SOUND_TYPE_HAMMER_FIRE,  SOUND_TYPE_GUN_FIRE,   SOUND_TYPE_SHOTGUN_FIRE,
                                                          SOUND_TYPE_GRENADE_FIRE, SOUND_TYPE_LASER_FIRE, SOUND_TYPE_NINJA_FIRE};
  emit_sound(pCore->m_pWorld, pCore->m_Pos, s_aWeaponSounds[pCore->m_ActiveWeapon], pCore->m_Id);

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

        if (pCore->m_pWorld->particle) {
          if (vlength(vvsub(pTarget->m_Pos, ProjStartPos)) > 0.0f)
            pCore->m_pWorld->particle(vvsub(pTarget->m_Pos, vfmul(vnormalize(vvsub(pTarget->m_Pos, ProjStartPos)), HALFPHYSICALSIZE)),
                                      PARTICLE_TYPE_HAMMER_HIT, pCore->m_Id, pCore->m_pWorld->user_data);
          else
            pCore->m_pWorld->particle(ProjStartPos, PARTICLE_TYPE_HAMMER_HIT, pCore->m_Id, pCore->m_pWorld->user_data);
        }

        mvec2 Dir;
        if (vsqdistance(pTarget->m_Pos, pCore->m_Pos) > 0.0f)
          Dir = vnormalize(vvsub(pTarget->m_Pos, pCore->m_Pos));
        else
          Dir = vec2_init(0.f, -1.f);

        float Strength = pCore->m_pTuning->m_HammerStrength;

        mvec2 Temp = vvadd(pTarget->m_Vel, vfmul(vnormalize(vvadd(Dir, vec2_init(0.f, -1.1f))), 10.0f));
        Temp = vvsub(clamp_vel(pTarget->m_MoveRestrictions, Temp), pTarget->m_Vel);

        mvec2 Force = vfmul(vvadd(vec2_init(0.f, -1.0f), Temp), Strength);

        cc_take_damage(pTarget, Force, 3);
        cc_unfreeze(pTarget);

        Hits++;
      }
    }
    /*if (pCore->m_pWorld->m_NumCharacters > 1) {
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          int Idx = (((int)vgety(ProjStartPos) >> 5) + dx) * pCore->m_pCollision->m_MapData.width + (((int)vgetx(ProjStartPos) >> 5) + dy);
          Idx = iclamp(Idx, 0, pCore->m_pCollision->m_MapData.width * pCore->m_pCollision->m_MapData.height - 1);
          int Id = pCore->m_pWorld->m_Accelerator.m_pGrid->m_pTeeGrid[Idx];
          while (Id >= 0) {
            if (pCore->m_Id == Id) {
              Id = pCore->m_pWorld->m_Accelerator.m_pTeeList[Id].m_Child;
              continue;
            }

            if (vdistance(pCore->m_pWorld->m_pCharacters[Id].m_Pos, ProjStartPos) < HALFPHYSICALSIZE + PHYSICALSIZE) {
              SCharacterCore *pTarget = &pCore->m_pWorld->m_pCharacters[Id];

              if (pTarget == pCore || pTarget->m_Solo)
                continue;

              if (pCore->m_pWorld->eff_hammer) {
                if (vlength(vvsub(pTarget->m_Pos, ProjStartPos)) > 0.0f)
                  pCore->m_pWorld->eff_hammer(vvsub(pTarget->m_Pos, vfmul(vnormalize(vvsub(pTarget->m_Pos, ProjStartPos)), HALFPHYSICALSIZE)),
                                              pCore->m_pWorld->user_data);
                else
                  pCore->m_pWorld->eff_hammer(ProjStartPos, pCore->m_pWorld->user_data);
              }

              mvec2 Dir;
              if (vsqdistance(pTarget->m_Pos, pCore->m_Pos) > 0.0f)
                Dir = vnormalize(vvsub(pTarget->m_Pos, pCore->m_Pos));
              else
                Dir = vec2_init(0.f, -1.f);

              float Strength = pCore->m_pTuning->m_HammerStrength;

              mvec2 Temp = vvadd(pTarget->m_Vel, vfmul(vnormalize(vvadd(Dir, vec2_init(0.f, -1.1f))), 10.0f));
              Temp = vvsub(clamp_vel(pTarget->m_MoveRestrictions, Temp), pTarget->m_Vel);

              mvec2 Force = vfmul(vvadd(vec2_init(0.f, -1.0f), Temp), Strength);

              cc_take_damage(pTarget, Force, 3);
              cc_unfreeze(pTarget);

              Hits++;
            }

            Id = pCore->m_pWorld->m_Accelerator.m_pTeeList[Id].m_Child;
          }
        }
      }
    }*/

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

  if (pCore->m_pWorld->m_pConfig->m_SvHealthAndAmmo && pCore->m_aWeaponAmmo[pCore->m_ActiveWeapon] > 0)
    --pCore->m_aWeaponAmmo[pCore->m_ActiveWeapon];

  // reloadtimer can be changed earlier by hammer so check again
  if (!pCore->m_ReloadTimer) {
    pCore->m_ReloadTimer = (*(&pCore->m_pTuning->m_HammerFireDelay + pCore->m_ActiveWeapon) * GAME_TICK_SPEED) / 1000;
  }
}

void cc_handle_weapons(SCharacterCore *pCore) {
  if (pCore->m_aWeaponGot[WEAPON_NINJA])
    cc_handle_ninja(pCore);
  if (pCore->m_Jetpack && pCore->m_ActiveWeapon == WEAPON_GUN)
    cc_handle_jetpack(pCore);
  if (pCore->m_ReloadTimer) {
    --pCore->m_ReloadTimer;
    return;
  }
}

void cc_tick(SCharacterCore *pCore) {
  if (pCore->m_RespawnDelay)
    --pCore->m_RespawnDelay;

  cc_tick_deferred(pCore);

  // handle Weapons
  cc_handle_weapons(pCore);

  cc_ddrace_postcore_tick(pCore);

  pCore->m_PrevPos = pCore->m_Pos;

  pCore->m_PrevFire = pCore->m_Input.m_Fire;
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
  pCore->m_Accelerator.hash = next_world_hash();
  pCore->m_pConfig = pConfig;
  pCore->m_UniqueRace = pConfig->m_SvFastcap || pConfig->m_SvKillGrenades || pConfig->m_SvHealthAndAmmo;

  init_switchers(pCore, pCollision->m_HighestSwitchNumber);

  pCore->m_pTunings = pCollision->m_aTuningList;
  apply_map_settings(&pCollision->m_MapData, &(SMapSettingsTarget){
                                                 .m_pConfig = pConfig,
                                                 .m_pTunings = pCore->m_pTunings,
                                                 .m_pSwitches = pCore->m_pSwitches,
                                                 .m_pUniqueRace = &pCore->m_UniqueRace,
                                                 .m_NumSwitches = pCore->m_NumSwitches,
                                             });

  wc_create_all_entities(pCore);
}

static EGameMode normalize_game_mode(EGameMode GameMode) {
  if (GameMode < GAME_MODE_DDRACE || GameMode >= NUM_GAME_MODES)
    return GAME_MODE_DDRACE;
  return GameMode;
}

static void enable_unique_race_game_mode(SWorldCore *pCore, SCollision *pCollision, SConfig *pConfig) {
  pConfig->m_SvSoloServer = 1;
  pConfig->m_SvDestroyBulletsOnDeath = pConfig->m_SvKillGrenades;
  pCore->m_UniqueRace = true;

  for (int Zone = 0; Zone < NUM_TUNE_ZONES; ++Zone) {
    pCollision->m_aTuningList[Zone].m_PlayerCollision = 0.0f;
    pCollision->m_aTuningList[Zone].m_PlayerHooking = 0.0f;
  }
}

bool init_game_mode(SWorldCore *pCore, SCollision *pCollision, STeeGrid *pGrid, SConfig *pConfig, map_data_t *pMap, EGameMode GameMode) {
  GameMode = normalize_game_mode(GameMode);
  const bool NoWeapons = GameMode == GAME_MODE_FASTCAP_NO_WPNS;
  init_config(pConfig);
  pConfig->m_SvNoWeapons = NoWeapons;
  if (!init_collision_with_no_weapons(pCollision, pMap, NoWeapons))
    return false;

  *pGrid = tg_empty();
  tg_init(pGrid, pCollision->m_MapData.width, pCollision->m_MapData.height);
  wc_init(pCore, pCollision, pGrid, pConfig);
  pConfig->m_SvNoWeapons = NoWeapons;

  if (GameMode == GAME_MODE_RACE) {
    pConfig->m_SvFastcap = 0;
    enable_unique_race_game_mode(pCore, pCollision, pConfig);
  } else if (GameMode == GAME_MODE_FASTCAP || GameMode == GAME_MODE_FASTCAP_NO_WPNS) {
    enable_unique_race_game_mode(pCore, pCollision, pConfig);
    pConfig->m_SvFastcap = 1;
    pConfig->m_SvHealthAndAmmo = 1;
    pConfig->m_SvDestroyBulletsOnDeath = 1;
  } else {
    pConfig->m_SvFastcap = 0;
    pCore->m_UniqueRace = false;
  }
  return true;
}

static void wc_free_pickup_cooldowns(SWorldCore *pCore) {
  if (!pCore->m_pPickupCooldowns)
    return;
  for (int Index = 0; Index < pCore->m_NumCharacters; ++Index)
    free(pCore->m_pPickupCooldowns[Index].m_pEntries);
  free(pCore->m_pPickupCooldowns);
  pCore->m_pPickupCooldowns = NULL;
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
  wc_free_pickup_cooldowns(pCore);
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
  const int NumCharacters = pCore->m_NumCharacters;
  SCharacterCore *const pCharacters = pCore->m_pCharacters;

  // decay hitnum before new values are added to preserve outside sources seeing it.
  for (int i = 0; i < NumCharacters; ++i)
    if (pCharacters[i].m_HitNum > 0)
      --pCharacters[i].m_HitNum;

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

  // The character array is only reallocated by wc_add_character /
  // wc_remove_character, neither of which runs during a tick, so the bound and
  // the base pointer are loaded once instead of once per pass.

  for (int i = 0; i < NumCharacters; ++i)
    cc_do_pickup(&pCharacters[i]);

  // Tick characters
  if (NumCharacters > 1)
    wc_accelerator_tick(pCore);
  for (int i = 0; i < NumCharacters; ++i)
    cc_pre_tick(&pCharacters[i]);
  for (int i = 0; i < NumCharacters; ++i)
    cc_tick(&pCharacters[i]);

  // Do tick deferred
  // funny thing no other entities than the character actually have a deferred
  // tick function lol
  for (int i = 0; i < NumCharacters; ++i) {
    cc_move(&pCharacters[i]);
    cc_quantize(&pCharacters[i]); // also refreshes m_BlockIdx / m_BlockInfo
    pCharacters[i].m_MoveRestrictions = get_move_restrictions(pCore, pCharacters[i].m_Pos, pCharacters[i].m_BlockIdx);
  }

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
  if (pWorld->m_pPickupCooldowns) {
    SPickupCooldownList *pNewCooldowns = realloc(pWorld->m_pPickupCooldowns, (size_t)NewSize * sizeof(SPickupCooldownList));
    if (pNewCooldowns) {
      pWorld->m_pPickupCooldowns = pNewCooldowns;
      memset(&pWorld->m_pPickupCooldowns[OldSize], 0, (size_t)Num * sizeof(SPickupCooldownList));
    } else
      wc_free_pickup_cooldowns(pWorld);
  }

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

    if (pWorld->m_pConfig->m_SvSoloServer)
      pChar->m_Solo = true;
    if (pWorld->particle)
      pWorld->particle(SpawnPos, PARTICLE_TYPE_PLAYER_SPAWN, pChar->m_Id, pWorld->user_data);

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

// Everything a player still has in flight, dropped in one pass. Entities are
// only marked here: the end-of-tick sweep in wc_tick is what unlinks and frees
// them, so this is safe to call from inside a tick -- which is where it is
// called from, cc_die under sv_destroy_bullets_on_death.
void wc_remove_entities_from_player(SWorldCore *pWorld, int PlayerId) {
  if (!pWorld || PlayerId < 0)
    return;

  for (int i = 0; i < NUM_WORLD_ENTTYPES; ++i) {
    for (SEntity *pEnt = pWorld->m_apFirstEntityTypes[i]; pEnt; pEnt = pEnt->m_pNextTypeEntity) {
      int Owner;
      if (pEnt->m_ObjType == WORLD_ENTTYPE_PROJECTILE)
        Owner = ((SProjectile *)pEnt)->m_Owner;
      else if (pEnt->m_ObjType == WORLD_ENTTYPE_LASER)
        Owner = ((SLaser *)pEnt)->m_Owner;
      else
        continue;

      if (Owner == PlayerId)
        pEnt->m_MarkedForDestroy = true;
    }
  }
}

// TODO: remove ourselves from the grid linked list tater potator structure
void wc_remove_character(SWorldCore *pWorld, int CharacterId) {
  if (!pWorld || CharacterId < 0 || CharacterId >= pWorld->m_NumCharacters)
    return;

  // Unlink from TeeList
  // We do this before shifting indices so we access the correct current indices.
  STeeLink *pLink = &pWorld->m_Accelerator.m_pTeeList[CharacterId];
  if (pLink->m_Parent >= 0) {
    pWorld->m_Accelerator.m_pTeeList[pLink->m_Parent].m_Child = pLink->m_Child;
  }
  if (pLink->m_Child >= 0) {
    pWorld->m_Accelerator.m_pTeeList[pLink->m_Child].m_Parent = pLink->m_Parent;
  }
  if (pWorld->m_pPickupCooldowns) {
    free(pWorld->m_pPickupCooldowns[CharacterId].m_pEntries);
    if (CharacterId < pWorld->m_NumCharacters - 1)
      memmove(&pWorld->m_pPickupCooldowns[CharacterId], &pWorld->m_pPickupCooldowns[CharacterId + 1],
              sizeof(SPickupCooldownList) * (size_t)(pWorld->m_NumCharacters - CharacterId - 1));
  }

  // Update references in other characters and entities
  for (int i = 0; i < pWorld->m_NumCharacters; ++i) {
    if (i == CharacterId)
      continue;
    SCharacterCore *pChar = &pWorld->m_pCharacters[i];

    // Update HookedPlayer
    if (pChar->m_HookedPlayer == CharacterId) {
      cc_release_hook(pChar);
    } else if (pChar->m_HookedPlayer > CharacterId) {
      pChar->m_HookedPlayer--;
    }

    // Update HitObjects
    for (int j = 0; j < pChar->m_NumObjectsHit;) {
      if (pChar->m_aHitObjects[j] == CharacterId) {
        // Remove this hit object by shifting
        memmove(&pChar->m_aHitObjects[j], &pChar->m_aHitObjects[j + 1], sizeof(int) * (pChar->m_NumObjectsHit - j - 1));
        pChar->m_NumObjectsHit--;
        // Don't increment j, check the new value at j
      } else {
        if (pChar->m_aHitObjects[j] > CharacterId) {
          pChar->m_aHitObjects[j]--;
        }
        j++;
      }
    }
  }

  // Update Entity Owners
  for (int i = 0; i < NUM_WORLD_ENTTYPES; ++i) {
    SEntity *pEnt = pWorld->m_apFirstEntityTypes[i];
    while (pEnt) {
      int *pOwner = NULL;
      if (pEnt->m_ObjType == WORLD_ENTTYPE_PROJECTILE) {
        pOwner = &((SProjectile *)pEnt)->m_Owner;
      } else if (pEnt->m_ObjType == WORLD_ENTTYPE_LASER) {
        pOwner = &((SLaser *)pEnt)->m_Owner;
      }

      if (pOwner) {
        if (*pOwner == CharacterId) {
          *pOwner = -1;
        } else if (*pOwner > CharacterId) {
          (*pOwner)--;
        }
      }
      pEnt = pEnt->m_pNextTypeEntity;
    }
  }

  // Update indices in TeeList
  // We need to adjust Parent/Child indices for everyone else because their IDs will shift.
  for (int i = 0; i < pWorld->m_NumCharacters; ++i) {
    if (i == CharacterId)
      continue;
    STeeLink *l = &pWorld->m_Accelerator.m_pTeeList[i];
    if (l->m_Parent > CharacterId)
      l->m_Parent--;
    if (l->m_Child > CharacterId)
      l->m_Child--;
  }

  // Shift arrays
  if (CharacterId < pWorld->m_NumCharacters - 1) {
    memmove(&pWorld->m_pCharacters[CharacterId], &pWorld->m_pCharacters[CharacterId + 1],
            sizeof(SCharacterCore) * (pWorld->m_NumCharacters - CharacterId - 1));
    memmove(&pWorld->m_Accelerator.m_pTeeList[CharacterId], &pWorld->m_Accelerator.m_pTeeList[CharacterId + 1],
            sizeof(STeeLink) * (pWorld->m_NumCharacters - CharacterId - 1));
  }

  pWorld->m_NumCharacters--;

  // Update IDs for shifted characters
  for (int i = 0; i < pWorld->m_NumCharacters; ++i) {
    pWorld->m_pCharacters[i].m_Id = i;
    pWorld->m_Accelerator.m_pTeeList[i].m_TeeId = i;
  }

  // Resize or cleanup
  if (pWorld->m_NumCharacters == 0) {
    free(pWorld->m_pCharacters);
    pWorld->m_pCharacters = NULL;
    free(pWorld->m_Accelerator.m_pTeeList);
    pWorld->m_Accelerator.m_pTeeList = NULL;
    free(pWorld->m_pPickupCooldowns);
    pWorld->m_pPickupCooldowns = NULL;
  } else {
    // realloc down
    SCharacterCore *pNewArray = realloc(pWorld->m_pCharacters, (size_t)pWorld->m_NumCharacters * sizeof(SCharacterCore));
    if (pNewArray)
      pWorld->m_pCharacters = pNewArray;
    STeeLink *pNewLinks = realloc(pWorld->m_Accelerator.m_pTeeList, (size_t)pWorld->m_NumCharacters * sizeof(STeeLink));
    if (pNewLinks)
      pWorld->m_Accelerator.m_pTeeList = pNewLinks;
    if (pWorld->m_pPickupCooldowns) {
      SPickupCooldownList *pNewCooldowns = realloc(pWorld->m_pPickupCooldowns, (size_t)pWorld->m_NumCharacters * sizeof(SPickupCooldownList));
      if (pNewCooldowns)
        pWorld->m_pPickupCooldowns = pNewCooldowns;
    }
  }

  // Refresh Grid
  // This will use the updated TeeList (with correct Parent/Child/TeeId) to rebuild m_pTeeGrid
  wc_clear_grid(pWorld);
  // Force hash update if needed, though wc_clear_grid is usually called when hash changes.
  // We manually cleared it, so it is consistent with current state.
  pWorld->m_Accelerator.hash = next_world_hash();
  pWorld->m_Accelerator.m_pGrid->hash = pWorld->m_Accelerator.hash;
}

void wc_create_explosion(SWorldCore *pWorld, mvec2 Pos, int Owner) {
#define EXPLOSION_RADIUS 135.0f
#define EXPLOSION_INNER_RADIUS 48.0f
  if (pWorld->particle)
    pWorld->particle(Pos, PARTICLE_TYPE_EXPLOSION, -1, pWorld->user_data);
  emit_sound(pWorld, Pos, SOUND_TYPE_GRENADE_EXPLODE, -1);

  const bool OwnerValid = Owner >= 0 && Owner < pWorld->m_NumCharacters;
  bool Hit = !OwnerValid || !pWorld->m_pCharacters[Owner].m_GrenadeHitDisabled;
  float Strength;
  if (OwnerValid)
    Strength = pWorld->m_pCharacters[Owner].m_pTuning->m_ExplosionStrength;
  else
    Strength = pWorld->m_pTunings[0].m_ExplosionStrength;

  for (int i = 0; i < pWorld->m_NumCharacters; i++) {
    SCharacterCore *pChr = &pWorld->m_pCharacters[i];
    if (pChr->m_RespawnDelay)
      continue;
    mvec2 Diff = vvsub(pChr->m_Pos, Pos);
    float l = vlength(Diff);
    if (l >= EXPLOSION_RADIUS + PHYSICALSIZE)
      continue;
    mvec2 ForceDir = vec2_init(0, 1);
    if (l)
      ForceDir = vnormalize_nomask(Diff);
    l = 1 - fclamp((l - EXPLOSION_INNER_RADIUS) / (EXPLOSION_RADIUS - EXPLOSION_INNER_RADIUS), 0.0f, 1.0f);

    float Dmg = Strength * l;
    if (!(int)Dmg)
      continue;

    pChr->m_HitNum += Dmg;
    if (Hit || Owner == pChr->m_Id) {
      if (pChr->m_Solo && Owner != pChr->m_Id)
        continue;
      cc_take_damage(pChr, vfmul(ForceDir, Dmg * 2), (int)Dmg);
    }
  }
}

SCharacterCore *wc_intersect_character(SWorldCore *pWorld, mvec2 Pos0, mvec2 Pos1, float Radius, mvec2 *pNewPos, const SCharacterCore *pNotThis,
                                       const SCharacterCore *pThisOnly) {
  float ClosestLen = vdistance(Pos0, Pos1) * 100.0f;
  SCharacterCore *pClosest = NULL;

  for (int i = 0; i < pWorld->m_NumCharacters; ++i) {
    SCharacterCore *pEntity = &pWorld->m_pCharacters[i];
    if (pEntity->m_RespawnDelay)
      continue;
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

  pEnt->m_pNextTypeEntity = NULL;
  pEnt->m_pPrevTypeEntity = NULL;
}

void wc_copy_world(SWorldCore *__restrict__ pTo, SWorldCore *__restrict__ pFrom) {
  wc_free_pickup_cooldowns(pTo);
  pTo->m_GameTick = pFrom->m_GameTick;
  pTo->m_pCollision = pFrom->m_pCollision;
  pTo->m_pConfig = pFrom->m_pConfig;
  pTo->m_pTunings = pFrom->m_pTunings;
  pTo->m_UniqueRace = pFrom->m_UniqueRace;
  pTo->m_Accelerator.m_pGrid = pFrom->m_Accelerator.m_pGrid;
  // TODO: fix, this is very bad:
  pTo->m_Accelerator.hash = next_world_hash();

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
  if (pFrom->m_pPickupCooldowns && pTo->m_NumCharacters > 0) {
    pTo->m_pPickupCooldowns = calloc((size_t)pTo->m_NumCharacters, sizeof(SPickupCooldownList));
    if (pTo->m_pPickupCooldowns) {
      for (int Character = 0; Character < pTo->m_NumCharacters; ++Character) {
        const SPickupCooldownList *pFromList = &pFrom->m_pPickupCooldowns[Character];
        SPickupCooldownList *pToList = &pTo->m_pPickupCooldowns[Character];
        if (pFromList->m_NumEntries == 0)
          continue;
        pToList->m_pEntries = malloc((size_t)pFromList->m_NumEntries * sizeof(SPickupCooldown));
        if (!pToList->m_pEntries)
          continue;
        memcpy(pToList->m_pEntries, pFromList->m_pEntries, (size_t)pFromList->m_NumEntries * sizeof(SPickupCooldown));
        pToList->m_NumEntries = pFromList->m_NumEntries;
        pToList->m_Capacity = pFromList->m_NumEntries;
      }
    }
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
