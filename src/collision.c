#include "collision_tables.h"
#include "limits.h"
#include "settings.h"
#include "src/tiles.h"
#include <assert.h>
#include <ddnet_map_loader.h>
#include <ddnet_physics/collision.h>
#include <ddnet_physics/gamecore.h>
#include <ddnet_physics/vmath.h>
#include <float.h>
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <malloc.h>
#else
#include <mm_malloc.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MR_DIR_HERE = 0, MR_DIR_RIGHT, MR_DIR_DOWN, MR_DIR_LEFT, MR_DIR_UP, NUM_MR_DIRS };

static bool tile_exists_next(SCollision *pCollision, int Index) {
  const unsigned char *pTileIdx = pCollision->m_MapData.game_layer.data;
  const unsigned char *pTileFlgs = pCollision->m_MapData.game_layer.flags;
  const unsigned char *pFrontIdx = pCollision->m_MapData.front_layer.data;
  const unsigned char *pFrontFlgs = pCollision->m_MapData.front_layer.flags;
  const unsigned char *pDoorIdx = pCollision->m_MapData.door_layer.index;
  const unsigned char *pDoorFlgs = pCollision->m_MapData.door_layer.flags;
  int TileOnTheLeft = (Index - 1 > 0) ? Index - 1 : Index;
  int TileOnTheRight = (Index + 1 < pCollision->m_MapData.width * pCollision->m_MapData.height) ? Index + 1 : Index;
  int TileBelow = (Index + pCollision->m_MapData.width < pCollision->m_MapData.width * pCollision->m_MapData.height)
                      ? Index + pCollision->m_MapData.width
                      : Index;
  int TileAbove = (Index - pCollision->m_MapData.width > 0) ? Index - pCollision->m_MapData.width : Index;

  if ((pTileIdx[TileOnTheRight] == TILE_STOP && pTileFlgs[TileOnTheRight] == ROTATION_270) ||
      (pTileIdx[TileOnTheLeft] == TILE_STOP && pTileFlgs[TileOnTheLeft] == ROTATION_90))
    return true;
  if ((pTileIdx[TileBelow] == TILE_STOP && pTileFlgs[TileBelow] == ROTATION_0) ||
      (pTileIdx[TileAbove] == TILE_STOP && pTileFlgs[TileAbove] == ROTATION_180))
    return true;
  if (pTileIdx[TileOnTheRight] == TILE_STOPA || pTileIdx[TileOnTheLeft] == TILE_STOPA ||
      ((pTileIdx[TileOnTheRight] == TILE_STOPS || pTileIdx[TileOnTheLeft] == TILE_STOPS)))
    return true;
  if (pTileIdx[TileBelow] == TILE_STOPA || pTileIdx[TileAbove] == TILE_STOPA ||
      ((pTileIdx[TileBelow] == TILE_STOPS || pTileIdx[TileAbove] == TILE_STOPS) && pTileFlgs[TileBelow] | ROTATION_180 | ROTATION_0))
    return true;

  if (pFrontIdx) {
    if (pFrontIdx[TileOnTheRight] == TILE_STOPA || pFrontIdx[TileOnTheLeft] == TILE_STOPA ||
        ((pFrontIdx[TileOnTheRight] == TILE_STOPS || pFrontIdx[TileOnTheLeft] == TILE_STOPS)))
      return true;
    if (pFrontIdx[TileBelow] == TILE_STOPA || pFrontIdx[TileAbove] == TILE_STOPA ||
        ((pFrontIdx[TileBelow] == TILE_STOPS || pFrontIdx[TileAbove] == TILE_STOPS) && pFrontFlgs[TileBelow] | ROTATION_180 | ROTATION_0))
      return true;
    if ((pFrontIdx[TileOnTheRight] == TILE_STOP && pFrontFlgs[TileOnTheRight] == ROTATION_270) ||
        (pFrontIdx[TileOnTheLeft] == TILE_STOP && pFrontFlgs[TileOnTheLeft] == ROTATION_90))
      return true;
    if ((pFrontIdx[TileBelow] == TILE_STOP && pFrontFlgs[TileBelow] == ROTATION_0) ||
        (pFrontIdx[TileAbove] == TILE_STOP && pFrontFlgs[TileAbove] == ROTATION_180))
      return true;
  }
  if (pDoorIdx) {
    if (pDoorIdx[TileOnTheRight] == TILE_STOPA || pDoorIdx[TileOnTheLeft] == TILE_STOPA ||
        ((pDoorIdx[TileOnTheRight] == TILE_STOPS || pDoorIdx[TileOnTheLeft] == TILE_STOPS)))
      return true;
    if (pDoorIdx[TileBelow] == TILE_STOPA || pDoorIdx[TileAbove] == TILE_STOPA ||
        ((pDoorIdx[TileBelow] == TILE_STOPS || pDoorIdx[TileAbove] == TILE_STOPS) && pDoorFlgs[TileBelow] | ROTATION_180 | ROTATION_0))
      return true;
    if ((pDoorIdx[TileOnTheRight] == TILE_STOP && pDoorFlgs[TileOnTheRight] == ROTATION_270) ||
        (pDoorIdx[TileOnTheLeft] == TILE_STOP && pDoorFlgs[TileOnTheLeft] == ROTATION_90))
      return true;
    if ((pDoorIdx[TileBelow] == TILE_STOP && pDoorFlgs[TileBelow] == ROTATION_0) ||
        (pDoorIdx[TileAbove] == TILE_STOP && pDoorFlgs[TileAbove] == ROTATION_180))
      return true;
  }
  return false;
}

static bool tile_exists(SCollision *pCollision, int Index) {
  const unsigned char *pTiles = pCollision->m_MapData.game_layer.data;
  const unsigned char *pFront = pCollision->m_MapData.front_layer.data;
  const unsigned char *pDoor = pCollision->m_MapData.door_layer.index;
  const unsigned char *pTele = pCollision->m_MapData.tele_layer.type;
  const unsigned char *pSpeedup = pCollision->m_MapData.speedup_layer.force;
  const unsigned char *pSwitch = pCollision->m_MapData.switch_layer.type;
  const unsigned char *pTune = pCollision->m_MapData.tune_layer.type;

  if ((pTiles[Index] >= TILE_FREEZE && pTiles[Index] <= TILE_TELE_LASER_DISABLE) ||
      (pTiles[Index] >= TILE_LFREEZE && pTiles[Index] <= TILE_LUNFREEZE))
    return true;
  if (pFront && ((pFront[Index] >= TILE_FREEZE && pFront[Index] <= TILE_TELE_LASER_DISABLE) ||
                 (pFront[Index] >= TILE_LFREEZE && pFront[Index] <= TILE_LUNFREEZE)))
    return true;
  if (pTele && (pTele[Index] == TILE_TELEIN || pTele[Index] == TILE_TELEINEVIL || pTele[Index] == TILE_TELECHECKINEVIL ||
                pTele[Index] == TILE_TELECHECK || pTele[Index] == TILE_TELECHECKIN))
    return true;
  if (pSpeedup && pSpeedup[Index] > 0)
    return true;
  if (pDoor && pDoor[Index])
    return true;
  if (pSwitch && pSwitch[Index])
    return true;
  if (pTune && pTune[Index])
    return true;
  return tile_exists_next(pCollision, Index);
}

#define SQRT2 1.4142135623730951
__attribute__((unused)) static void init_distance_field(SCollision *pCollision) {
  const size_t orig_width = pCollision->m_MapData.width;
  const size_t orig_height = pCollision->m_MapData.height;
  const unsigned char *pInfos = pCollision->m_pTileInfos;

  const size_t hr_width = orig_width * DISTANCE_FIELD_RESOLUTION;
  const size_t hr_height = orig_height * DISTANCE_FIELD_RESOLUTION;
  float *hr_field = _mm_malloc(hr_width * hr_height * sizeof(float), 32);
  if (!hr_field) {
    printf("Error: could not allocate %zu bytes of memory\n", hr_width * hr_height * sizeof(float));
    return;
  }
  for (size_t y = 0; y < hr_height; ++y) {
    for (size_t x = 0; x < hr_width; ++x) {
      const size_t orig_x = x / DISTANCE_FIELD_RESOLUTION;
      const size_t orig_y = y / DISTANCE_FIELD_RESOLUTION;
      const size_t orig_idx = orig_y * orig_width + orig_x;
      const size_t tele = pCollision->m_MapData.tele_layer.type ? pCollision->m_MapData.tele_layer.type[orig_idx] : 0;
      hr_field[y * hr_width + x] = (pInfos[orig_idx] & INFO_ISSOLID || tele == TILE_TELEINHOOK || tele == TILE_TELEINWEAPON) ? 0.0f : FLT_MAX;
    }
  }

  // First pass: left-top to right-bottom
  for (size_t y = 1; y < hr_height - 1; ++y) {
    for (size_t x = 1; x < hr_width - 1; ++x) {
      const size_t idx = y * hr_width + x;
      if (hr_field[idx] == 0.0f)
        continue;

      float min_dist = FLT_MAX;
      min_dist = fminf(min_dist, hr_field[idx - 1] + 1.0f);
      min_dist = fminf(min_dist, hr_field[idx - hr_width] + 1.0f);
      min_dist = fminf(min_dist, hr_field[idx - hr_width - 1] + (float)SQRT2);
      min_dist = fminf(min_dist, hr_field[idx - hr_width + 1] + (float)SQRT2);

      hr_field[idx] = fminf(hr_field[idx], min_dist);
    }
  }

  // Second pass: right-bottom to left-top
  for (size_t y = hr_height - 2; y > 0; --y) {
    for (size_t x = hr_width - 2; x > 0; --x) {
      const size_t idx = y * hr_width + x;
      if (hr_field[idx] == 0.0f)
        continue;

      float min_dist = FLT_MAX;
      min_dist = fminf(min_dist, hr_field[idx + 1] + 1.0f);
      min_dist = fminf(min_dist, hr_field[idx + hr_width] + 1.0f);
      min_dist = fminf(min_dist, hr_field[idx + hr_width + 1] + (float)SQRT2);
      min_dist = fminf(min_dist, hr_field[idx + hr_width - 1] + (float)SQRT2);
      hr_field[idx] = fminf(hr_field[idx], min_dist);
    }
  }

  pCollision->m_pSolidTeleDistanceField = _mm_malloc(hr_width * hr_height, 64);
  if (!pCollision->m_pSolidTeleDistanceField) {
    printf("Could not allocated %zu bytes for m_pSolidTeleDistanceField\n", hr_width * hr_height);
  }

  const float scale_to_world = 32.f / (float)DISTANCE_FIELD_RESOLUTION;
  for (size_t i = 0; i < hr_width * hr_height; ++i) {
    hr_field[i] -= 1.5f;
    hr_field[i] *= scale_to_world;
    hr_field[i] = fclamp(hr_field[i], 0, 255);
    pCollision->m_pSolidTeleDistanceField[i] = (uint8_t)imax((int)hr_field[i] - 1, 0);
  }
  free(hr_field);
}
#undef SQRT2

void tuning_update_derived(STuningParams *pTuning) {
  const float MaxRampSpeed = pTuning->m_VelrampRange / (50 * logf(fmaxf(pTuning->m_VelrampCurvature, 1.01f)));
  pTuning->m_SpeedupDefaultMaxSpeed = fmaxf(MaxRampSpeed, pTuning->m_VelrampStart / 50) * 5.0f;
}

static void init_tuning_params(STuningParams *pTunings) {
#define MACRO_TUNING_PARAM(Name, Value) pTunings->m_##Name = Value;
#include <ddnet_physics/tuning.h>
#undef MACRO_TUNING_PARAM

  // DDNet resets these weapon tunings to DDRace values before applying map
  // settings.
  pTunings->m_GunCurvature = 0.0f;
  pTunings->m_GunSpeed = 1400.0f;
  pTunings->m_ShotgunCurvature = 0.0f;
  pTunings->m_ShotgunSpeed = 500.0f;
  pTunings->m_ShotgunSpeeddiff = 0.0f;
}

