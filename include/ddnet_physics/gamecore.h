#ifndef LIB_GAMECORE_H
#define LIB_GAMECORE_H

#ifndef DDNET_PHYSICS_API
#if defined(_WIN32)
#if defined(FRAMETEE_EXPORTS)
#define DDNET_PHYSICS_API __declspec(dllexport)
#else
#define DDNET_PHYSICS_API __declspec(dllimport)
#endif
#else
#define DDNET_PHYSICS_API
#endif
#endif

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

typedef enum {
  GAME_MODE_DDRACE = 0,
  GAME_MODE_RACE,
  GAME_MODE_FASTCAP,
  GAME_MODE_FASTCAP_NO_WPNS,
  NUM_GAME_MODES,
} EGameMode;

enum { WEAPON_HAMMER = 0, WEAPON_GUN, WEAPON_SHOTGUN, WEAPON_GRENADE, WEAPON_LASER, WEAPON_NINJA, NUM_WEAPONS };
enum { NUM_TIME_CHECKPOINTS = 25 };

// Padded to 16 bytes and 8-byte aligned so `m_Input = *pNewInput` is two moves.
// At 14 bytes / 2-byte alignment clang had to break the copy into a pile of
// small loads and stores, which measured 4.2% of all retired instructions.
typedef struct __attribute__((aligned(8))) {
  int8_t m_Direction;
  int16_t m_TargetX;
  int16_t m_TargetY;
  uint8_t m_Jump;
  uint8_t m_Fire;
  uint8_t m_Hook;
  uint8_t m_WantedWeapon;
  uint8_t m_TeleOut;
  uint16_t m_Flags;
  uint8_t m_aPadding[2];
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
  mvec2 m_From;
  mvec2 m_Dir;
  mvec2 m_TelePos;
  mvec2 m_PrevPos;
  STuningParams *m_pTuning;
  float m_Energy;
  int m_Bounces;
  int m_EvalTick;
  int m_Owner;
  uint8_t m_Type; // WEAPON_SHOTGUN or WEAPON_LASER
  bool m_WasTele;
  bool m_ZeroEnergyBounceInLastTick;
  bool m_TeleportCancelled;
  bool m_IsBlueTeleport;
} SLaser;

// }}}

// SCharacter {{{

typedef struct CharacterCore {
  // Group by alignment to keep cached worlds small without unaligned SIMD loads.
  struct WorldCore *m_pWorld;
  SCollision *m_pCollision;
  mvec2 m_Pos;
  mvec2 m_Vel;
  mvec2 m_PrevPos;
  mvec2 m_HookPos;
  mvec2 m_HookDir;
  mvec2 m_HookTeleBase;
  mvec2 m_TeleGunPos;
  struct {
    mvec2 m_ActivationDir;
    int m_ActivationTick;
    int m_CurrentMoveTime;
    int m_OldVelAmount;
  } m_Ninja;

  // Pointers, aligned input and grid coordinates.
  STuningParams *m_pTuning;
  SPlayerInput m_Input;
  uivec2 m_BlockPos;

  // Player IDs, tick counters and configurable jump counts retain their full range.
  int m_Id;
  int m_BlockIdx;
  int m_HookTick;
  int m_JumpedTotal; // jumps performed in the air
  int m_Jumps;
  int m_StartTime;
  int m_FreezeTime;
  int m_FreezeStart;
  int m_TuningBlockIdx;
  int m_HookedPlayer;
  int m_ReloadTimer;
  int m_aHitObjects[10];
  int m_StartTick;
  int m_FinishTick;
  float m_StartTickOffset;
  float m_FinishTickOffset;
  float m_RaceTime; // finished time in seconds, -1 while unfinished
  uint32_t m_TimeCpMask;
  float m_TileFraction;
  int m_DamageTick;
  int m_DamageTaken;
  int m_HitNum;
  int m_AttackTick; // external animations
  float m_VelMag;   // cached for external use to avoid repeated sqrts
  float m_VelRamp;  // cached for external use to avoid repeated expfs
  uint32_t m_SpawnGeneration;

  // Byte-sized state. Keep weapon ownership at eight bytes for the hot 64-bit load.
  unsigned char m_BlockInfo; // immutable tile info cached by cc_calc_indices
  int8_t m_HookState;
  unsigned char m_LastWeapon; // previous weapon when ninja was activated
  unsigned char m_ActiveWeapon;
  bool m_aWeaponGot[8];

  // Jump flags use bits 0 and 1; checkpoint/team values also need the -1 sentinel.
  uint8_t m_Jumped;
  unsigned char m_PrevFire;
  unsigned char m_Colliding;
  unsigned char m_TeleCheckpoint;
  unsigned char m_MoveRestrictions;
  uint8_t m_NumObjectsHit;
  int8_t m_LastTimeCp;       // -1 or 0..NUM_TIME_CHECKPOINTS-1
  int8_t m_FastcapStartTeam; // -1 before starting, 0 for red, 1 for blue
  uint8_t m_RespawnDelay;
  int8_t m_aWeaponAmmo[NUM_WEAPONS];
  int8_t m_Health;
  int8_t m_Armor;
  bool m_NewHook;
  bool m_Grounded;
  bool m_LeftWall;
  bool m_LastRefillJumps;
  bool m_LastPenalty;
  bool m_LastBonus;
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
  bool m_DeepFrozen;
  bool m_LiveFrozen;
  bool m_FrozenLastTick;
  // Unfrozen by a hammer while the inputs of a step were applied: it fires
  // back at the start of wc_tick, whatever its place among the players.
  bool m_UnfrozenByHit;
  bool m_TeleGunTeleport;
  bool m_IsBlueTeleGunTeleport;
  bool m_aGotFastcapFlag[2];
  bool m_IsInFreeze; // demo rendering and freezebars

  // Checkpoint times are cold state; keep them after the per-tick fields.
  float m_aTimeCp[NUM_TIME_CHECKPOINTS];
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
  int m_EndTick;
  int m_LastUpdateTick;
  uint8_t m_Type; // TILE_SWITCH* values from the map's byte-sized tile IDs
  bool m_Status;
  bool m_Initial;
} SSwitch;

