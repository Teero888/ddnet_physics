#ifndef LIB_GAMECORE_H
#define LIB_GAMECORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ddnet_physics/collision.h>
#include <ddnet_physics/vmath.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct Config {
#define MACRO_CONFIG_INT(Name, Def) int m_##Name;
#include <ddnet_physics/config.h>
#undef MACRO_CONFIG_INT
} SConfig;

enum { WEAPON_HAMMER = 0, WEAPON_GUN, WEAPON_SHOTGUN, WEAPON_GRENADE, WEAPON_LASER, WEAPON_NINJA, NUM_WEAPONS };

typedef struct {
  int8_t m_Direction;
  int16_t m_TargetX;
  int16_t m_TargetY;
  uint8_t m_Jump;
  uint8_t m_Fire;
  uint8_t m_Hook;
  uint8_t m_WantedWeapon;
  uint8_t m_TeleOut;
  uint16_t m_Flags;
} SPlayerInput;

enum {
  HOOK_RETRACTED = -1,
  HOOK_IDLE = 0,
  HOOK_RETRACT_START = 1,
  HOOK_RETRACT_END = 3,
  HOOK_FLYING,
  HOOK_GRABBED,
};

// sadly binary integer literals are a C23 extension xdd
enum {
  FLAG_KILL = 1 << 0,
  FLAG_SPEC = 1 << 1,
  FLAG_HOOKLINE = 1 << 2,
  FLAG_CHATBUBBLE = 1 << 3,
  FLAG_SIT = 1 << 4,
  FLAG_CONNECT = 1 << 5,
  FLAG_DISCONNECT = 1 << 6,
  FLAG_EYESTATE = 7 << 7, // bits 7..9; 0b111 << 7
  FLAG_EMOTE_TRIGGER = 1 << 10,
  FLAG_EMOTE_INDEX = 15 << 11, // bits 11..14; 0b1111 << 11
};

// not the same ordering as in ddnet but it matches the texture offsets
enum { EYE_NORMAL, EYE_ANGRY, EYE_PAIN, EYE_HAPPY, EYE_BLINK, EYE_SURPRISE, NUM_EYES };

static inline int get_flag_kill(const SPlayerInput *p) { return p->m_Flags & FLAG_KILL; }

static inline void set_flag_kill(SPlayerInput *p, int value) {
  if (value)
    p->m_Flags |= FLAG_KILL;
  else
    p->m_Flags &= ~FLAG_KILL;
}

static inline int get_flag_spec(const SPlayerInput *p) { return p->m_Flags & FLAG_SPEC; }

static inline void set_flag_spec(SPlayerInput *p, int value) {
  if (value)
    p->m_Flags |= FLAG_SPEC;
  else
    p->m_Flags &= ~FLAG_SPEC;
}

static inline int get_flag_hookline(const SPlayerInput *p) { return p->m_Flags & FLAG_HOOKLINE; }

static inline void set_flag_hookline(SPlayerInput *p, int value) {
  if (value)
    p->m_Flags |= FLAG_HOOKLINE;
  else
    p->m_Flags &= ~FLAG_HOOKLINE;
}

static inline int get_flag_chatbubble(const SPlayerInput *p) { return p->m_Flags & FLAG_CHATBUBBLE; }

static inline void set_flag_chatbubble(SPlayerInput *p, int value) {
  if (value)
    p->m_Flags |= FLAG_CHATBUBBLE;
  else
    p->m_Flags &= ~FLAG_CHATBUBBLE;
}

static inline int get_flag_sit(const SPlayerInput *p) { return p->m_Flags & FLAG_SIT; }

static inline void set_flag_sit(SPlayerInput *p, int value) {
  if (value)
    p->m_Flags |= FLAG_SIT;
  else
    p->m_Flags &= ~FLAG_SIT;
}

static inline int get_flag_connect(const SPlayerInput *p) { return p->m_Flags & FLAG_CONNECT; }

static inline void set_flag_connect(SPlayerInput *p, int value) {
  if (value)
    p->m_Flags |= FLAG_CONNECT;
  else
    p->m_Flags &= ~FLAG_CONNECT;
}

static inline int get_flag_disconnect(const SPlayerInput *p) { return p->m_Flags & FLAG_DISCONNECT; }

static inline void set_flag_disconnect(SPlayerInput *p, int value) {
  if (value)
    p->m_Flags |= FLAG_DISCONNECT;
  else
    p->m_Flags &= ~FLAG_DISCONNECT;
}

static inline int get_flag_emote_trigger(const SPlayerInput *p) { return p->m_Flags & FLAG_EMOTE_TRIGGER; }