/* Helper: expand an unsigned-char layer with clamp-repeat edges.
 * old == NULL -> *out_new = NULL and return true (layer absent).
 * newW/newH computed by caller to avoid recomputation/overflow checks in each helper.
 */
static bool expand_uchar_layer(const unsigned char *old, unsigned char **out_new, int oldW, int oldH, int pad, int newW, int newH) {
  if (!old) {
    *out_new = NULL;
    return true;
  }

  /* safety checks */
  if (oldW <= 0 || oldH <= 0 || pad < 0)
    return false;

  size_t newSize = (size_t)newW * (size_t)newH;
  /* guard overflow */
  if (newSize == 0)
    return false;

  unsigned char *newBuf = (unsigned char *)malloc(newSize * sizeof(unsigned char));
  if (!newBuf)
    return false;

  for (int y = 0; y < newH; ++y) {
    int src_y = (y < pad) ? 0 : ((y >= pad + oldH) ? (oldH - 1) : (y - pad));
    const unsigned char *src_row = old + (size_t)src_y * (size_t)oldW;
    unsigned char *dst_row = newBuf + (size_t)y * (size_t)newW;

    /* left clamp region */
    if (pad > 0) {
      /* src_row[0] repeated */
      memset(dst_row, (int)src_row[0], (size_t)pad);
    }

    /* middle region: contiguous copy of old row */
    memcpy(dst_row + pad, src_row, (size_t)oldW);

    /* right clamp region */
    int right = newW - pad - oldW;
    if (right > 0) {
      memset(dst_row + pad + oldW, (int)src_row[oldW - 1], (size_t)right);
    }
  }

  *out_new = newBuf;
  return true;
}

/* Helper: expand short (signed 16-bit) layer */
static bool expand_short_layer(const short *old, short **out_new, int oldW, int oldH, int pad, int newW, int newH) {
  if (!old) {
    *out_new = NULL;
    return true;
  }
  if (oldW <= 0 || oldH <= 0 || pad < 0)
    return false;

  size_t newSize = (size_t)newW * (size_t)newH;
  if (newSize == 0)
    return false;

  short *newBuf = (short *)malloc(newSize * sizeof(short));
  if (!newBuf)
    return false;

  for (int y = 0; y < newH; ++y) {
    int src_y = (y < pad) ? 0 : ((y >= pad + oldH) ? (oldH - 1) : (y - pad));
    const short *src_row = old + (size_t)src_y * (size_t)oldW;
    short *dst_row = newBuf + (size_t)y * (size_t)newW;

    /* left */
    for (int i = 0; i < pad; ++i)
      dst_row[i] = src_row[0];

    /* middle */
    memcpy((void *)(dst_row + pad), (const void *)src_row, (size_t)oldW * sizeof(short));

    /* right */
    int right = newW - pad - oldW;
    for (int i = 0; i < right; ++i)
      dst_row[pad + oldW + i] = src_row[oldW - 1];
  }

  *out_new = newBuf;
  return true;
}

/* Helper: expand int (32-bit) layer */
static bool expand_int_layer(const int *old, int **out_new, int oldW, int oldH, int pad, int newW, int newH) {
  if (!old) {
    *out_new = NULL;
    return true;
  }
  if (oldW <= 0 || oldH <= 0 || pad < 0)
    return false;

  size_t newSize = (size_t)newW * (size_t)newH;
  if (newSize == 0)
    return false;

  int *newBuf = (int *)malloc(newSize * sizeof(int));
  if (!newBuf)
    return false;

  for (int y = 0; y < newH; ++y) {
    int src_y = (y < pad) ? 0 : ((y >= pad + oldH) ? (oldH - 1) : (y - pad));
    const int *src_row = old + (size_t)src_y * (size_t)oldW;
    int *dst_row = newBuf + (size_t)y * (size_t)newW;

    /* left */
    for (int i = 0; i < pad; ++i)
      dst_row[i] = src_row[0];

    /* middle */
    memcpy((void *)(dst_row + pad), (const void *)src_row, (size_t)oldW * sizeof(int));

    /* right */
    int right = newW - pad - oldW;
    for (int i = 0; i < right; ++i)
      dst_row[pad + oldW + i] = src_row[oldW - 1];
  }

  *out_new = newBuf;
  return true;
}

/* Main function to expand & shift the whole map_data_t by 'pad' on each axis.
 * On success: map is updated and old buffers freed.
 * On failure: map is left untouched and function returns false.
 */