typedef struct {
  int m_Key;
  int m_EndTick;
} SPickupCooldown;

typedef struct {
  SPickupCooldown *m_pEntries;
  int m_NumEntries;
  int m_Capacity;
} SPickupCooldownList;

typedef enum {
  PARTICLE_TYPE_PLAYER_SPAWN,
  PARTICLE_TYPE_PLAYER_DEATH,
  PARTICLE_TYPE_SMOKE,
  PARTICLE_TYPE_BULLET_TRAIL,
  PARTICLE_TYPE_BULLET_STARS,
  PARTICLE_TYPE_HAMMER_HIT,
  PARTICLE_TYPE_EXPLOSION,
  PARTICLE_TYPE_AIR_JUMP,
  PARTICLE_TYPE_CONFETTI,
} EParticleType;

// DDNet world sounds. Values match the network protocol so callers building a
// demo can forward the physics result without reconstructing what happened.
typedef enum {
  SOUND_TYPE_GUN_FIRE = 0,
  SOUND_TYPE_SHOTGUN_FIRE = 1,
  SOUND_TYPE_GRENADE_FIRE = 2,
  SOUND_TYPE_HAMMER_FIRE = 3,
  SOUND_TYPE_HAMMER_HIT = 4,
  SOUND_TYPE_NINJA_FIRE = 5,
  SOUND_TYPE_GRENADE_EXPLODE = 6,
  SOUND_TYPE_NINJA_HIT = 7,
  SOUND_TYPE_LASER_FIRE = 8,
  SOUND_TYPE_LASER_BOUNCE = 9,
  SOUND_TYPE_WEAPON_SWITCH = 10,
  SOUND_TYPE_PLAYER_PAIN_SHORT = 11,
  SOUND_TYPE_PLAYER_PAIN_LONG = 12,
  SOUND_TYPE_BODY_LAND = 13,
  SOUND_TYPE_PLAYER_AIRJUMP = 14,
  SOUND_TYPE_PLAYER_JUMP = 15,
  SOUND_TYPE_PLAYER_DIE = 16,
  SOUND_TYPE_PLAYER_SPAWN = 17,
  SOUND_TYPE_PLAYER_SKID = 18,
  SOUND_TYPE_TEE_CRY = 19,
  SOUND_TYPE_HOOK_LOOP = 20,
  SOUND_TYPE_HOOK_ATTACH_GROUND = 21,
  SOUND_TYPE_HOOK_ATTACH_PLAYER = 22,
  SOUND_TYPE_HOOK_NOATTACH = 23,
} ESoundType;