static inline void set_flag_emote_trigger(SPlayerInput *p, int value) {
  if (value)
    p->m_Flags |= FLAG_EMOTE_TRIGGER;
  else
    p->m_Flags &= ~FLAG_EMOTE_TRIGGER;
}

static inline uint8_t get_flag_eye_state(const SPlayerInput *p) { return (p->m_Flags & FLAG_EYESTATE) >> 7; }

static inline void set_flag_eye_state(SPlayerInput *p, uint8_t state) {
  state &= 0x7;
  p->m_Flags = (p->m_Flags & ~FLAG_EYESTATE) | (state << 7);
}

static inline uint8_t get_flag_emote_index(const SPlayerInput *p) { return (p->m_Flags & FLAG_EMOTE_INDEX) >> 11; }

static inline void set_flag_emote_index(SPlayerInput *p, uint8_t index) {
  index &= 0xF;
  p->m_Flags = (p->m_Flags & ~FLAG_EMOTE_INDEX) | (index << 11);
}

enum { WORLD_ENTTYPE_PROJECTILE = 0, WORLD_ENTTYPE_LASER, NUM_WORLD_ENTTYPES };

bool is_switch_active_cb(int Number, void *pUser);

// Entities {{{

typedef struct Entity {
  struct WorldCore *m_pWorld;
  SCollision *m_pCollision;
  struct Entity *__restrict__ m_pPrevTypeEntity;
  struct Entity *__restrict__ m_pNextTypeEntity;
  mvec2 m_Pos;
  int m_ObjType;
  int m_Number;
  int m_Layer;
  bool m_MarkedForDestroy;
  bool m_Spawned;
} SEntity;

typedef struct Projectile {
  SEntity m_Base;
  mvec2 m_Direction;
  STuningParams *m_pTuning;
  int m_LifeSpan;
  int m_Owner;
  int m_Type;
  int m_StartTick;
  int m_Bouncing;
  bool m_Explosive;
  bool m_Freeze;
  bool m_IsSolo;
} SProjectile;

typedef struct Laser {
  SEntity m_Base;
  STuningParams *m_pTuning;
  mvec2 m_From;
  mvec2 m_Dir;
  mvec2 m_TelePos;
  mvec2 m_PrevPos;
  bool m_WasTele;
  float m_Energy;
  int m_Bounces;
  int m_EvalTick;
  int m_Owner;
  bool m_ZeroEnergyBounceInLastTick;
  int m_Type;
  bool m_TeleportCancelled;
  bool m_IsBlueTeleport;
} SLaser;

// }}}

// SCharacter {{{

typedef struct CharacterCore {
  struct WorldCore *m_pWorld;
  SCollision *m_pCollision;
  int m_Id;
  mvec2 m_PrevPos;
  mvec2 m_Pos;
  mvec2 m_Vel;

  uivec2 m_BlockPos;
  int m_BlockIdx;

  mvec2 m_HookPos;
  mvec2 m_HookDir;
  mvec2 m_HookTeleBase;
  int m_HookTick;
  int8_t m_HookState;

  unsigned char m_LastWeapon; // only used for ninja, is only triggered when ninja is activated
  unsigned char m_ActiveWeapon;
  bool m_aWeaponGot[NUM_WEAPONS];

  // ninja
  struct {
    mvec2 m_ActivationDir;
    int m_ActivationTick;
    int m_CurrentMoveTime;
    int m_OldVelAmount;
  } m_Ninja;

  bool m_NewHook;

  bool m_Grounded;
  int m_Jumped;
  // m_JumpedTotal counts the jumps performed in the air
  int m_JumpedTotal;
  int m_Jumps;

  unsigned char m_PrevFire;
  SPlayerInput m_Input;

  // DDRace
  int m_StartTime;

  unsigned char m_Colliding;
  bool m_LeftWall;
  unsigned char m_TeleCheckpoint;

  // Last refers to the last tick
  bool m_LastRefillJumps;
  bool m_LastPenalty;
  bool m_LastBonus;

  // DDNet Character
  bool m_Solo;
  bool m_Jetpack;
  bool m_CollisionDisabled;
  bool m_EndlessHook;
  bool m_EndlessJump;
  bool m_HammerHitDisabled;
  bool m_GrenadeHitDisabled;
  bool m_LaserHitDisabled;
  bool m_ShotgunHitDisabled;
  bool m_HookHitDisabled;
  bool m_HasTelegunGun;
  bool m_HasTelegunGrenade;
  bool m_HasTelegunLaser;
  int m_FreezeTime;
  int m_FreezeStart;
  bool m_DeepFrozen;
  bool m_LiveFrozen;
  bool m_FrozenLastTick;
  STuningParams *m_pTuning;
  unsigned char m_MoveRestrictions;
  // we might have more than 255 player ids
  int m_HookedPlayer;

  mvec2 m_TeleGunPos;
  bool m_TeleGunTeleport;
  bool m_IsBlueTeleGunTeleport;

  int m_ReloadTimer;

  int m_aHitObjects[10];
  uint8_t m_NumObjectsHit;

  int m_StartTick;
  int m_FinishTick;

  uint8_t m_RespawnDelay;

  // external use

  int m_HitNum;
  int m_AttackTick;  // for external animations
  bool m_IsInFreeze; // for demo rendering, freezebars
  float m_VelMag;    // for external use to avoid multiple sqrts
  float m_VelRamp;   // for external use to avoid multiple expfs

} SCharacterCore;
// }}}