bool expand_and_shift_map(map_data_t *map, int pad) {
  if (!map)
    return false;
  if (pad <= 0)
    return true; /* nothing to do */

  int oldW = map->width;
  int oldH = map->height;
  if (oldW <= 0 || oldH <= 0)
    return false;

  /* compute new dims with overflow checks */
  size_t newW_sz = (size_t)oldW + (size_t)2 * (size_t)pad;
  size_t newH_sz = (size_t)oldH + (size_t)2 * (size_t)pad;
  if (newW_sz == 0 || newH_sz == 0)
    return false;
  if (newW_sz > SIZE_MAX / newH_sz)
    return false; /* would overflow size_t multiply */

  int newW = (int)newW_sz;
  int newH = (int)newH_sz;

  /* Temporary pointers for new buffers. NULL means "layer absent" (preserve absence). */
  unsigned char *new_game_data = NULL, *new_game_flags = NULL;
  unsigned char *new_front_data = NULL, *new_front_flags = NULL;
  unsigned char *new_tele_number = NULL, *new_tele_type = NULL;
  unsigned char *new_spd_force = NULL, *new_spd_max = NULL, *new_spd_type = NULL;
  short *new_spd_angle = NULL;
  unsigned char *new_switch_number = NULL, *new_switch_type = NULL, *new_switch_flags = NULL, *new_switch_delay = NULL;
  unsigned char *new_door_index = NULL, *new_door_flags = NULL;
  int *new_door_number = NULL;
  unsigned char *new_tune_number = NULL, *new_tune_type = NULL;

  /* Expand each layer that exists. If any allocation fails -> cleanup and return false. */
  if (!expand_uchar_layer(map->game_layer.data, &new_game_data, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->game_layer.flags, &new_game_flags, oldW, oldH, pad, newW, newH))
    goto fail;

  if (!expand_uchar_layer(map->front_layer.data, &new_front_data, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->front_layer.flags, &new_front_flags, oldW, oldH, pad, newW, newH))
    goto fail;

  if (!expand_uchar_layer(map->tele_layer.number, &new_tele_number, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->tele_layer.type, &new_tele_type, oldW, oldH, pad, newW, newH))
    goto fail;

  if (!expand_uchar_layer(map->speedup_layer.force, &new_spd_force, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->speedup_layer.max_speed, &new_spd_max, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->speedup_layer.type, &new_spd_type, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_short_layer(map->speedup_layer.angle, &new_spd_angle, oldW, oldH, pad, newW, newH))
    goto fail;

  if (!expand_uchar_layer(map->switch_layer.number, &new_switch_number, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->switch_layer.type, &new_switch_type, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->switch_layer.flags, &new_switch_flags, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->switch_layer.delay, &new_switch_delay, oldW, oldH, pad, newW, newH))
    goto fail;

  if (!expand_uchar_layer(map->door_layer.index, &new_door_index, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->door_layer.flags, &new_door_flags, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_int_layer(map->door_layer.number, &new_door_number, oldW, oldH, pad, newW, newH))
    goto fail;

  if (!expand_uchar_layer(map->tune_layer.number, &new_tune_number, oldW, oldH, pad, newW, newH))
    goto fail;
  if (!expand_uchar_layer(map->tune_layer.type, &new_tune_type, oldW, oldH, pad, newW, newH))
    goto fail;

  /* All allocations succeeded. Free old buffers and commit new ones. */
  if (map->game_layer.data)
    free(map->game_layer.data);
  if (map->game_layer.flags)
    free(map->game_layer.flags);
  if (map->front_layer.data)
    free(map->front_layer.data);
  if (map->front_layer.flags)
    free(map->front_layer.flags);
  if (map->tele_layer.number)
    free(map->tele_layer.number);
  if (map->tele_layer.type)
    free(map->tele_layer.type);
  if (map->speedup_layer.force)
    free(map->speedup_layer.force);
  if (map->speedup_layer.max_speed)
    free(map->speedup_layer.max_speed);
  if (map->speedup_layer.type)
    free(map->speedup_layer.type);
  if (map->speedup_layer.angle)
    free(map->speedup_layer.angle);
  if (map->switch_layer.number)
    free(map->switch_layer.number);
  if (map->switch_layer.type)
    free(map->switch_layer.type);
  if (map->switch_layer.flags)
    free(map->switch_layer.flags);
  if (map->switch_layer.delay)
    free(map->switch_layer.delay);
  if (map->door_layer.index)
    free(map->door_layer.index);
  if (map->door_layer.flags)
    free(map->door_layer.flags);
  if (map->door_layer.number)
    free(map->door_layer.number);
  if (map->tune_layer.number)
    free(map->tune_layer.number);
  if (map->tune_layer.type)
    free(map->tune_layer.type);

  /* assign new buffers */
  map->game_layer.data = new_game_data;
  map->game_layer.flags = new_game_flags;
  map->front_layer.data = new_front_data;
  map->front_layer.flags = new_front_flags;
  map->tele_layer.number = new_tele_number;
  map->tele_layer.type = new_tele_type;
  map->speedup_layer.force = new_spd_force;
  map->speedup_layer.max_speed = new_spd_max;
  map->speedup_layer.type = new_spd_type;
  map->speedup_layer.angle = new_spd_angle;
  map->switch_layer.number = new_switch_number;
  map->switch_layer.type = new_switch_type;
  map->switch_layer.flags = new_switch_flags;
  map->switch_layer.delay = new_switch_delay;
  map->door_layer.index = new_door_index;
  map->door_layer.flags = new_door_flags;
  map->door_layer.number = new_door_number;
  map->tune_layer.number = new_tune_number;
  map->tune_layer.type = new_tune_type;

  /* update sizes */
  map->width = newW;
  map->height = newH;

  return true;

fail:
  /* free any new buffers allocated so far */
  if (new_game_data)
    free(new_game_data);
  if (new_game_flags)
    free(new_game_flags);
  if (new_front_data)
    free(new_front_data);
  if (new_front_flags)
    free(new_front_flags);
  if (new_tele_number)
    free(new_tele_number);
  if (new_tele_type)
    free(new_tele_type);
  if (new_spd_force)
    free(new_spd_force);
  if (new_spd_max)
    free(new_spd_max);
  if (new_spd_type)
    free(new_spd_type);
  if (new_spd_angle)
    free(new_spd_angle);
  if (new_switch_number)
    free(new_switch_number);
  if (new_switch_type)
    free(new_switch_type);
  if (new_switch_flags)
    free(new_switch_flags);
  if (new_switch_delay)
    free(new_switch_delay);
  if (new_door_index)
    free(new_door_index);
  if (new_door_flags)
    free(new_door_flags);
  if (new_door_number)
    free(new_door_number);
  if (new_tune_number)
    free(new_tune_number);
  if (new_tune_type)
    free(new_tune_type);
  return false;
}

static unsigned char door_layer_at(const map_data_t *__restrict__ pMap, const unsigned char *__restrict__ pLayer, float x, float y) {
  if (!pLayer)
    return 0;
  const int Nx = iclamp((int)roundf(x) / 32, 0, pMap->width - 1);
  const int Ny = iclamp((int)roundf(y) / 32, 0, pMap->height - 1);
  return pLayer[Ny * pMap->width + Nx];
}

// CCollision::CheckPoint.
static bool door_is_solid(const map_data_t *pMap, mvec2 Pos) {
  const unsigned char Index = door_layer_at(pMap, pMap->game_layer.data, vgetx(Pos), vgety(Pos));
  return Index == TILE_SOLID || Index == TILE_NOHOOK;
}

// CCollision::GetTile: the tile itself, but only when it is one of the four
// blocking kinds. ResetCollision uses it to leave a buried door alone.
static bool door_tile_blocks(const map_data_t *pMap, const unsigned char *pLayer, mvec2 Pos) {
  const unsigned char Index = door_layer_at(pMap, pLayer, vgetx(Pos), vgety(Pos));
  return Index >= TILE_SOLID && Index <= TILE_NOLASER;
}

void set_door_collision_at(SCollision *pCollision, float x, float y, unsigned char Type, unsigned char Flags, int Number) {
  map_data_t *pMap = &pCollision->m_MapData;
  if (!pMap->door_layer.index)
    return;
  const int Nx = iclamp((int)roundf(x) / 32, 0, pMap->width - 1);
  const int Ny = iclamp((int)roundf(y) / 32, 0, pMap->height - 1);
  const int Index = Ny * pMap->width + Nx;
  pMap->door_layer.index[Index] = Type;
  pMap->door_layer.flags[Index] = Flags;
  pMap->door_layer.number[Index] = Number;
}

bool intersect_no_laser(SCollision *__restrict__ pCollision, mvec2 Pos0, mvec2 Pos1, mvec2 *__restrict__ pOutCollision,
                        mvec2 *__restrict__ pOutBeforeCollision) {
  const map_data_t *pMap = &pCollision->m_MapData;
  const float Distance = vdistance(Pos0, Pos1);
  const int End = (int)ceilf(Distance);
  mvec2 Last = Pos0;

  for (int i = 0; i < End; ++i) {
    const mvec2 Pos = vvfmix(Pos0, Pos1, (float)i / Distance);
    const unsigned char Index = door_layer_at(pMap, pMap->game_layer.data, vgetx(Pos), vgety(Pos));
    const unsigned char Front = door_layer_at(pMap, pMap->front_layer.data, vgetx(Pos), vgety(Pos));
    if (Index == TILE_SOLID || Index == TILE_NOHOOK || Index == TILE_NOLASER || Front == TILE_NOLASER) {
      if (pOutCollision)
        *pOutCollision = Pos;
      if (pOutBeforeCollision)
        *pOutBeforeCollision = Last;
      return true;
    }
    Last = Pos;
  }

  if (pOutCollision)
    *pOutCollision = Pos1;
  if (pOutBeforeCollision)
    *pOutBeforeCollision = Pos1;
  return false;
}

static void door_reset_collision(SCollision *pCollision, mvec2 Pos, mvec2 Direction, int Length, int Number) {
  const map_data_t *pMap = &pCollision->m_MapData;
  if (door_tile_blocks(pMap, pMap->game_layer.data, Pos) || door_tile_blocks(pMap, pMap->front_layer.data, Pos))
    return;

  for (int i = 0; i < Length - 1; ++i) {
    const mvec2 CurrentPos = vvadd(Pos, vfmul(Direction, (float)i));
    if (door_is_solid(pMap, CurrentPos))
      break;
    set_door_collision_at(pCollision, vgetx(CurrentPos), vgety(CurrentPos), TILE_STOPA, 0, Number);
  }
}

// The same layers, order and side table wc_create_all_entities walks, so a door
// is found wherever DDNet's CGameContext::OnEntity would have found one.
static int door_entity_at(const map_data_t *pMap, int x, int y, int Layer) {
  if ((unsigned int)x >= (unsigned int)pMap->width || (unsigned int)y >= (unsigned int)pMap->height)
    return 0;
  const int Index = y * pMap->width + x;
  switch (Layer) {
  case LAYER_GAME:
    return pMap->game_layer.data[Index] - ENTITY_OFFSET;
  case LAYER_FRONT:
    return pMap->front_layer.data ? pMap->front_layer.data[Index] - ENTITY_OFFSET : 0;
  case LAYER_SWITCH:
    return pMap->switch_layer.type ? pMap->switch_layer.type[Index] - ENTITY_OFFSET : 0;
  default:
    return 0;
  }
}

static void build_doors(SCollision *pCollision) {
  map_data_t *pMap = &pCollision->m_MapData;
  static const int aSideX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  static const int aSideY[8] = {1, 1, 0, -1, -1, -1, 0, 1};
  static const int aLayers[3] = {LAYER_GAME, LAYER_FRONT, LAYER_SWITCH};

  int NumDoors = 0;
  for (int Pass = 0; Pass < 2; ++Pass) {
    if (Pass == 1) {
      if (NumDoors == 0)
        return;

      const size_t MapSize = (size_t)pMap->width * (size_t)pMap->height;
      pMap->door_layer.index = calloc(MapSize, sizeof(*pMap->door_layer.index));
      pMap->door_layer.flags = calloc(MapSize, sizeof(*pMap->door_layer.flags));
      pMap->door_layer.number = calloc(MapSize, sizeof(*pMap->door_layer.number));
      pCollision->m_pDoors = malloc((size_t)NumDoors * sizeof(SDoor));
      if (!pMap->door_layer.index || !pMap->door_layer.flags || !pMap->door_layer.number || !pCollision->m_pDoors) {
        free(pMap->door_layer.index);
        free(pMap->door_layer.flags);
        free(pMap->door_layer.number);
        free(pCollision->m_pDoors);
        pMap->door_layer.index = NULL;
        pMap->door_layer.flags = NULL;
        pMap->door_layer.number = NULL;
        pCollision->m_pDoors = NULL;
        return;
      }
      NumDoors = 0;
    }

    for (int l = 0; l < 3; ++l) {
      const int Layer = aLayers[l];
      for (int y = 0; y < pMap->height; ++y) {
        for (int x = 0; x < pMap->width; ++x) {
          if (door_entity_at(pMap, x, y, Layer) != ENTITY_DOOR)
            continue;
          const int Number = Layer == LAYER_SWITCH && pMap->switch_layer.number ? pMap->switch_layer.number[y * pMap->width + x] : 0;
          const mvec2 Pos = vec2_init((float)x * 32.0f + 16.0f, (float)y * 32.0f + 16.0f);

          for (int i = 0; i < 8; ++i) {
            const int Side = door_entity_at(pMap, x + aSideX[i], y + aSideY[i], Layer);
            if (Side < ENTITY_LASER_SHORT || Side > ENTITY_LASER_LONG)
              continue;
            if (Pass == 0) {
              ++NumDoors;
              continue;
            }

            const int Length = 32 * 3 + 32 * (Side - ENTITY_LASER_SHORT) * 3;
            const mvec2 Direction = vec2_init(sinf((float)i * (PI / 4.0f)), cosf((float)i * (PI / 4.0f)));
            mvec2 To = vvadd(Pos, vfmul(vnormalize(Direction), (float)Length));
            intersect_no_laser(pCollision, Pos, To, &To, NULL);
            door_reset_collision(pCollision, Pos, Direction, Length, Number);
            pCollision->m_pDoors[NumDoors++] = (SDoor){.m_Pos = Pos, .m_To = To, .m_Number = Number};
          }
        }
      }
    }
  }
  pCollision->m_NumDoors = NumDoors;
}

// SCollision now OWNS the pMap data, DO NOT FREE IT
bool init_collision(SCollision *__restrict__ pCollision, map_data_t *__restrict__ pMap) {
  return init_collision_with_no_weapons(pCollision, pMap, false);
}

bool init_collision_with_no_weapons(SCollision *__restrict__ pCollision, map_data_t *__restrict__ pMap, bool NoWeapons) {
  pCollision->m_MapData = *pMap;
  expand_and_shift_map(&pCollision->m_MapData, MAP_EXPAND);
  if (!pCollision->m_MapData.game_layer.data)
    return false;

  map_data_t *pMapData = &pCollision->m_MapData;
  const int Width = pMapData->width;
  const int Height = pMapData->height;
  const int MapSize = Width * Height;

  // Before the tile info passes: they read the door layer this fills in.
  build_doors(pCollision);

  pCollision->m_pTileInfos = _mm_malloc(MapSize * sizeof(char), 64);
  memset(pCollision->m_pTileInfos, 0, MapSize * sizeof(char));

  pCollision->m_pTileBroadCheck = _mm_malloc(MapSize * sizeof(char), 64);
  memset(pCollision->m_pTileBroadCheck, 0, MapSize * sizeof(char));

  pCollision->m_pMoveRestrictions = _mm_malloc(MapSize * NUM_MR_DIRS * sizeof(char), 64);
  memset(pCollision->m_pMoveRestrictions, 0, MapSize * NUM_MR_DIRS * sizeof(char));

  pCollision->m_pPickups = _mm_malloc(MapSize * sizeof(SPickup), 64);
  memset(pCollision->m_pPickups, 0, MapSize * sizeof(SPickup));
  pCollision->m_pFrontPickups = _mm_malloc(MapSize * sizeof(SPickup), 64);
  memset(pCollision->m_pFrontPickups, 0, MapSize * sizeof(SPickup));

  pCollision->m_pBroadSolidBitField = _mm_malloc(MapSize * sizeof(uint64_t), 64);
  memset(pCollision->m_pBroadSolidBitField, 0, MapSize * sizeof(uint64_t));

  pCollision->m_pBroadTeleInBitField = pCollision->m_MapData.tele_layer.type ? _mm_malloc(MapSize * sizeof(uint64_t), 64) : NULL;
  if (pCollision->m_pBroadTeleInBitField)
    memset(pCollision->m_pBroadTeleInBitField, 0, MapSize * sizeof(uint64_t));

  pCollision->m_pBroadIndicesBitField = _mm_malloc(MapSize * sizeof(uint64_t), 64);
  memset(pCollision->m_pBroadIndicesBitField, 0, MapSize * sizeof(uint64_t));
  pCollision->m_MoveRestrictionsFound = false;

  pCollision->m_MapMaxPos = vec2_init((float)Width * 32.f - (HALFPHYSICALSIZE + 2), (float)Height * 32.f - (HALFPHYSICALSIZE + 2));
  pCollision->m_MapClipMax = vec2_init((float)Width * 32.0f - 1.0f, (float)Height * 32.0f - 1.0f);

  pCollision->m_pWidthLookup = _mm_malloc(Height * sizeof(unsigned int), 64);

  for (int i = 0; i < Height; ++i)
    pCollision->m_pWidthLookup[i] = i * Width;

  /*   if (pCollision->m_MapData.tele_layer.type) {
      for (int y = 0; y < Height; ++y) {
        for (int x = 0; x < Width; ++x) {
          if (pCollision->m_MapData.tele_layer.type[y * Width + x])
            printf("%d\n", pCollision->m_MapData.tele_layer.type[y * Width + x]);
        }
      }
    } */

  for (int i = 0; i < NUM_TUNE_ZONES; ++i)
    init_tuning_params(&pCollision->m_aTuningList[i]);
  pCollision->m_aFastcapFlagPresent[0] = false;
  pCollision->m_aFastcapFlagPresent[1] = false;
  apply_map_settings(&pCollision->m_MapData, &(SMapSettingsTarget){
                                                 .m_pTunings = pCollision->m_aTuningList,
                                             });
  // Map settings can change the velramp tunings, so derive after applying them.
  for (int i = 0; i < NUM_TUNE_ZONES; ++i)
    tuning_update_derived(&pCollision->m_aTuningList[i]);
  // Figure out important things
  // Make lists of spawn points, tele outs and tele checkpoints outs
  // figure out highest switch number
  pCollision->m_HighestSwitchNumber = 0;
  if (pCollision->m_MapData.switch_layer.number)
    for (int i = 0; i < MapSize; ++i)
      pCollision->m_HighestSwitchNumber = imax(pCollision->m_HighestSwitchNumber, pCollision->m_MapData.switch_layer.number[i]);

  for (int i = 0; i < MapSize; ++i) {
    if (tile_exists(pCollision, i))
      pCollision->m_pTileInfos[i] |= INFO_TILENEXT;
    const int Tile = pCollision->m_MapData.game_layer.data[i];
    const int FTile = pCollision->m_MapData.front_layer.data ? pCollision->m_MapData.front_layer.data[i] : 0;
    if (Tile == TILE_SOLID || Tile == TILE_NOHOOK)
      pCollision->m_pTileInfos[i] |= INFO_ISSOLID;
    if (pMapData->speedup_layer.force && pMapData->speedup_layer.force[i] > 0)
      pCollision->m_pTileInfos[i] |= INFO_ISSPEEDUP;

    if ((Tile >= 192 && Tile <= 194) || (FTile >= 192 && FTile <= 194))
      ++pCollision->m_NumSpawnPoints;
    if (pMapData->tele_layer.type) {
      if (pMapData->tele_layer.type[i] == TILE_TELEOUT)
        ++pCollision->m_aNumTeleOuts[pMapData->tele_layer.number[i]];
      if (pMapData->tele_layer.type[i] == TILE_TELECHECKOUT)
        ++pCollision->m_aNumTeleCheckOuts[pMapData->tele_layer.number[i]];
    }

    for (int d = 0; d < NUM_MR_DIRS; d++) {
      int Tile;
      int Flags;
      if (pCollision->m_MapData.front_layer.data) {
        Tile = get_front_tile_index(pCollision, i);
        Flags = get_front_tile_flags(pCollision, i);
        pCollision->m_pMoveRestrictions[i][d] |= move_restrictions(d, Tile, Flags);
      }
      Tile = get_tile_index(pCollision, i);
      Flags = get_tile_flags(pCollision, i);
      pCollision->m_pMoveRestrictions[i][d] |= move_restrictions(d, Tile, Flags);

      if (pCollision->m_pMoveRestrictions[i][d])
        pCollision->m_MoveRestrictionsFound = true;
    }

    pCollision->m_pPickups[i].m_Type = -1;
    int EntIdx = pMapData->game_layer.data[i] - ENTITY_OFFSET;
    if (EntIdx == ENTITY_FLAGSTAND_RED || EntIdx == ENTITY_FLAGSTAND_BLUE) {
      const int Team = EntIdx == ENTITY_FLAGSTAND_RED ? 0 : 1;
      pCollision->m_aFastcapFlagPresent[Team] = true;
      pCollision->m_aFastcapFlagPositions[Team] = vec2_init((float)(i % Width) * 32.0f + 16.0f, (float)(i / Width) * 32.0f + 16.0f);
    }
    if ((EntIdx >= ENTITY_ARMOR_SHOTGUN && EntIdx <= ENTITY_ARMOR_LASER) || (EntIdx >= ENTITY_ARMOR_1 && EntIdx <= ENTITY_WEAPON_LASER)) {
      int Type = -1;
      int SubType = 0;
      if (EntIdx == ENTITY_ARMOR_1)
        Type = POWERUP_ARMOR;
      else if (EntIdx == ENTITY_ARMOR_SHOTGUN)
        Type = POWERUP_ARMOR_SHOTGUN;
      else if (EntIdx == ENTITY_ARMOR_GRENADE)
        Type = POWERUP_ARMOR_GRENADE;
      else if (EntIdx == ENTITY_ARMOR_NINJA)
        Type = POWERUP_ARMOR_NINJA;
      else if (EntIdx == ENTITY_ARMOR_LASER)
        Type = POWERUP_ARMOR_LASER;
      else if (EntIdx == ENTITY_HEALTH_1)
        Type = POWERUP_HEALTH;
      else if (EntIdx == ENTITY_WEAPON_SHOTGUN) {
        Type = POWERUP_WEAPON;
        SubType = WEAPON_SHOTGUN;
      } else if (EntIdx == ENTITY_WEAPON_GRENADE) {
        Type = POWERUP_WEAPON;
        SubType = WEAPON_GRENADE;
      } else if (EntIdx == ENTITY_WEAPON_LASER) {
        Type = POWERUP_WEAPON;
        SubType = WEAPON_LASER;
      } else if (EntIdx == ENTITY_POWERUP_NINJA) {
        Type = POWERUP_NINJA;
        SubType = WEAPON_NINJA;
      }
      if (NoWeapons && (Type == POWERUP_WEAPON || Type == POWERUP_NINJA))
        Type = -1;
      pCollision->m_pPickups[i].m_Type = Type;
      pCollision->m_pPickups[i].m_Subtype = SubType;
      if (Type >= 0 && pCollision->m_MapData.switch_layer.type) {
        const int SwitchType = pCollision->m_MapData.switch_layer.type[i];
        if (SwitchType) {
          pCollision->m_pPickups[i].m_Type = SwitchType;
          pCollision->m_pPickups[i].m_Number = pCollision->m_MapData.switch_layer.number[i];
        }
      }
    }

    pCollision->m_pFrontPickups[i].m_Type = -1;
    if (pMapData->front_layer.data) {
      EntIdx = pMapData->front_layer.data[i] - ENTITY_OFFSET;
      if (EntIdx == ENTITY_FLAGSTAND_RED || EntIdx == ENTITY_FLAGSTAND_BLUE) {
        const int Team = EntIdx == ENTITY_FLAGSTAND_RED ? 0 : 1;
        pCollision->m_aFastcapFlagPresent[Team] = true;
        pCollision->m_aFastcapFlagPositions[Team] = vec2_init((float)(i % Width) * 32.0f + 16.0f, (float)(i / Width) * 32.0f + 16.0f);
      }
      if ((EntIdx >= ENTITY_ARMOR_SHOTGUN && EntIdx <= ENTITY_ARMOR_LASER) || (EntIdx >= ENTITY_ARMOR_1 && EntIdx <= ENTITY_WEAPON_LASER)) {
        int Type = -1;
        int SubType = 0;
        if (EntIdx == ENTITY_ARMOR_1)
          Type = POWERUP_ARMOR;
        else if (EntIdx == ENTITY_ARMOR_SHOTGUN)
          Type = POWERUP_ARMOR_SHOTGUN;
        else if (EntIdx == ENTITY_ARMOR_GRENADE)
          Type = POWERUP_ARMOR_GRENADE;
        else if (EntIdx == ENTITY_ARMOR_NINJA)
          Type = POWERUP_ARMOR_NINJA;
        else if (EntIdx == ENTITY_ARMOR_LASER)
          Type = POWERUP_ARMOR_LASER;
        else if (EntIdx == ENTITY_HEALTH_1)
          Type = POWERUP_HEALTH;
        else if (EntIdx == ENTITY_WEAPON_SHOTGUN) {
          Type = POWERUP_WEAPON;
          SubType = WEAPON_SHOTGUN;
        } else if (EntIdx == ENTITY_WEAPON_GRENADE) {
          Type = POWERUP_WEAPON;
          SubType = WEAPON_GRENADE;
        } else if (EntIdx == ENTITY_WEAPON_LASER) {
          Type = POWERUP_WEAPON;
          SubType = WEAPON_LASER;
        } else if (EntIdx == ENTITY_POWERUP_NINJA) {
          Type = POWERUP_NINJA;
          SubType = WEAPON_NINJA;
        }
        if (NoWeapons && (Type == POWERUP_WEAPON || Type == POWERUP_NINJA))
          Type = -1;
        pCollision->m_pFrontPickups[i].m_Type = Type;
        pCollision->m_pFrontPickups[i].m_Subtype = SubType;
        if (Type >= 0 && pCollision->m_MapData.switch_layer.type) {
          const int SwitchType = pCollision->m_MapData.switch_layer.type[i];
          if (SwitchType) {
            pCollision->m_pFrontPickups[i].m_Type = SwitchType;
            pCollision->m_pFrontPickups[i].m_Number = pCollision->m_MapData.switch_layer.number[i];
          }
        }
      }
    }
    if (pMapData->switch_layer.type) {
      EntIdx = pMapData->switch_layer.type[i] - ENTITY_OFFSET;
      if (EntIdx == ENTITY_FLAGSTAND_RED || EntIdx == ENTITY_FLAGSTAND_BLUE) {
        const int Team = EntIdx == ENTITY_FLAGSTAND_RED ? 0 : 1;
        pCollision->m_aFastcapFlagPresent[Team] = true;
        pCollision->m_aFastcapFlagPositions[Team] = vec2_init((float)(i % Width) * 32.0f + 16.0f, (float)(i / Width) * 32.0f + 16.0f);
      }
    }
  }

  for (int y = 0; y < Height; ++y) {
    for (int x = 0; x < Width; ++x) {
      const int Idx = pCollision->m_pWidthLookup[y] + x;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int dIdx = pCollision->m_pWidthLookup[iclamp(y + dy, 0, Height - 1)] + iclamp(x + dx, 0, Width - 1);
          if (pCollision->m_pPickups[dIdx].m_Type >= 0)
            pCollision->m_pTileInfos[Idx] |= INFO_PICKUPNEXT;
          if (pCollision->m_pFrontPickups[dIdx].m_Type >= 0)
            pCollision->m_pTileInfos[Idx] |= INFO_PICKUPNEXT;
          if (pCollision->m_MapData.game_layer.data[dIdx] == TILE_DEATH ||
              (pCollision->m_MapData.front_layer.data && pCollision->m_MapData.front_layer.data[dIdx] == TILE_DEATH))
            pCollision->m_pTileInfos[Idx] |= INFO_CANHITKILL;
        }
      }

      for (int i = -1; i <= 1; ++i) {
        if (pCollision->m_pTileInfos[pCollision->m_pWidthLookup[iclamp(y + 1, 0, Height - 1)] + iclamp(x + i, 0, Width - 1)] & INFO_ISSOLID)
          pCollision->m_pTileInfos[Idx] |= INFO_CANGROUND;
      }
    }
  }

  if (pCollision->m_NumSpawnPoints > 0)
    pCollision->m_pSpawnPoints = malloc(pCollision->m_NumSpawnPoints * sizeof(mvec2));
  if (pMapData->tele_layer.type) {
    for (int i = 0; i < 256; ++i) {
      if (pCollision->m_aNumTeleOuts[i] > 0)
        pCollision->m_apTeleOuts[i] = malloc(pCollision->m_aNumTeleOuts[i] * sizeof(mvec2));
      if (pCollision->m_aNumTeleCheckOuts[i] > 0)
        pCollision->m_apTeleCheckOuts[i] = malloc(pCollision->m_aNumTeleCheckOuts[i] * sizeof(mvec2));
    }
  }

  for (int i = 0; i < MapSize; ++i) {
    if (pCollision->m_MapData.game_layer.data[i])
      pCollision->m_pTileBroadCheck[i] = true;
    else if (pCollision->m_MapData.front_layer.data && pCollision->m_MapData.front_layer.data[i])
      pCollision->m_pTileBroadCheck[i] = true;
    else if (pCollision->m_MapData.tele_layer.type && pCollision->m_MapData.tele_layer.type[i])
      pCollision->m_pTileBroadCheck[i] = true;
    else if (pCollision->m_MapData.switch_layer.type && pCollision->m_MapData.switch_layer.type[i])
      pCollision->m_pTileBroadCheck[i] = true;
    else if (pCollision->m_pTileInfos[i] & INFO_TILENEXT)
      pCollision->m_pTileBroadCheck[i] = true;
  }

  for (int y = 0; y < Height; ++y) {
    for (int x = 0; x < Width; ++x) {
      static const mvec2 DIRECTIONS[NUM_MR_DIRS] = {CTVEC2(0, 0), CTVEC2(18, 0), CTVEC2(0, 18), CTVEC2(-18, 0), CTVEC2(0, -18)};
      unsigned char Restrictions = 0;
#pragma clang loop unroll(full)
      for (int d = 0; d < NUM_MR_DIRS; d++) {
        mvec2 NewPos = vvclamp(vvadd(vec2_init(x * 32 + 16, y * 32 + 16), DIRECTIONS[d]), vec2_init(0, 0),
                               vec2_init((pCollision->m_MapData.width * 32) - 16, (pCollision->m_MapData.height * 32) - 16));
        int ModMapIndex = get_pure_map_index(pCollision, NewPos);

        Restrictions |= pCollision->m_pMoveRestrictions[ModMapIndex][d];

        if (pCollision->m_MapData.door_layer.index && pCollision->m_MapData.door_layer.index[ModMapIndex]) {
          Restrictions |=
              move_restrictions(d, pCollision->m_MapData.door_layer.index[ModMapIndex], pCollision->m_MapData.door_layer.flags[ModMapIndex]);
        }
        if (Restrictions) {
          pCollision->m_pTileInfos[y * Width + x] |= INFO_CANHITSTOPPER;
          break;
        }
      }
    }
  }

  // init_distance_field(pCollision);

  for (int y = 0; y < Height; ++y) {
    for (int x = 0; x < Width; ++x) {
      const int Idx = pCollision->m_pWidthLookup[y] + x;
      const int maxX = imin((Width - 1) - x, 7);
      const int maxY = imin((Height - 1) - y, 7);
      unsigned char aColumnFlags[8] = {0};
      uint64_t BroadField = 0;
      uint64_t SolidField = 0;
      uint64_t TeleField = 0;

      // Set bitfield for all sub-rectangles
      for (int dy = 0; dy <= maxY; ++dy) {
        unsigned char RectFlags = 0;
        const int Row = pCollision->m_pWidthLookup[y + dy] + x;
        for (int dx = 0; dx <= maxX; ++dx) {
          const uint64_t BitIdx = (uint64_t)1 << (dy * 8 + dx);
          unsigned char CellFlags = pCollision->m_pTileBroadCheck[Row + dx] ? 1 : 0;
          CellFlags |= (pCollision->m_pTileInfos[Row + dx] & INFO_ISSOLID) ? 2 : 0;
          if (pCollision->m_MapData.tele_layer.type) {
            const unsigned char Tele = pCollision->m_MapData.tele_layer.type[Row + dx];
            CellFlags |= (Tele == TILE_TELEINHOOK || Tele == TILE_TELEINWEAPON) ? 4 : 0;
          }
          aColumnFlags[dx] |= CellFlags;
          RectFlags |= aColumnFlags[dx];
          if (RectFlags & 1)
            BroadField |= BitIdx;
          if (RectFlags & 2)
            SolidField |= BitIdx;
          if (RectFlags & 4)
            TeleField |= BitIdx;
        }
      }
      pCollision->m_pBroadIndicesBitField[Idx] = BroadField;
      pCollision->m_pBroadSolidBitField[Idx] = SolidField;
      if (pCollision->m_pBroadTeleInBitField)
        pCollision->m_pBroadTeleInBitField[Idx] = TeleField;

// This works, validation not necessary currently
#if 0
      for (int dy = 0; dy <= maxY; ++dy) {
        for (int dx = 0; dx <= maxX; ++dx) {
          bool Hit = false;
          for (int ay = y; ay <= y + dy; ++ay) {
            const unsigned char *rowStart = pCollision->m_pTileInfos + pCollision->m_pWidthLookup[ay];
            for (int ax = x; ax <= x + dx; ++ax) {
              if (rowStart[ax] & INFO_ISSOLID) {
                Hit = true;
                break;
              }
            }
            if (Hit)
              break;
          }

          // Check the corresponding bit in the bitfield
          const uint64_t BitIdx = (uint64_t)1 << (dy * 8 + dx);
          uint64_t Opt = pCollision->m_pBroadSolidBitField[Idx] & BitIdx;
          if (Hit != (bool)Opt) {
            printf("ERROR at (%d, %d) for size (%d, %d): Hit: %d, Opt: %lu\n", x, y, dx, dy, Hit, Opt);
          }
        }
      }
#endif
    }
  }

  // for (int i = 0; i < MapSize; ++i) {
  //   if (i % Width == 0)
  //     printf("\n");
  //   printf(pCollision->m_pTileInfos[i] & INFO_ISSOLID ? "@" : "'");
  // }

  if (!pMapData->tele_layer.type && !pCollision->m_NumSpawnPoints)
    return true;

  int aTeleIdx[256] = {0}, aTeleCheckIdx[256] = {0}, SpawnPointIdx = 0;
  for (int y = 0; y < Height; ++y) {
    for (int x = 0; x < Width; ++x) {
      const int Idx = pCollision->m_pWidthLookup[y] + x;
      if (pCollision->m_NumSpawnPoints) {
        if ((pMapData->game_layer.data[Idx] >= 192 && pMapData->game_layer.data[Idx] <= 194) ||
            (pMapData->front_layer.data && pMapData->front_layer.data[Idx] >= 192 && pMapData->front_layer.data[Idx] <= 194))
          pCollision->m_pSpawnPoints[SpawnPointIdx++] = vec2_init(x, y);
      }
      if (pMapData->tele_layer.type) {
        if (pMapData->tele_layer.type[Idx] == TILE_TELEOUT)
          pCollision->m_apTeleOuts[pMapData->tele_layer.number[Idx]][aTeleIdx[pMapData->tele_layer.number[Idx]]++] =
              vec2_init((x * 32.f) + 16.f, (y * 32.f) + 16.f);
        if (pMapData->tele_layer.type[Idx] == TILE_TELECHECKOUT)
          pCollision->m_apTeleCheckOuts[pMapData->tele_layer.number[Idx]][aTeleCheckIdx[pMapData->tele_layer.number[Idx]]++] =
              vec2_init((x * 32.f) + 16.f, (y * 32.f) + 16.f);
      }
    }
  }

  // fill dispatch table for handle_tiles
  cc_init_tile_handlers();
  return true;
}