typedef struct WorldCore {
  SCollision *m_pCollision;
  STeeAccelerator m_Accelerator;

  // Expects confg/tunings from outside so you can reuse them as many times as
  // needed
  SConfig *m_pConfig;
  STuningParams *m_pTunings;

  SEntity *m_apFirstEntityTypes[NUM_WORLD_ENTTYPES];

  // Store and tick characters seperately from other entities since
  // the amount of players mostly only gets set once for simulations
  // NOTE: i could do this on the stack and just set a max but keep
  // the num characters var for a speedup probably
  SCharacterCore *m_pCharacters;
  SSwitch *m_pSwitches;
  SPickupCooldownList *m_pPickupCooldowns;

  int m_NumCharacters;
  int m_NumSwitches;
  int m_GameTick;
  // Inside wc_tick (as opposed to applying the inputs before it).
  bool m_InTick;
  bool m_UniqueRace; // detected from Unique Race map settings, may also be enabled by the caller

  // external use

  // these get called so someone constructing a demo for example can capture these without meddling with the physics or recalculating them
  void *user_data;
  void (*particle)(mvec2 pos, int type, int cid, void *user_data);
  void (*damage_indicator)(mvec2 pos, float angle, int amount, int cid, void *user_data);
  void (*sound)(mvec2 pos, int sound_id, int cid, void *user_data);
} SWorldCore;

// }}}

DDNET_PHYSICS_API STeeGrid tg_empty(void);
DDNET_PHYSICS_API void tg_init(STeeGrid *pGrid, int width, int height);
DDNET_PHYSICS_API void tg_destroy(STeeGrid *pGrid);

DDNET_PHYSICS_API void init_config(SConfig *pConfig);
DDNET_PHYSICS_API bool init_game_mode(SWorldCore *pCore, SCollision *pCollision, STeeGrid *pGrid, SConfig *pConfig, map_data_t *pMap,
                                      EGameMode GameMode);
DDNET_PHYSICS_API void wc_init(SWorldCore *pCore, SCollision *pCollision, STeeGrid *pGrid, SConfig *pConfig);
DDNET_PHYSICS_API void wc_copy_world(SWorldCore *__restrict__ pTo, SWorldCore *__restrict__ pFrom);
DDNET_PHYSICS_API void wc_tick(SWorldCore *pCore);
DDNET_PHYSICS_API void wc_free(SWorldCore *pCore);
DDNET_PHYSICS_API SWorldCore wc_empty(void);

DDNET_PHYSICS_API void cc_on_input(SCharacterCore *pCore, const SPlayerInput *pNewInput);
DDNET_PHYSICS_API SCharacterCore *wc_add_character(SWorldCore *pWorld, int Num);
DDNET_PHYSICS_API void wc_remove_character(SWorldCore *pWorld, int CharacterId);
DDNET_PHYSICS_API void wc_remove_entities_from_player(SWorldCore *pWorld, int PlayerId);

// utility functions you might need
DDNET_PHYSICS_API mvec2 prj_get_pos(SProjectile *pProj, float Time);
DDNET_PHYSICS_API SCharacterCore *wc_intersect_character(SWorldCore *pWorld, mvec2 Pos0, mvec2 Pos1, float Radius, mvec2 *pNewPos,
                                                         const SCharacterCore *pNotThis, const SCharacterCore *pThisOnly);
DDNET_PHYSICS_API void wc_insert_entity(SWorldCore *pWorld, SEntity *pEnt);
DDNET_PHYSICS_API bool cc_freeze(SCharacterCore *pCore, int Seconds);
DDNET_PHYSICS_API void cc_unfreeze(SCharacterCore *pCore);
DDNET_PHYSICS_API bool cc_take_damage(SCharacterCore *pCore, mvec2 Force, int Damage);
DDNET_PHYSICS_API void cc_calc_indices(SCharacterCore *pCore);
DDNET_PHYSICS_API void cc_die(SCharacterCore *pCore);

#ifdef __cplusplus
}
#endif

#endif // LIB_GAMECORE_H