// World {{{

typedef struct {
  int32_t m_TeeId;
  int32_t m_Parent; // -1 means no parent explanation -> https://i.imgflip.com/6iwo55.jpg
  int32_t m_Child;  // -1 means no child
  uint32_t m_Tile;  // Grid Child index. always exists
} STeeLink;

// global struct
typedef struct {
  int *m_pTeeGrid; // array for the whole map
  uint64_t hash;
} STeeGrid;

typedef struct {
  STeeGrid *m_pGrid;
  STeeLink *m_pTeeList; // array of tees (world specific)
  uint64_t hash;        // hash to compare between tee grid world changes
} STeeAccelerator;

// We don't want teams for the physics, that makes switches easier
typedef struct {
  bool m_Status;
  bool m_Initial;
  int m_EndTick;
  int m_Type;
  int m_LastUpdateTick;
} SSwitch;

typedef enum {
  PARTICLE_TYPE_PLAYER_SPAWN,
  PARTICLE_TYPE_PLAYER_DEATH,
  PARTICLE_TYPE_SMOKE,
  PARTICLE_TYPE_BULLET_TRAIL,
  PARTICLE_TYPE_BULLET_STARS,
  PARTICLE_TYPE_HAMMER_HIT,
  PARTICLE_TYPE_EXPLOSION,
  PARTICLE_TYPE_AIR_JUMP,
} EParticleType;

typedef struct WorldCore {
  SCollision *m_pCollision;
  STeeAccelerator m_Accelerator;

  // Expects confg/tunings from outside so you can reuse them as many times as
  // needed
  SConfig *m_pConfig;
  STuningParams *m_pTunings;

  SEntity *m_pNextTraverseEntity;
  SEntity *m_apFirstEntityTypes[NUM_WORLD_ENTTYPES];

  // Store and tick characters seperately from other entities since
  // the amount of players mostly only gets set once for simulations
  // NOTE: i could do this on the stack and just set a max but keep
  // the num characters var for a speedup probably
  int m_NumCharacters;
  SCharacterCore *m_pCharacters;

  int m_NumSwitches;
  SSwitch *m_pSwitches;

  int m_GameTick;

  // external use

  // these get called so someone constructing a demo for example can capture these without meddling with the physics or recalculating them
  void *user_data;
  void (*particle)(mvec2 pos, int type, int cid, void *user_data);
} SWorldCore;

// }}}

STeeGrid tg_empty(void);
void tg_init(STeeGrid *pGrid, int width, int height);
void tg_destroy(STeeGrid *pGrid);

void init_config(SConfig *pConfig);
void wc_init(SWorldCore *pCore, SCollision *pCollision, STeeGrid *pGrid, SConfig *pConfig);
void wc_copy_world(SWorldCore *__restrict__ pTo, SWorldCore *__restrict__ pFrom);
void wc_tick(SWorldCore *pCore);
void wc_free(SWorldCore *pCore);
SWorldCore wc_empty(void);

void cc_on_input(SCharacterCore *pCore, const SPlayerInput *pNewInput);
SCharacterCore *wc_add_character(SWorldCore *pWorld, int Num);
void wc_remove_character(SWorldCore *pWorld, int CharacterId);

// utility functions you might need
mvec2 prj_get_pos(SProjectile *pProj, float Time);
SCharacterCore *wc_intersect_character(SWorldCore *pWorld, mvec2 Pos0, mvec2 Pos1, float Radius, mvec2 *pNewPos, const SCharacterCore *pNotThis,
                                       const SCharacterCore *pThisOnly);
void wc_insert_entity(SWorldCore *pWorld, SEntity *pEnt);
bool cc_freeze(SCharacterCore *pCore, int Seconds);
void cc_unfreeze(SCharacterCore *pCore);
void cc_calc_indices(SCharacterCore *pCore);
void cc_die(SCharacterCore *pCore);

#ifdef __cplusplus
}
#endif

#endif // LIB_GAMECORE_H