void free_collision(SCollision *pCollision) {
  if (!pCollision)
    return;

  free_map_data(&pCollision->m_MapData);
  if (pCollision->m_pPickups)
    _mm_free(pCollision->m_pPickups);
  if (pCollision->m_pFrontPickups)
    _mm_free(pCollision->m_pFrontPickups);
  if (pCollision->m_pMoveRestrictions)
    _mm_free(pCollision->m_pMoveRestrictions);
  if (pCollision->m_pSolidTeleDistanceField)
    _mm_free(pCollision->m_pSolidTeleDistanceField);
  if (pCollision->m_pTileBroadCheck)
    _mm_free(pCollision->m_pTileBroadCheck);
  if (pCollision->m_pWidthLookup)
    _mm_free(pCollision->m_pWidthLookup);
  if (pCollision->m_pBroadSolidBitField)
    _mm_free(pCollision->m_pBroadSolidBitField);
  if (pCollision->m_pBroadTeleInBitField)
    _mm_free(pCollision->m_pBroadTeleInBitField);
  if (pCollision->m_pBroadIndicesBitField)
    _mm_free(pCollision->m_pBroadIndicesBitField);
  if (pCollision->m_pTileInfos)
    _mm_free(pCollision->m_pTileInfos);

  // The door layer itself belongs to the map data free_map_data just released.
  free(pCollision->m_pDoors);

  // Free spawn points
  if (pCollision->m_NumSpawnPoints)
    free(pCollision->m_pSpawnPoints);

  // Free tele layer arrays
  for (int i = 0; i < 256; ++i) {
    free(pCollision->m_apTeleOuts[i]);
    free(pCollision->m_apTeleCheckOuts[i]);
  }

  // Zero everything
  memset(pCollision, 0, sizeof(SCollision));
}

