#ifndef LIB_COLLISION_H
#define LIB_COLLISION_H

#if defined(_MSC_VER) && !defined(__clang__)
#ifndef __restrict__
#define __restrict__ __restrict
#endif
#define __attribute__(x)
#define __builtin_unreachable() __assume(0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <ddnet_map_loader.h>
#include "stdbool.h"
#include "vmath.h"
#include <stdint.h>

enum {
  CANTMOVE_LEFT = 1 << 0,
  CANTMOVE_RIGHT = 1 << 1,
  CANTMOVE_UP = 1 << 2,
  CANTMOVE_DOWN = 1 << 3,
};

#define DEATH 9
#define PICKUPSIZE 14
#define PHYSICALSIZE 28.f
#define HALFPHYSICALSIZE 14
#define PHYSICALSIZEVEC vec2_init(28.f, 28.f)
#define MAP_EXPAND 200
#define MAP_EXPAND32 (200*32)

enum {
  INFO_ISSOLID = 1 << 0,
  INFO_TILENEXT = 1 << 1,
  INFO_PICKUPNEXT = 1 << 2,
  INFO_CANGROUND = 1 << 3,
  INFO_CANHITKILL = 1 << 4,
  INFO_CANHITSOLID = 1 << 5,
  INFO_CANHITSTOPPER = 1 << 6,
};

typedef struct TuningParams {
#define MACRO_TUNING_PARAM(Name, Value) float m_##Name;
#include <ddnet_physics/tuning.h>
#undef MACRO_TUNING_PARAM
} STuningParams;

enum {
  POWERUP_HEALTH,
  POWERUP_ARMOR,
  POWERUP_WEAPON,
  POWERUP_NINJA,
  POWERUP_ARMOR_SHOTGUN,
  POWERUP_ARMOR_GRENADE,
  POWERUP_ARMOR_NINJA,
  POWERUP_ARMOR_LASER,
  NUM_POWERUPS
};
typedef struct Pickup {
  int8_t m_Type;
  uint8_t m_Number;
  uint8_t m_Subtype;
} SPickup;

enum {
  NUM_TUNE_ZONES = 256,
  DISTANCE_FIELD_RESOLUTION = 32,
};

typedef struct Collision {
  map_data_t m_MapData;
  uint32_t *m_pWidthLookup;
  uint64_t *m_pBroadSolidBitField;
  uint64_t *m_pBroadIndicesBitField;
  uint8_t *m_pTileInfos;
  SPickup *m_pPickups;
  SPickup *m_pFrontPickups;
  uint8_t (*m_pMoveRestrictions)[5];
  uint8_t *m_pTileBroadCheck;
  uint8_t *m_pSolidTeleDistanceField;
  uint64_t *m_pBroadTeleInBitField;
  mvec2 *m_apTeleOuts[256];
  mvec2 *m_apTeleCheckOuts[256];
  mvec2 *m_pSpawnPoints;

  int m_NumSpawnPoints;
  int m_aNumTeleOuts[256];
  int m_aNumTeleCheckOuts[256];
  STuningParams m_aTuningList[NUM_TUNE_ZONES];

  int m_HighestSwitchNumber;

  bool m_MoveRestrictionsFound;
} SCollision;

bool init_collision(SCollision *__restrict__ pCollision, map_data_t *__restrict__ pMap);
void free_collision(SCollision *pCollision);
int get_pure_map_index(SCollision *pCollision, mvec2 Pos);
unsigned char move_restrictions(unsigned char Direction, unsigned char Tile, unsigned char Flags);
unsigned char get_tile_index(SCollision *pCollision, int Index);
unsigned char get_front_tile_index(SCollision *pCollision, int Index);
unsigned char get_tile_flags(SCollision *pCollision, int Index);
unsigned char get_front_tile_flags(SCollision *pCollision, int Index);
unsigned char get_switch_number(SCollision *pCollision, int Index);
unsigned char get_switch_type(SCollision *pCollision, int Index);
unsigned char get_switch_delay(SCollision *pCollision, int Index);
unsigned char is_teleport(SCollision *pCollision, int Index);
unsigned char is_teleport_hook(SCollision *pCollision, int Index);
unsigned char is_teleport_weapon(SCollision *pCollision, int Index);
unsigned char is_evil_teleport(SCollision *pCollision, int Index);
unsigned char is_check_teleport(SCollision *pCollision, int Index);
unsigned char is_check_evil_teleport(SCollision *pCollision, int Index);
unsigned char is_tele_checkpoint(SCollision *pCollision, int Index);
unsigned char get_collision_at(SCollision *pCollision, mvec2 Pos);
unsigned char get_front_collision_at(SCollision *pCollision, mvec2 Pos);
unsigned char get_move_restrictions(SCollision *pCollision, void *pUser, mvec2 Pos, int Idx);
int get_map_index(SCollision *pCollision, mvec2 Pos);
bool check_point(SCollision *pCollision, mvec2 Pos);
void move_point(SCollision *pCollision, mvec2 *pInoutPos, mvec2 *pInoutVel, float Elasticity);
bool is_hook_blocker(SCollision *pCollision, int Index, mvec2 Pos0, mvec2 Pos1);
unsigned char intersect_line_tele_hook(SCollision *__restrict__ pCollision, mvec2 Pos0, mvec2 Pos1, mvec2 *__restrict__ pOutCollision,
                                       unsigned char *__restrict__ pTeleNr);
unsigned char intersect_line_tele_weapon(SCollision *__restrict__ pCollision, mvec2 Pos0, mvec2 Pos1, mvec2 *__restrict__ pOutCollision,
                                         unsigned char *__restrict__ pTeleNr);

bool test_box(SCollision *pCollision, mvec2 Pos, mvec2 Size);
unsigned char is_tune(SCollision *pCollision, int Index);
bool is_speedup(SCollision *pCollision, int Index);
void get_speedup(SCollision *__restrict__ pCollision, int Index, mvec2 *__restrict__ pDir, int *__restrict__ pForce, int *__restrict__ pMaxSpeed,
                 int *__restrict__ pType);
bool intersect_line(SCollision *__restrict__ pCollision, mvec2 Pos0, mvec2 Pos1, mvec2 *__restrict__ pOutCollision,
                    mvec2 *__restrict__ pOutBeforeCollision);
void move_box(const SCollision *__restrict__ pCollision, mvec2 Pos, mvec2 Vel, mvec2 *__restrict__ pOutPos, mvec2 *__restrict__ pOutVel,
              mvec2 Elasticity, bool *__restrict__ pGrounded);
bool get_nearest_air_pos_player(SCollision *pCollision, mvec2 PlayerPos, mvec2 *pOutPos);
bool get_nearest_air_pos(SCollision *pCollision, mvec2 Pos, mvec2 PrevPos, mvec2 *pOutPos);
int get_index(SCollision *pCollision, mvec2 PrevPos, mvec2 Pos);
unsigned char mover_speed(SCollision *pCollision, int x, int y, mvec2 *pSpeed);
int entity(SCollision *pCollision, int x, int y, int Layer);

#ifdef __cplusplus
}
#endif

#endif // LIB_COLLISION_H