int get_pure_map_index(SCollision *pCollision, mvec2 Pos) {
  const int nx = (int)(vgetx(Pos) + 0.5f) >> 5;
  const int ny = (int)(vgety(Pos) + 0.5f) >> 5;
  return pCollision->m_pWidthLookup[ny] + nx;
}

static unsigned char get_move_restrictions_raw(unsigned char Tile, unsigned char Flags) {
  Flags &= (TILEFLAG_XFLIP | TILEFLAG_YFLIP | TILEFLAG_ROTATE);
  switch (Tile) {
  case TILE_STOP: {
    static const int move_table[] = {
        CANTMOVE_DOWN, // 0: ROTATION_0
        CANTMOVE_DOWN, // 1: TILEFLAG_YFLIP ^ ROTATION_180
        CANTMOVE_UP,   // 2: TILEFLAG_YFLIP ^ ROTATION_0
        CANTMOVE_UP,   // 3: ROTATION_180
        0,
        0,
        0,
        0,              // 4-7: unused
        CANTMOVE_LEFT,  // 8: ROTATION_90
        CANTMOVE_LEFT,  // 9: TILEFLAG_YFLIP ^ ROTATION_270
        CANTMOVE_RIGHT, // 10: TILEFLAG_YFLIP ^ ROTATION_90
        CANTMOVE_RIGHT  // 11: ROTATION_270
    };
    return move_table[Flags];
  }
  case TILE_STOPS:
    return (Flags & TILEFLAG_ROTATE) ? (CANTMOVE_LEFT | CANTMOVE_RIGHT) : (CANTMOVE_DOWN | CANTMOVE_UP);
  case TILE_STOPA:
    return CANTMOVE_LEFT | CANTMOVE_RIGHT | CANTMOVE_UP | CANTMOVE_DOWN;
  default:
    return 0;
  }
}

unsigned char move_restrictions(unsigned char Direction, unsigned char Tile, unsigned char Flags) {
  unsigned char Result = get_move_restrictions_raw(Tile, Flags);
  if (Direction == MR_DIR_HERE && Tile == TILE_STOP)
    return Result;
  static const unsigned char aDirections[NUM_MR_DIRS] = {0, CANTMOVE_RIGHT, CANTMOVE_DOWN, CANTMOVE_LEFT, CANTMOVE_UP};
  return Result & aDirections[Direction];
}

unsigned char get_tile_index(SCollision *pCollision, int Index) { return pCollision->m_MapData.game_layer.data[Index]; }
unsigned char get_front_tile_index(SCollision *pCollision, int Index) { return pCollision->m_MapData.front_layer.data[Index]; }
unsigned char get_tile_flags(SCollision *pCollision, int Index) { return pCollision->m_MapData.game_layer.flags[Index]; }
unsigned char get_front_tile_flags(SCollision *pCollision, int Index) { return pCollision->m_MapData.front_layer.flags[Index]; }
unsigned char get_switch_number(SCollision *pCollision, int Index) { return pCollision->m_MapData.switch_layer.number[Index]; }
unsigned char get_switch_type(SCollision *pCollision, int Index) { return pCollision->m_MapData.switch_layer.type[Index]; }
unsigned char get_switch_delay(SCollision *pCollision, int Index) { return pCollision->m_MapData.switch_layer.delay[Index]; }
unsigned char is_teleport(SCollision *pCollision, int Index) {
  return pCollision->m_MapData.tele_layer.type[Index] == TILE_TELEIN ? pCollision->m_MapData.tele_layer.number[Index] : 0;
}
unsigned char is_teleport_hook(SCollision *pCollision, int Index) {
  return pCollision->m_MapData.tele_layer.type[Index] == TILE_TELEINHOOK ? pCollision->m_MapData.tele_layer.number[Index] : 0;
}
unsigned char is_teleport_weapon(SCollision *pCollision, int Index) {
  return pCollision->m_MapData.tele_layer.type[Index] == TILE_TELEINWEAPON ? pCollision->m_MapData.tele_layer.number[Index] : 0;
}
unsigned char is_evil_teleport(SCollision *pCollision, int Index) {
  return pCollision->m_MapData.tele_layer.type[Index] == TILE_TELEINEVIL ? pCollision->m_MapData.tele_layer.number[Index] : 0;
}
unsigned char is_check_teleport(SCollision *pCollision, int Index) {
  return pCollision->m_MapData.tele_layer.type[Index] == TILE_TELECHECKIN ? pCollision->m_MapData.tele_layer.number[Index] : 0;
}
unsigned char is_check_evil_teleport(SCollision *pCollision, int Index) {
  return pCollision->m_MapData.tele_layer.type[Index] == TILE_TELECHECKINEVIL ? pCollision->m_MapData.tele_layer.number[Index] : 0;
}
unsigned char is_tele_checkpoint(SCollision *pCollision, int Index) {
  return pCollision->m_MapData.tele_layer.type[Index] == TILE_TELECHECK ? pCollision->m_MapData.tele_layer.number[Index] : 0;
}

unsigned char get_collision_at(SCollision *pCollision, mvec2 Pos) {
  const int Nx = (int)vgetx(Pos) >> 5;
  const int Ny = (int)vgety(Pos) >> 5;
  const unsigned char Idx = pCollision->m_MapData.game_layer.data[pCollision->m_pWidthLookup[Ny] + Nx];
  return Idx * (Idx - 1 <= TILE_NOLASER - 1);
}
__attribute__((unused)) static unsigned char get_collision_at_idx(SCollision *pCollision, int Idx) {
  const unsigned char Tile = pCollision->m_MapData.game_layer.data[Idx];
  return Tile * (Tile - 1 <= TILE_NOLASER - 1);
}

unsigned char get_front_collision_at(SCollision *pCollision, mvec2 Pos) {
  const int Nx = (int)vgetx(Pos) >> 5;
  const int Ny = (int)vgety(Pos) >> 5;
  const int pos = pCollision->m_pWidthLookup[Ny] + Nx;
  const unsigned char Idx = pCollision->m_MapData.front_layer.data[pos];
  return Idx * (Idx - 1 <= TILE_NOLASER - 1);
}

unsigned char get_move_restrictions(SWorldCore *pWorld, mvec2 Pos, int Idx) {

  SCollision *pCollision = pWorld->m_pCollision;
  if (!pCollision->m_MoveRestrictionsFound && !pCollision->m_MapData.door_layer.index)
    return 0;
  if (!(pCollision->m_pTileInfos[Idx] & INFO_CANHITSTOPPER))
    return 0;

  static const mvec2 DIRECTIONS[NUM_MR_DIRS] = {CTVEC2(0, 0), CTVEC2(18, 0), CTVEC2(0, 18), CTVEC2(-18, 0), CTVEC2(0, -18)};
  unsigned char Restrictions = 0;
#pragma clang loop unroll(full)
  for (int d = 0; d < NUM_MR_DIRS; d++) {
    mvec2 NewPos = vvclamp(vvadd(Pos, DIRECTIONS[d]), vec2_init(0, 0),
                           vec2_init((pCollision->m_MapData.width * 32) - 16, (pCollision->m_MapData.height * 32) - 16));
    int ModMapIndex = get_pure_map_index(pCollision, NewPos);

    Restrictions |= pCollision->m_pMoveRestrictions[ModMapIndex][d];

    if (pCollision->m_MapData.door_layer.index && pCollision->m_MapData.door_layer.index[ModMapIndex]) {
      if (pWorld->m_pSwitches && pWorld->m_pSwitches[pCollision->m_MapData.door_layer.number[ModMapIndex]].m_Status) {
        Restrictions |=
            move_restrictions(d, pCollision->m_MapData.door_layer.index[ModMapIndex], pCollision->m_MapData.door_layer.flags[ModMapIndex]);
      }
    }
  }
  return Restrictions;
}

int get_map_index(SCollision *pCollision, mvec2 Pos) {
  const int Nx = (int)vgetx(Pos) >> 5;
  const int Ny = (int)vgety(Pos) >> 5;
  const int Index = pCollision->m_pWidthLookup[Ny] + Nx;
  if (pCollision->m_pTileInfos[Index] & INFO_TILENEXT)
    return Index;
  return -1;
}

bool check_point(SCollision *pCollision, mvec2 Pos) {
  const int Nx = (int)(vgetx(Pos) + 0.5f) >> 5;
  const int Ny = (int)(vgety(Pos) + 0.5f) >> 5;
  return pCollision->m_pTileInfos[pCollision->m_pWidthLookup[Ny] + Nx] & INFO_ISSOLID;
}

static bool check_point_idx(SCollision *pCollision, int Idx) { return pCollision->m_pTileInfos[Idx] & INFO_ISSOLID; }

static void through_offset(mvec2 Pos0, mvec2 Pos1, int *__restrict__ pOffsetX, int *__restrict__ pOffsetY) {
  static const int offsets[8][2] = {{0, 32}, {32, 0}, {0, -32}, {32, 0}, {0, 32}, {-32, 0}, {0, -32}, {-32, 0}};
  const float dx = vgetx(Pos0) - vgetx(Pos1);
  const float dy = vgety(Pos0) - vgety(Pos1);
  int index = ((dx < 0.0f) << 2) | ((dy < 0.0f) << 1) | ((dx >= 0.0f ? dx : -dx) > (dy >= 0.0f ? dy : -dy));
  *pOffsetX = offsets[index][0];
  *pOffsetY = offsets[index][1];
}

static bool is_through(SCollision *pCollision, int x, int y, int OffsetX, int OffsetY, mvec2 Pos0, mvec2 Pos1) {
  if (x < 0 || y < 0 || x >= pCollision->m_MapData.width * 32 || y >= pCollision->m_MapData.height * 32)
    return false;
  int pos = get_pure_map_index(pCollision, vec2_init(x, y));
  unsigned char *pFrontIdx = pCollision->m_MapData.front_layer.data;
  unsigned char *pFrontFlgs = pCollision->m_MapData.front_layer.flags;

  if (pFrontIdx) {
    unsigned char FrontTile = pFrontIdx[pos];
    if (FrontTile == TILE_THROUGH_ALL || FrontTile == TILE_THROUGH_CUT)
      return true;
    if (FrontTile == TILE_THROUGH_DIR) {
      unsigned char Flags = pFrontFlgs[pos];
      if ((Flags == ROTATION_0 && vgety(Pos0) > vgety(Pos1)) || (Flags == ROTATION_90 && vgetx(Pos0) < vgetx(Pos1)) ||
          (Flags == ROTATION_180 && vgety(Pos0) < vgety(Pos1)) || (Flags == ROTATION_270 && vgetx(Pos0) > vgetx(Pos1))) {
        return true;
      }
    }
  }
  int offpos = get_pure_map_index(pCollision, vec2_init(x + OffsetX, y + OffsetY));
  if (offpos < 0)
    return false;

  unsigned char *pTileIdx = pCollision->m_MapData.game_layer.data;
  return pTileIdx[offpos] == TILE_THROUGH || (pFrontIdx && pFrontIdx[offpos] == TILE_THROUGH);
}

bool is_hook_blocker(SCollision *pCollision, int Index, mvec2 Pos0, mvec2 Pos1) {
  unsigned char *pTileIdx = pCollision->m_MapData.game_layer.data;
  unsigned char *pTileFlgs = pCollision->m_MapData.game_layer.flags;

  unsigned char *pFrontIdx = pCollision->m_MapData.front_layer.data;
  unsigned char *pFrontFlgs = pCollision->m_MapData.front_layer.flags;
  if (pTileIdx[Index] == TILE_THROUGH_ALL || (pFrontIdx && pFrontIdx[Index] == TILE_THROUGH_ALL))
    return true;
  if (pTileIdx[Index] == TILE_THROUGH_DIR &&
      ((pTileFlgs[Index] == ROTATION_0 && vgety(Pos0) < vgety(Pos1)) || (pTileFlgs[Index] == ROTATION_90 && vgetx(Pos0) > vgetx(Pos1)) ||
       (pTileFlgs[Index] == ROTATION_180 && vgety(Pos0) > vgety(Pos1)) || (pTileFlgs[Index] == ROTATION_270 && vgetx(Pos0) < vgetx(Pos1))))
    return true;
  if (pFrontIdx && pFrontIdx[Index] == TILE_THROUGH_DIR &&
      ((pFrontFlgs[Index] == ROTATION_0 && vgety(Pos0) < vgety(Pos1)) || (pFrontFlgs[Index] == ROTATION_90 && vgetx(Pos0) > vgetx(Pos1)) ||
       (pFrontFlgs[Index] == ROTATION_180 && vgety(Pos0) > vgety(Pos1)) || (pFrontFlgs[Index] == ROTATION_270 && vgetx(Pos0) < vgetx(Pos1))))
    return true;
  return false;
}

static bool broad_check(const SCollision *__restrict__ pCollision, mvec2 Start, mvec2 End) {
  const __m128i MinI = _mm_srai_epi32(_mm_cvttps_epi32(_mm_min_ps(Start, End)), 5);
  const __m128i MaxI = _mm_srai_epi32(_mm_cvttps_epi32(_mm_max_ps(Start, End)), 5);
  const __m128i DiffI = _mm_sub_epi32(MaxI, MinI);
  const int MinX = _mm_cvtsi128_si32(MinI);
  const int MinY = _mm_extract_epi32(MinI, 1);
  const int MaxX = _mm_cvtsi128_si32(MaxI);
  const int MaxY = _mm_extract_epi32(MaxI, 1);
  if (MinY < 0 || MaxY >= pCollision->m_MapData.height || MinX < 0 || MaxX >= pCollision->m_MapData.width)
    return 0;

  return (bool)(pCollision->m_pBroadSolidBitField[(MinY * pCollision->m_MapData.width) + MinX] &
                (uint64_t)1 << ((_mm_extract_epi32(DiffI, 1) << 3) + _mm_cvtsi128_si32(DiffI)));
}

static bool broad_check_tele(const SCollision *__restrict__ pCollision, mvec2 Start, mvec2 End) {
  const __m128i MinI = _mm_srai_epi32(_mm_cvttps_epi32(_mm_min_ps(Start, End)), 5);
  const __m128i MaxI = _mm_srai_epi32(_mm_cvttps_epi32(_mm_max_ps(Start, End)), 5);
  const __m128i DiffI = _mm_sub_epi32(MaxI, MinI);
  const int MinX = _mm_cvtsi128_si32(MinI);
  const int MinY = _mm_extract_epi32(MinI, 1);
  const int MaxX = _mm_cvtsi128_si32(MaxI);
  const int MaxY = _mm_extract_epi32(MaxI, 1);
  if (MinY < 0 || MaxY >= pCollision->m_MapData.height || MinX < 0 || MaxX >= pCollision->m_MapData.width)
    return 0;

  return (bool)(pCollision->m_pBroadTeleInBitField[pCollision->m_pWidthLookup[MinY] + MinX] &
                (uint64_t)1 << ((_mm_extract_epi32(DiffI, 1) << 3) + _mm_cvtsi128_si32(DiffI)));
}

#define TILE_SHIFT 5
#define TILE_SIZE (1 << TILE_SHIFT)

#if 0
static float fast_absf(float v) { return v < 0.0f ? -v : v; }
// Original intersect code
unsigned char intersect_line_tele_hook(SCollision *__restrict__ pCollision, mvec2 Pos0, mvec2 Pos1,
                                       mvec2 *__restrict__ pOutCollision,
                                       unsigned char *__restrict__ pTeleNr) {
  float Distance = vdistance(Pos0, Pos1);
  int End = (Distance + 1);
  int dx = 0, dy = 0; // Offset for checking the "through" tile
  through_offset(Pos0, Pos1, &dx, &dy);
  for (int i = 0; i <= End; i++) {
    float a = i / (float)End;
    mvec2 Pos = vvfmix(Pos0, Pos1, a);
    // Temporary position for checking collision
    int ix = vgetx(Pos) + 0.5f;
    int iy = vgety(Pos) + 0.5f;

    int Index = get_pure_map_index(pCollision, Pos);
    if (pTeleNr) {
      *pTeleNr = is_teleport_hook(pCollision, Index);
    }
    if (pTeleNr && *pTeleNr) {
      if (pOutCollision)
        *pOutCollision = Pos;
      return TILE_TELEINHOOK;
    }

    int hit = 0;
    if (check_point_idx(pCollision, Index)) {
      if (!is_through(pCollision, ix, iy, dx, dy, Pos0, Pos1))
        hit = get_collision_at_idx(pCollision, Index);
    } else if (is_hook_blocker(pCollision, Index, Pos0, Pos1)) {
      hit = TILE_NOHOOK;
    }
    if (hit) {
      if (pOutCollision)
        *pOutCollision = Pos;
      return hit;
    }
  }
  if (pOutCollision)
    *pOutCollision = Pos1;
  return 0;
}
#else

unsigned char intersect_line_tele_hook(SCollision *__restrict__ pCollision, mvec2 Pos0, mvec2 Pos1, mvec2 *__restrict__ pOutCollision,
                                       unsigned char *__restrict__ pTeleNr) {
  // Plain locals rather than a two-element array: the array form made clang
  // materialise a stack slot on the early-out path, which is the common one.
  const bool SolidNear = broad_check(pCollision, Pos0, Pos1);
  const bool TeleNear = pTeleNr ? broad_check_tele(pCollision, Pos0, Pos1) : false;
  if (!SolidNear && !TeleNear) {
    *pOutCollision = Pos1;
    return 0;
  }

  // Voxel DDA instead of marching the ray in ~1 unit samples. Measured, the old
  // loop scanned 28.8 samples per surviving call to find 2.5 distinct tiles; the
  // traversal below visits exactly those tiles in the same order. Tile indices
  // use the engine's round-then-shift convention ((int)(v + 0.5f) >> 5), so the
  // walk is done in a +0.5 shifted space where tile edges sit on multiples of 32.
  const int Width = pCollision->m_MapData.width;
  const int MapSize = Width * pCollision->m_MapData.height;
  const float x0 = vgetx(Pos0) + 0.5f, y0 = vgety(Pos0) + 0.5f;
  const float x1 = vgetx(Pos1) + 0.5f, y1 = vgety(Pos1) + 0.5f;
  const float ddx = x1 - x0, ddy = y1 - y0;

  int tx = (int)x0 >> 5, ty = (int)y0 >> 5;
  const int StepX = ddx > 0.f ? 1 : -1, StepY = ddy > 0.f ? 1 : -1;
  const float AbsDx = ddx > 0.f ? ddx : -ddx, AbsDy = ddy > 0.f ? ddy : -ddy;
  float tMaxX = AbsDx > 0.f ? (((ddx > 0.f ? (float)((tx + 1) << 5) : (float)(tx << 5)) - x0) / ddx) : FLT_MAX;
  float tMaxY = AbsDy > 0.f ? (((ddy > 0.f ? (float)((ty + 1) << 5) : (float)(ty << 5)) - y0) / ddy) : FLT_MAX;
  const float tDeltaX = AbsDx > 0.f ? 32.0f / AbsDx : FLT_MAX;
  const float tDeltaY = AbsDy > 0.f ? 32.0f / AbsDy : FLT_MAX;

  int dx = 0, dy = 0;
  through_offset(Pos0, Pos1, &dx, &dy);

  float t = 0.f;
  for (int Guard = 0; Guard < 64; ++Guard) {
    const int Index = ty * Width + tx;
    if ((unsigned int)Index >= (unsigned int)MapSize)
      break;
    if (pTeleNr) {
      *pTeleNr = is_teleport_hook(pCollision, Index);
      if (*pTeleNr) {
        *pOutCollision = vvfmix(Pos0, Pos1, t);
        return TILE_TELEINHOOK;
      }
    }
    if (check_point_idx(pCollision, Index)) {
      if (!is_through(pCollision, tx << 5, ty << 5, dx, dy, Pos0, Pos1)) {
        *pOutCollision = vvfmix(Pos0, Pos1, t);
        return pCollision->m_MapData.game_layer.data[Index];
      }
    } else if (is_hook_blocker(pCollision, Index, Pos0, Pos1)) {
      *pOutCollision = vvfmix(Pos0, Pos1, t);
      return TILE_NOHOOK;
    }
    if (tMaxX < tMaxY) {
      t = tMaxX;
      tMaxX += tDeltaX;
      tx += StepX;
    } else if (tMaxY < tMaxX) {
      t = tMaxY;
      tMaxY += tDeltaY;
      ty += StepY;
    } else {
      t = tMaxX;
      tMaxX += tDeltaX;
      tMaxY += tDeltaY;
      tx += StepX;
      ty += StepY;
    }
    if (t > 1.f)
      break;
  }
  *pOutCollision = Pos1;
  return 0;
}
#endif
static float fast_absf(float v) { return v < 0.0f ? -v : v; }
unsigned char intersect_line_tele_weapon(SCollision *__restrict__ pCollision, mvec2 Pos0, mvec2 Pos1, mvec2 *__restrict__ pOutCollision,
                                         unsigned char *__restrict__ pTeleNr) {
#define NORMALIZE()                                                                                                                                  \
  if (vgetx(Pos0) < vgetx(Pos1))                                                                                                                     \
    *pOutCollision = vsetx(*pOutCollision, vgetx(*pOutCollision) - 1);                                                                               \
  if (vgety(Pos0) < vgety(Pos1))                                                                                                                     \
    *pOutCollision = vsety(*pOutCollision, vgety(*pOutCollision) - 1);

  const int Width = pCollision->m_MapData.width;
  const unsigned char *game = pCollision->m_MapData.game_layer.data;

  const float x0 = vgetx(Pos0);
  const float y0 = vgety(Pos0);
  const float x1 = vgetx(Pos1);
  const float y1 = vgety(Pos1);

  const float dx = x1 - x0;
  const float dy = y1 - y0;

  if (dx == 0.0f && dy == 0.0f) {
    int ix = ((int)(x0 + 0.5f)) >> TILE_SHIFT;
    int iy = ((int)(y0 + 0.5f)) >> TILE_SHIFT;
    int idx = iy * Width + ix;

    if (pTeleNr) {
      unsigned char tele = is_teleport_hook(pCollision, idx);
      if (tele) {
        *pTeleNr = tele;
        *pOutCollision = Pos0;
        return TILE_TELEINWEAPON;
      }
    }

    if (check_point_idx(pCollision, idx)) {
      *pOutCollision = Pos0;
      return game[idx];
    }

    *pOutCollision = Pos1;
    return 0;
  }

  int mapX = ((int)(x0 + 0.5f)) >> TILE_SHIFT;
  int mapY = ((int)(y0 + 0.5f)) >> TILE_SHIFT;
  const int endX = ((int)(x1 + 0.5f)) >> TILE_SHIFT;
  const int endY = ((int)(y1 + 0.5f)) >> TILE_SHIFT;

  const int stepX = (dx > 0.0f) ? 1 : ((dx < 0.0f) ? -1 : 0);
  const int stepY = (dy > 0.0f) ? 1 : ((dy < 0.0f) ? -1 : 0);

  float inv_dx = (dx != 0.0f) ? 1.0f / dx : 0.0f;
  float inv_dy = (dy != 0.0f) ? 1.0f / dy : 0.0f;
  const float absInvDX = fast_absf(inv_dx);
  const float absInvDY = fast_absf(inv_dy);

  float tMaxX = 1e30f, tMaxY = 1e30f;
  float tDeltaX = 1e30f, tDeltaY = 1e30f;

  if (stepX != 0) {
    int nextBoundaryX = (stepX > 0) ? ((mapX + 1) << TILE_SHIFT) : (mapX << TILE_SHIFT);
    tMaxX = (nextBoundaryX - x0) * inv_dx;
    if (tMaxX < 0.0f)
      tMaxX = 0.0f;
    tDeltaX = (float)TILE_SIZE * absInvDX;
  }

  if (stepY != 0) {
    int nextBoundaryY = (stepY > 0) ? ((mapY + 1) << TILE_SHIFT) : (mapY << TILE_SHIFT);
    tMaxY = (nextBoundaryY - y0) * inv_dy;
    if (tMaxY < 0.0f)
      tMaxY = 0.0f;
    tDeltaY = (float)TILE_SIZE * absInvDY;
  }

  float u = 0.0f;
  int idx = mapY * Width + mapX;

  for (;;) {
    if (pTeleNr) {
      unsigned char tele = is_teleport_weapon(pCollision, idx);
      if (tele) {
        *pTeleNr = tele;
        *pOutCollision = vvfmix(Pos0, Pos1, u);
        NORMALIZE()
        return TILE_TELEINWEAPON;
      }
    }

    if (check_point_idx(pCollision, idx)) {
      *pOutCollision = vvfmix(Pos0, Pos1, u);
      NORMALIZE()
      return game[idx];
    }

    if (mapX == endX && mapY == endY)
      break;

    if (tMaxX < tMaxY) {
      mapX += stepX;
      idx += stepX;
      u = tMaxX;
      tMaxX += tDeltaX;
    } else {
      mapY += stepY;
      idx += stepY * Width;
      u = tMaxY;
      tMaxY += tDeltaY;
    }

    if (u > 1.0f) {
      u = 1.0f;
      mapX = endX;
      mapY = endY;
      idx = endY * Width + endX;
      break;
    }
  }

  *pOutCollision = Pos1;
  return 0;
#undef NORMALIZE
}

bool test_box(SCollision *pCollision, mvec2 Pos, mvec2 Size) {
  float SizeX = vgetx(Size) * 0.5f;
  float SizeY = vgety(Size) * 0.5f;
  float PosX = vgetx(Pos);
  float PosY = vgety(Pos);
  if (check_point(pCollision, vec2_init(PosX - SizeX, PosY - SizeY)))
    return true;
  if (check_point(pCollision, vec2_init(PosX + SizeX, PosY - SizeY)))
    return true;
  if (check_point(pCollision, vec2_init(PosX - SizeX, PosY + SizeY)))
    return true;
  if (check_point(pCollision, vec2_init(PosX + SizeX, PosY + SizeY)))
    return true;
  return false;
}

unsigned char is_tune(SCollision *pCollision, int Index) {
  if (!pCollision->m_MapData.tune_layer.type)
    return 0;
  if (Index < 0)
    return 0;
  if (pCollision->m_MapData.tune_layer.type[Index])
    return pCollision->m_MapData.tune_layer.number[Index];
  return 0;
}

bool is_speedup(SCollision *pCollision, int Index) {
  return pCollision->m_MapData.speedup_layer.type && pCollision->m_MapData.speedup_layer.force[Index] > 0;
}

void get_speedup(SCollision *__restrict__ pCollision, int Index, mvec2 *__restrict__ pDir, int *__restrict__ pForce, int *__restrict__ pMaxSpeed,
                 int *__restrict__ pType) {
  float Angle = pCollision->m_MapData.speedup_layer.angle[Index] * (PI / 180.0f);
  *pForce = pCollision->m_MapData.speedup_layer.force[Index];
  *pType = pCollision->m_MapData.speedup_layer.type[Index];
  *pDir = vdirection(Angle);
  if (pMaxSpeed)
    *pMaxSpeed = pCollision->m_MapData.speedup_layer.max_speed[Index];
}

// TODO: do the same optimization as in intersect_line_tele_hook
bool intersect_line(SCollision *__restrict__ pCollision, mvec2 Pos0, mvec2 Pos1, mvec2 *__restrict__ pOutCollision,
                    mvec2 *__restrict__ pOutBeforeCollision) {
  if (!broad_check(pCollision, Pos0, Pos1)) {
    *pOutCollision = Pos1;
    *pOutBeforeCollision = Pos1;
    return 0;
  }

  float Distance = vdistance(Pos0, Pos1);
  int End = Distance + 1;
  mvec2 Last = Pos0;
  int LastIdx = -1;
  for (int i = 0; i <= End; i++) {
    float a = i / (float)End;
    mvec2 Pos = vvfmix(Pos0, Pos1, a);
    int Nx = (int)(vgetx(Pos) + 0.5f) >> 5;
    int Ny = (int)(vgety(Pos) + 0.5f) >> 5;
    int Idx = pCollision->m_pWidthLookup[Ny] + Nx;
    if (LastIdx == Idx)
      continue;
    LastIdx = Idx;
    if (check_point_idx(pCollision, Idx)) {
      *pOutCollision = Pos;
      *pOutBeforeCollision = Last;
      return true;
    }

    Last = Pos;
  }
  *pOutCollision = Pos1;
  *pOutBeforeCollision = Pos1;
  return false;
}

static inline bool test_box_character_mask(const SCollision *__restrict__ pCollision, unsigned int x, unsigned int y, uint32_t BoundaryMaskX,
                                           uint32_t BoundaryMaskY) {
  // Character coordinates are always positive and inside the expanded map.
  const uint32_t frac_x = x & 31;
  const uint32_t frac_y = y & 31;
  if (((BoundaryMaskX >> frac_x) & 1U) == 0 && ((BoundaryMaskY >> frac_y) & 1U) == 0)
    return false;

  const unsigned char *__restrict__ pInfos = pCollision->m_pTileInfos;
  const unsigned int Width = pCollision->m_MapData.width;
  const unsigned int RowBottom = ((y + HALFPHYSICALSIZE) >> 5) * Width;
  const unsigned int RowTop = ((y - HALFPHYSICALSIZE) >> 5) * Width;
  const unsigned int ColLeft = (x - HALFPHYSICALSIZE) >> 5;
  const unsigned int ColRight = (x + HALFPHYSICALSIZE) >> 5;

  return ((pInfos[RowBottom + ColLeft] | pInfos[RowBottom + ColRight] | pInfos[RowTop + ColLeft] | pInfos[RowTop + ColRight]) & INFO_ISSOLID) != 0;
}

bool test_box_character(const SCollision *__restrict__ pCollision, int x, int y) {
  const uint32_t BoundaryMask = (1U << 13) | (1U << 18);
  return test_box_character_mask(pCollision, (unsigned int)x, (unsigned int)y, BoundaryMask, BoundaryMask);
}

static inline uivec2 round_pos_i(mvec2 v) {
  const __m128i i = _mm_cvttps_epi32(_mm_add_ps(v, _mm_set1_ps(0.5f)));
  uivec2 r;
  r.x = (unsigned int)_mm_cvtsi128_si32(i);
  r.y = (unsigned int)_mm_extract_epi32(i, 1);
  return r;
}

void move_box(const SCollision *__restrict__ pCollision, mvec2 Pos, mvec2 Vel, mvec2 *__restrict__ pOutPos, mvec2 *__restrict__ pOutVel,
              bool *__restrict__ pGrounded) {
  const mvec2 AbsVel = _mm_andnot_ps(_mm_set1_ps(-0.0f), Vel);
  const float MaxComponent = _mm_cvtss_f32(_mm_max_ss(AbsVel, _mm_shuffle_ps(AbsVel, AbsVel, _MM_SHUFFLE(1, 1, 1, 1))));
  if (MaxComponent <= 0.00001f)
    return;

  mvec2 NewPos = vvadd(Pos, Vel);

  const mvec2 minVec = _mm_min_ps(Pos, NewPos);
  const mvec2 maxVec = _mm_max_ps(Pos, NewPos);
  const mvec2 offset = _mm_set1_ps(HALFPHYSICALSIZE + 1.0f);
  const mvec2 minAdj = _mm_sub_ps(minVec, offset);
  const mvec2 maxAdj = _mm_add_ps(maxVec, offset);
  // The expanded map keeps the entire swept box in positive coordinates, so
  // logical shifts preserve the tile indices and address math stays unsigned.
  const __m128i MinI = _mm_srli_epi32(_mm_cvttps_epi32(minAdj), 5);
  const __m128i MaxI = _mm_srli_epi32(_mm_cvttps_epi32(maxAdj), 5);
  const __m128i DiffI = _mm_sub_epi32(MaxI, MinI);
  const unsigned int MinX = _mm_cvtsi128_si32(MinI);
  const unsigned int MinY = _mm_extract_epi32(MinI, 1);
  // bitshift by the index in the 8x8 block (max 63)
  const uint64_t Mask = (uint64_t)1 << ((_mm_extract_epi32(DiffI, 1) << 3) + _mm_cvtsi128_si32(DiffI));
  const unsigned int BroadIndex = MinY * (unsigned int)pCollision->m_MapData.width + MinX;
  const uint64_t IsSolid = pCollision->m_pBroadSolidBitField[BroadIndex] & Mask;
  if (!IsSolid) {
    *pOutPos = NewPos;
    return;
  }

  // Two-unit sampling cuts the loop count roughly in half. The widened mask
  // below covers both possible rounded coordinates after crossing a tile edge.
  const unsigned short Max = (unsigned short)(MaxComponent * 0.5f);
  const uint32_t NegativeBoundaryMask = (1U << 12) | (1U << 13);
  const uint32_t PositiveBoundaryMask = (1U << 18) | (1U << 19);
  const uint32_t BoundaryMaskX = vgetx(Vel) > 0.0f ? PositiveBoundaryMask : NegativeBoundaryMask;
  const uint32_t BoundaryMaskY = vgety(Vel) > 0.0f ? PositiveBoundaryMask : NegativeBoundaryMask;
  mvec2 Step = vfmul(Vel, s_aFractionTable[Max]);

  uivec2 IPos = round_pos_i(Pos);
  uivec2 INewPos;
  for (int i = 0; i <= Max; i++) {
    NewPos = vvadd(Pos, Step);
    INewPos = round_pos_i(NewPos);
    if (test_box_character_mask(pCollision, INewPos.x, INewPos.y, BoundaryMaskX, BoundaryMaskY)) {
      bool Hit = false;
      if (test_box_character_mask(pCollision, IPos.x, INewPos.y, BoundaryMaskX, BoundaryMaskY)) {
        if (vgety(Vel) > 0)
          *pGrounded = true;
        NewPos = vsety(NewPos, vgety(Pos));
        Vel = vsety(Vel, 0);
        Step = vsety(Step, 0);
        Hit = true;
      }
      if (test_box_character_mask(pCollision, INewPos.x, IPos.y, BoundaryMaskX, BoundaryMaskY)) {
        NewPos = vsetx(NewPos, vgetx(Pos));
        Vel = vsetx(Vel, 0);
        Step = vsetx(Step, 0);
        Hit = true;
      }
      if (!Hit) {
        NewPos = Pos;
        Vel = vec2_init(0, 0);
        Step = vec2_init(0, 0);
      }
    }
    IPos = INewPos;
    Pos = NewPos;
  }

  *pOutPos = Pos;
  *pOutVel = Vel;
}

bool get_nearest_air_pos_player(SCollision *__restrict__ pCollision, mvec2 PlayerPos, mvec2 *__restrict__ pOutPos) {
  for (int dist = 5; dist >= -1; dist--) {
    *pOutPos = vec2_init(vgetx(PlayerPos), vgety(PlayerPos) - dist);
    if (!test_box(pCollision, *pOutPos, PHYSICALSIZEVEC))
      return true;
  }
  return false;
}

bool get_nearest_air_pos(SCollision *__restrict__ pCollision, mvec2 Pos, mvec2 PrevPos, mvec2 *__restrict__ pOutPos) {
  for (int k = 0; k < 16 && check_point(pCollision, Pos); k++) {
    Pos = vvsub(Pos, vnormalize(vvsub(PrevPos, Pos)));
  }

  mvec2 PosInBlock = vec2_init((int)(vgetx(Pos) + 0.5f) % 32, (int)(vgety(Pos) + 0.5f) % 32);
  mvec2 BlockCenter = vfadd(vvsub(vec2_init((int)(vgetx(Pos) + 0.5f), (int)(vgety(Pos) + 0.5f)), PosInBlock), 16.0f);

  *pOutPos = vec2_init(vgetx(BlockCenter) + (vgetx(PosInBlock) < 16 ? -2.0f : 1.0f), vgety(Pos));
  if (!test_box(pCollision, *pOutPos, PHYSICALSIZEVEC))
    return true;

  *pOutPos = vec2_init(vgetx(Pos), vgety(BlockCenter) + (vgety(PosInBlock) < 16 ? -2.0f : 1.0f));
  if (!test_box(pCollision, *pOutPos, PHYSICALSIZEVEC))
    return true;

  *pOutPos = vec2_init(vgetx(BlockCenter) + (vgetx(PosInBlock) < 16 ? -2.0f : 1.0f), vgety(BlockCenter) + (vgety(PosInBlock) < 16 ? -2.0f : 1.0f));
  return !test_box(pCollision, *pOutPos, PHYSICALSIZEVEC);
}

int get_index(SCollision *pCollision, mvec2 PrevPos, mvec2 Pos) {
  float Distance = vdistance(PrevPos, Pos);

  if (!Distance) {
    int Nx = (int)vgetx(Pos) >> 5;
    int Ny = (int)vgety(Pos) >> 5;
    return pCollision->m_pWidthLookup[Ny] + Nx;
  }

  // The original loop returned unconditionally on its first iteration, so only
  // i == 0 ever ran: a == 0, which makes the mix exactly PrevPos. ceil() (and
  // its libm call) only decided whether the body ran at all, and Distance is
  // non-zero here, so it always did.
  const int Nx = (int)vgetx(PrevPos) >> 5;
  const int Ny = (int)vgety(PrevPos) >> 5;
  return pCollision->m_pWidthLookup[Ny] + Nx;
}

int entity(SCollision *pCollision, int x, int y, int Layer) {
  if ((unsigned int)x >= pCollision->m_MapData.width || (unsigned int)y >= pCollision->m_MapData.height)
    return 0;

  const int Index = pCollision->m_pWidthLookup[y] + x;
  switch (Layer) {
  case LAYER_GAME:
    return pCollision->m_MapData.game_layer.data[Index] - ENTITY_OFFSET;
  case LAYER_FRONT:
    return pCollision->m_MapData.front_layer.data[Index] - ENTITY_OFFSET;
  case LAYER_SWITCH:
    return pCollision->m_MapData.switch_layer.type[Index] - ENTITY_OFFSET;
  case LAYER_TELE:
    return pCollision->m_MapData.tele_layer.type[Index] - ENTITY_OFFSET;
  case LAYER_SPEEDUP:
    return pCollision->m_MapData.speedup_layer.type[Index] - ENTITY_OFFSET;
  case LAYER_TUNE:
    return pCollision->m_MapData.tune_layer.type[Index] - ENTITY_OFFSET;
  default:
    printf("Error while initializing gameworld: invalid layer found\n");
  }
  return 0;
}
