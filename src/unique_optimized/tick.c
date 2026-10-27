/* The tick of the optimized Unique backend: the core, the tee, its grenades
 * and the pickups. See ../unique_reference for what every piece is a port of;
 * the functions here are named like theirs. Everything that takes a
 * game_mode_t is inlined into the tick of each mode, with the mode a
 * constant. */
#include "internal.h"
#include "../ddnet_optimized/powf.h"

#include <limits.h>

#ifdef __SSE2__
#include <emmintrin.h>
#ifdef __SSE4_1__
#include <smmintrin.h>
#endif
#endif

/* ------------------------------------------------------- velocity ramp */

/* VelocityRamp(): 1 / std::pow(curvature, ...), which is powf(). Most of the
 * time that is glibc's, which is computed here without the call, see
 * ../ddnet_optimized/powf.h. */
ALWAYS_INLINE float velocity_ramp(const tuning_t *tuning, float value) {
  if (value < tuning->velramp_start)
    return 1.0f;
  const float y = (value - tuning->velramp_start) / tuning->velramp_range;
#if DDNET_POWF_REPLICA
  double ylogx;
  if (tuning->velramp_powf != VELRAMP_POWF_LIBRARY && powf_fast(tuning->velramp_log2_curvature, y, &ylogx))
    return 1.0f / (tuning->velramp_powf == VELRAMP_POWF_FMA ? powf_exp2_fma(ylogx) : powf_exp2_sse2(ylogx));
#endif
  return 1.0f / powf(tuning->velramp_curvature, y);
}

/* Which powf() velocity_ramp() can compute itself (the same as
 * velocity_ramp_prepare() of ../ddnet_optimized/character_core.c). */
void velocity_ramp_prepare(tuning_t *tuning) {
  tuning->velramp_powf = VELRAMP_POWF_LIBRARY;
  tuning->velramp_log2_curvature = 0;
#if DDNET_POWF_REPLICA
  uint32_t ix;
  memcpy(&ix, &tuning->velramp_curvature, sizeof(ix));
  /* powf() takes another way for an x that is not positive and normal */
  if (ix - 0x00800000 >= 0x7f800000 - 0x00800000)
    return;
  volatile float probe_x = 0x1.19999ap+0f, probe_y = 0x1.80e36ap+8f;
  const float probe = powf(probe_x, probe_y);
  int which;
  __builtin_cpu_init();
  if (probe == 0x1.e5901ep+52f)
    which = VELRAMP_POWF_SSE2;
  else if (probe == 0x1.e5901cp+52f && __builtin_cpu_supports("fma"))
    which = VELRAMP_POWF_FMA;
  else
    return;
  const double logx = which == VELRAMP_POWF_FMA ? powf_log2_fma(ix) : powf_log2_sse2(ix);
  for (int i = -64; i <= 64; i++) {
    volatile float x = tuning->velramp_curvature, y = (float)i * 0.37f + (float)i * (float)i * 0.0013f;
    double ylogx;
    if (!powf_fast(logx, y, &ylogx))
      continue;
    const float mine = which == VELRAMP_POWF_FMA ? powf_exp2_fma(ylogx) : powf_exp2_sse2(ylogx);
    const float theirs = powf(x, y);
    if (memcmp(&mine, &theirs, sizeof(mine)) != 0)
      return;
  }
  tuning->velramp_powf = which;
  tuning->velramp_log2_curvature = logx;
#endif
}

/* ---------------------------------------------------------------- core */

#ifdef __SSE2__
/* mask ? a : b */
static inline __m128 select_ss(__m128 mask, __m128 a, __m128 b) {
#ifdef __SSE4_1__
  return _mm_blendv_ps(b, a, mask);
#else
  return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
#endif
}

/* What CCharacterCore::Tick does to the horizontal velocity for a direction:
 * SaturatedAdd(-max_speed, max_speed, vel, -accel or accel), or vel * friction
 * without a direction (all three worked out, one picked: the direction is
 * input, which a branch predictor cannot know). */
static inline float move_select(int direction, float vel_x, float max_speed, float accel, float friction) {
  const __m128 vel = _mm_set_ss(vel_x);
  const __m128 max = _mm_set_ss(max_speed), min = _mm_set_ss(-max_speed);
  const float either[2] = {accel, -accel};
  const __m128 modifier = _mm_set_ss(either[direction < 0]);
  const __m128 sum = _mm_add_ss(vel, modifier);
  const __m128 down = select_ss(_mm_cmplt_ss(vel, min), vel, select_ss(_mm_cmplt_ss(sum, min), min, sum));
  const __m128 up = select_ss(_mm_cmpgt_ss(vel, max), vel, select_ss(_mm_cmpgt_ss(sum, max), max, sum));
  const __m128 moved = select_ss(_mm_cmplt_ss(modifier, _mm_setzero_ps()), down, up);
  const __m128 slowed = _mm_mul_ss(vel, _mm_set_ss(friction));
  const __m128 still = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_cvtsi32_si128(direction), _mm_setzero_si128()));
  return _mm_cvtss_f32(select_ss(still, slowed, moved));
}
#endif

/* The jump of CCharacterCore::Tick as a table (see ../ddnet_optimized/character_core.c): bits 0 and 1 the
 * new two bits of jumped, 4 a jump from the ground, 8 one in the air; the index is key down (1), the two
 * bits of jumped (2, 4), on the ground (8), and 0, 16 or 32 for no jumps, one or -1, or more than one. */
static const uint8_t jump_table[48] = {0, 11, 0, 1, 2, 2, 2, 3, 0, 5, 0, 1, 0, 0, 0, 1,
                                       0, 11, 0, 1, 2, 2, 2, 3, 0, 5, 0, 1, 0, 5, 0, 1,
                                       0, 11, 0, 1, 2, 2, 2, 3, 0, 5, 0, 1, 0, 5, 0, 1};

/* CCharacterCore::Tick for a hook that is on its way (no hook teleporters, no other tees). */
__attribute__((noinline)) static void core_hook_fly(world_t *w, core_t *core, const tuning_t *tuning) {
  const collision_t *col = collision(w);
  const vec2 hook_base = core->pos;
  vec2 new_pos = vadd(core->hook_pos, vscale(core->hook_dir, tuning->hook_fire_speed));
  /* "vdistance(hook_base, new_pos) > hook_length" without the root */
  const vec2 from_base = vsub(hook_base, new_pos);
  if (vdot(from_base, from_base) >= tuning->hook_length_squared) {
    core->hook_state = HOOK_RETRACT_START;
    new_pos = vadd(hook_base, vscale(vnormalize(vsub(new_pos, hook_base)), tuning->hook_length));
  }

  /* make sure that the hook doesn't go though the ground */
  const int hit = collision_intersect_line_tele_hook(col, false, core->hook_pos, new_pos, &new_pos, NULL, NULL);

  if (core->hook_state == HOOK_FLYING) {
    /* check against ground */
    if (hit == TILE_NOHOOK) {
      core->triggered_events |= COREEVENT_HOOK_HIT_NOHOOK;
      core->hook_state = HOOK_RETRACT_START;
    } else if (hit) {
      core->triggered_events |= COREEVENT_HOOK_ATTACH_GROUND;
      core->hook_state = HOOK_GRABBED;
    }
    core->hook_pos = new_pos;
  }
}

/* CCharacterCore::Tick and TickDeferred. flags: collision_flags_at() of the position. */
static ALWAYS_INLINE void core_tick(world_t *w, core_t *core, int flags, const tuning_t *tuning) {
  const collision_t *col = collision(w);

  /* (no stopper around: no restrictions; nothing solid around: in the air) */
  if (!(flags & DDNET_TILEFLAG_NEAR_STOPPER))
    core->move_restrictions = 0;
  else
    core->move_restrictions = collision_get_move_restrictions(col, NULL, NULL, core->pos, 18.0f, -1);
  bool grounded = false;
  if (flags & DDNET_TILEFLAG_NEAR_SOLID)
    grounded = collision_is_on_ground(col, core->pos, PHYSICAL_SIZE);
  core->triggered_events = 0;

  core->vel.y += tuning->gravity;

  float max_speed = grounded ? tuning->ground_control_speed : tuning->air_control_speed;
  float accel = grounded ? tuning->ground_control_accel : tuning->air_control_accel;
  float friction = grounded ? tuning->ground_friction : tuning->air_friction;

  /* handle input */
  core->direction = core->input.direction;
  /* (the angle is worked out of the aim when somebody asks, ddnet_character_angle()) */
  core->aim_x = core->input.target_x;
  core->aim_y = core->input.target_y;

  /* handle jump, and the rule for standing on the ground that comes after it */
  {
    const int jumped = core->jumped;
    const int pressed = core->input.jump != 0;
    const int jumps = (core->jumps != 0) + (core->jumps > 1);
    const int e = jump_table[pressed | (jumped & 3) << 1 | (int)grounded << 3 | jumps << 4];
    const int ground = (e >> 2) & 1, air = e >> 3;

    core->triggered_events |= (-ground & COREEVENT_GROUND_JUMP) | (-air & COREEVENT_AIR_JUMP);
    const float vel_y[3] = {core->vel.y, -tuning->air_jump_impulse, -tuning->ground_jump_impulse};
    core->vel.y = vel_y[air + 2 * ground];
    core->jumped = (jumped & ~3) | (e & 3);
    core->jumped_total = (core->jumped_total + air) & -!grounded;
  }

  /* handle hook */
  if (core->input.hook) {
    if (core->hook_state == HOOK_IDLE) {
      core->hook_state = HOOK_FLYING;
      const vec2 target_direction = vnormalize(v2(core->input.target_x, core->input.target_y));
      core->hook_pos = vadd(core->pos, vscale(vscale(target_direction, PHYSICAL_SIZE), 1.5f));
      core->hook_dir = target_direction;
      core->hook_tick = (int)((float)SERVER_TICK_SPEED * (1.25f - tuning->hook_duration));
      core->triggered_events |= COREEVENT_HOOK_LAUNCH;
    }
  } else {
    core->hook_state = HOOK_IDLE;
    core->hook_pos = core->pos;
  }

  /* add the speed modification according to players wanted direction */
#ifdef __SSE2__
  core->vel.x = move_select(core->direction, core->vel.x, max_speed, accel, friction);
#else
  if (core->direction < 0)
    core->vel.x = saturated_add(-max_speed, max_speed, core->vel.x, -accel);
  if (core->direction > 0)
    core->vel.x = saturated_add(-max_speed, max_speed, core->vel.x, accel);
  if (core->direction == 0)
    core->vel.x *= friction;
#endif

  /* do hook */
  if (core->hook_state == HOOK_IDLE) {
    core->hook_pos = core->pos;
  } else if (core->hook_state >= HOOK_RETRACT_START && core->hook_state < HOOK_RETRACT_END) {
    core->hook_state++;
  } else if (core->hook_state == HOOK_RETRACT_END) {
    core->triggered_events |= COREEVENT_HOOK_RETRACT;
    core->hook_state = HOOK_RETRACTED;
  } else if (core->hook_state == HOOK_FLYING) {
    core_hook_fly(w, core, tuning);
  }

  if (core->hook_state == HOOK_GRABBED) {
    if (vdistance(core->hook_pos, core->pos) > 46.0f) {
      vec2 hook_vel = vscale(vnormalize(vsub(core->hook_pos, core->pos)), tuning->hook_drag_accel);
      if (hook_vel.y > 0)
        hook_vel.y *= 0.3f;

      const float factor[2] = {0.75f, 0.95f};
      hook_vel.x *= factor[((hook_vel.x < 0) & (core->direction < 0)) | ((hook_vel.x > 0) & (core->direction > 0))];

      vec2 new_vel = vadd(core->vel, hook_vel);

      /* check if we are under the legal limit for the hook */
      const float new_vel_length = vlength(new_vel);
      if (new_vel_length < tuning->hook_drag_speed || new_vel_length < vlength(core->vel)) {
        core->vel = new_vel;
        core->hook_accel = hook_vel;
      }
    }

    /* release hook (max default hook time is 1.25 s) */
    core->hook_tick++;
  }

  /* TickDeferred: clamp the velocity to something sane ("vlength(vel) > 6000" without the root) */
  if (vdot(core->vel, core->vel) >= tuning->speed_clamp_squared)
    core->vel = vscale(vnormalize(core->vel), 6000);
}

/* CCharacterCore::Move, for a position that is rounded right after (core_quantize()). */
__attribute__((unused)) static ALWAYS_INLINE void core_move(world_t *w, core_t *core, const tuning_t *tuning) {
  const collision_t *col = collision(w);

  /* the velocity ramp: below its start the factor is 1, which changes nothing */
  float ramp_value = 1.0f;
  const bool ramp = vdot(core->vel, core->vel) >= tuning->velramp_start_squared;
  if (ramp) {
    ramp_value = velocity_ramp(tuning, vlength(core->vel) * 50);
    core->vel.x = core->vel.x * ramp_value;
  }

  vec2 new_pos = core->pos;
  const vec2 old_vel = core->vel;
  bool grounded = false;
  collision_move_box_rounded(col, &new_pos, &core->vel, PHYSICAL_SIZE_VEC2,
                             v2(tuning->ground_elasticity_x, tuning->ground_elasticity_y), &grounded);

  if (grounded) {
    core->jumped &= ~2;
    core->jumped_total = 0;
  }

  {
    const int stopped = (core->vel.x < 0.001f) & (core->vel.x > -0.001f);
    const int side = (old_vel.x > 0) + 2 * (old_vel.x < 0);
    core->colliding = -stopped & side;
    core->left_wall = (core->left_wall | !stopped) != 0;
  }

  if (ramp)
    core->vel.x = core->vel.x * (1.0f / ramp_value);

  core->pos = new_pos;
}

/* CCharacterCore::Quantize */
#ifdef __SSE2__
/* round_fast(v * scale) * inverse for four values, with the operations of the plain version below */
static inline __m128 quantize4(__m128 v, __m128 scale, __m128 inverse) {
  v = _mm_mul_ps(v, scale);
  const __m128 half = _mm_or_ps(_mm_and_ps(v, _mm_set1_ps(-0.0f)), _mm_set1_ps(0.5f));
  return _mm_mul_ps(_mm_cvtepi32_ps(_mm_cvttps_epi32(_mm_add_ps(v, half))), inverse);
}
/* Four floats, loaded one by one: they were mostly just written one by one,
 * and a load of all four at once would have to wait for those stores. */
static inline __m128 load4(const float *values) {
  __m128 a = _mm_load_ss(&values[0]), b = _mm_load_ss(&values[1]);
  __m128 c = _mm_load_ss(&values[2]), d = _mm_load_ss(&values[3]);
#if defined(__GNUC__)
  /* (the compiler would make one load of it) */
  __asm__("" : "+x"(a), "+x"(b), "+x"(c), "+x"(d));
#endif
  return _mm_movelh_ps(_mm_unpacklo_ps(a, b), _mm_unpacklo_ps(c, d));
}
#endif
__attribute__((unused)) static ALWAYS_INLINE void core_quantize(core_t *core) {
#ifdef __SSE2__
  /* pos and vel, hook_pos and hook_dir are next to each other; multiplying by
   * 1 changes nothing and multiplying by 1/256 is dividing by 256 */
  _Static_assert(offsetof(core_t, vel) == offsetof(core_t, pos) + 2 * sizeof(float), "layout");
  _Static_assert(offsetof(core_t, hook_dir) == offsetof(core_t, hook_pos) + 2 * sizeof(float), "layout");
  const __m128 scale = _mm_setr_ps(1.0f, 1.0f, 256.0f, 256.0f);
  const __m128 inverse = _mm_setr_ps(1.0f, 1.0f, 1.0f / 256.0f, 1.0f / 256.0f);
  _mm_storeu_ps(&core->pos.x, quantize4(load4(&core->pos.x), scale, inverse));
  _mm_storeu_ps(&core->hook_pos.x, quantize4(load4(&core->hook_pos.x), scale, inverse));
  return;
#endif
  core->pos.x = round_fast(core->pos.x);
  core->pos.y = round_fast(core->pos.y);
  core->vel.x = round_fast(core->vel.x * 256.0f) / 256.0f;
  core->vel.y = round_fast(core->vel.y * 256.0f) / 256.0f;
  core->hook_pos.x = round_fast(core->hook_pos.x);
  core->hook_pos.y = round_fast(core->hook_pos.y);
  core->hook_dir.x = round_fast(core->hook_dir.x * 256.0f) / 256.0f;
  core->hook_dir.y = round_fast(core->hook_dir.y * 256.0f) / 256.0f;
}

#ifdef __SSE4_1__
/* core_move() and core_quantize() in one piece, from ../ddnet_optimized/character.c
 * (character_move_alone()): the move at once if it rounds like the steps of
 * CCollision::MoveBox do and nothing solid is on the way, and the rounding of
 * the position and the velocity together. Every float operation is the one
 * of those functions. */
static ALWAYS_INLINE void core_move_quantize(world_t *w, core_t *core, const tuning_t *tuning) {
  const collision_t *col = collision(w);

  float ramp_value = 1.0f;
  const bool ramp = vdot(core->vel, core->vel) >= tuning->velramp_start_squared;
  if (ramp) {
    ramp_value = velocity_ramp(tuning, vlength(core->vel) * 50);
    core->vel.x = core->vel.x * ramp_value;
  }
  const float old_vel_x = core->vel.x;

  const __m128 sign = _mm_set1_ps(-0.0f);
  const __m128 p = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&core->pos));
  const __m128 v = _mm_unpacklo_ps(_mm_load_ss(&core->vel.x), _mm_load_ss(&core->vel.y));
  __m128 end = p;
  bool at_once = false, grounded = false;
  {
    const float travel = fabsf(core->vel.x) + fabsf(core->vel.y);
    if (travel < 4096.0f && (_mm_movemask_ps(p) & 3) == 0) {
      const float largest = (core->pos.x + core->pos.y) + (travel + 1.0f);
      const float error = largest * (((travel + 2.0f) + 4.0f) * 1.2e-7f) + 2e-5f;
      const __m128 direct = _mm_add_ps(p, v);
      const __m128 size = _mm_andnot_ps(sign, direct);
      const __m128 fraction = _mm_sub_ps(size, _mm_round_ps(size, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));
      const __m128 distance = _mm_andnot_ps(sign, _mm_sub_ps(fraction, _mm_set1_ps(0.5f)));
      if (largest < 1e6f && error < 0.2f && (_mm_movemask_ps(_mm_cmpgt_ps(distance, _mm_set1_ps(error))) & 3) == 3) {
        /* the tiles a corner of the tee can get on, see move_box() */
        const __m128 out = _mm_set1_ps(PHYSICAL_SIZE * 0.5f + 0.75f);
        const __m128 low = _mm_sub_ps(_mm_min_ps(p, direct), out);
        const __m128 high = _mm_add_ps(_mm_max_ps(p, direct), out);
        const __m128 inner = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&col->inner_width));
        if ((_mm_movemask_ps(_mm_cmpge_ps(low, _mm_set1_ps(32.0f))) & 3) == 3 &&
            (_mm_movemask_ps(_mm_cmplt_ps(high, inner)) & 3) == 3) {
          const __m128i low_tile = _mm_srai_epi32(_mm_cvttps_epi32(low), 5);
          const __m128i high_tile = _mm_srai_epi32(_mm_cvttps_epi32(high), 5);
          if (rect_count(col, col->sat_solid, _mm_cvtsi128_si32(low_tile), _mm_extract_epi32(low_tile, 1),
                         _mm_cvtsi128_si32(high_tile) + 1, _mm_extract_epi32(high_tile, 1) + 1) == 0) {
            end = direct;
            at_once = true;
          }
        }
      }
    }
  }
  if (!at_once) {
    vec2 new_pos = core->pos;
    move_box_steps(col, &new_pos, &core->vel, PHYSICAL_SIZE_VEC2,
                   v2(tuning->ground_elasticity_x, tuning->ground_elasticity_y), &grounded, true);
    end = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&new_pos));
    if (grounded) {
      core->jumped &= ~2;
      core->jumped_total = 0;
    }
  }
  {
    const int stopped = (core->vel.x < 0.001f) & (core->vel.x > -0.001f);
    const int side = (old_vel_x > 0) + 2 * (old_vel_x < 0);
    core->colliding = -stopped & side;
    core->left_wall = (core->left_wall | !stopped) != 0;
  }
  if (ramp)
    core->vel.x = core->vel.x * (1.0f / ramp_value);

  /* CCharacterCore::Quantize: the position and the velocity, and the hook */
  const __m128 scale = _mm_setr_ps(1.0f, 1.0f, 256.0f, 256.0f);
  const __m128 inverse = _mm_setr_ps(1.0f, 1.0f, 1.0f / 256.0f, 1.0f / 256.0f);
  const __m128 moved =
      _mm_mul_ps(_mm_movelh_ps(end, _mm_unpacklo_ps(_mm_load_ss(&core->vel.x), _mm_load_ss(&core->vel.y))), scale);
  const __m128i whole = _mm_cvttps_epi32(_mm_add_ps(moved, _mm_or_ps(_mm_and_ps(moved, sign), _mm_set1_ps(0.5f))));
  _mm_storeu_ps(&core->pos.x, _mm_mul_ps(_mm_cvtepi32_ps(whole), inverse));
  _mm_storeu_ps(&core->hook_pos.x, quantize4(load4(&core->hook_pos.x), scale, inverse));
}
#endif

/* ---------------------------------------------------------- the race */

static void character_die(world_t *w, character_t *chr) {
  player_t *player = &w->player;
  /* a nice sound, and bursting tee death effect */
  EMIT_SOUND(w, chr->pos, DDNET_SOUND_PLAYER_DIE);
  EMIT_PARTICLE(w, chr->pos, DDNET_PARTICLE_PLAYER_DEATH);
  player->previous_die_tick = player->die_tick;
  player->die_tick = server_tick(w);
  chr->alive = false;
}

/* CGameTeams::OnCharacterStart for a tee alone in its team */
static void race_start(world_t *w, character_t *chr, float fraction_of_tick) {
  if (chr->race_state == DDNET_RACE_STARTED || chr->race_state == DDNET_RACE_FINISHED)
    return;
  chr->race_state = DDNET_RACE_STARTED;
  chr->start_time = server_tick(w);
  chr->start_time_offset = fraction_of_tick;
}

/* CGameTeams::OnCharacterFinish and OnFinish of Unique Race */
static void race_finish(world_t *w, character_t *chr, float fraction_of_tick) {
  int time_ticks = server_tick(w) - chr->start_time;
  if (time_ticks < 0)
    return;
  w->player.finish_tick = server_tick(w);
  w->player.finish_time = (fraction_of_tick - chr->start_time_offset + time_ticks) / (float)SERVER_TICK_SPEED;
  chr->race_state = DDNET_RACE_FINISHED;
  EMIT_PARTICLE(w, chr->pos, DDNET_PARTICLE_CONFETTI);
}

/* ------------------------------------------------------------- damage */

static ALWAYS_INLINE bool character_increase_health(character_t *chr, int amount) {
  if (chr->health >= 10)
    return false;
  chr->health = clampi(chr->health + amount, 0, 10);
  return true;
}

static ALWAYS_INLINE bool character_increase_armor(character_t *chr, int amount) {
  if (chr->armor >= 10)
    return false;
  chr->armor = clampi(chr->armor + amount, 0, 10);
  return true;
}

/* CCharacter::TakeDamage of Unique Race */
static ALWAYS_INLINE void character_take_damage(world_t *w, character_t *chr, vec2 force, int dmg, game_mode_t m) {
  chr->core.vel = vadd(chr->core.vel, force);

  if (m.health) {
    /* m_pPlayer only inflicts half damage on self */
    dmg = maxi(1, dmg / 2);
    if (EVENT_BUILD) {
      /* create healthmod indicator, its stars apart from those of the hits just before */
      chr->damage_taken++;
      if (server_tick(w) < chr->damage_taken_tick + 25) {
        EMIT_DAMAGE_IND(w, chr->pos, chr->damage_taken * 0.25f, dmg);
      } else {
        chr->damage_taken = 0;
        EMIT_DAMAGE_IND(w, chr->pos, 0, dmg);
      }
      chr->damage_taken_tick = server_tick(w);
    }
    if (dmg) {
      if (chr->armor) {
        if (dmg > 1) {
          chr->health--;
          dmg--;
        }
        if (dmg > chr->armor) {
          dmg -= chr->armor;
          chr->armor = 0;
        } else {
          chr->armor -= dmg;
          dmg = 0;
        }
      }
      chr->health -= dmg;
    }
    if (chr->health <= 0)
      character_die(w, chr);
    else
      EMIT_SOUND(w, chr->pos, dmg > 2 ? DDNET_SOUND_PLAYER_PAIN_LONG : DDNET_SOUND_PLAYER_PAIN_SHORT);
  }
}

/* CGameContext::CreateExplosion of a grenade of the tee */
static ALWAYS_INLINE void create_explosion(world_t *w, vec2 pos, game_mode_t m) {
  EMIT_PARTICLE(w, pos, DDNET_PARTICLE_EXPLOSION);
  character_t *chr = get_player_char(w);
  const float radius = 135.0f;
  const float inner_radius = 48.0f;
  if (!chr || !(vdistance(chr->pos, pos) < radius + PHYSICAL_SIZE))
    return;
  vec2 diff = vsub(chr->pos, pos);
  vec2 force_dir = v2(0, 1);
  float l = vlength(diff);
  if (l)
    force_dir = vnormalize(diff);
  l = 1 - clampf((l - inner_radius) / (radius - inner_radius), 0.0f, 1.0f);
  const float strength = w->tuning_values[w->player.tune_zone].explosion_strength;

  float dmg = strength * l;
  if (!(int)dmg)
    return;

  chr->hit_num += (int)dmg;
  character_take_damage(w, chr, vscale(vscale(force_dir, dmg), 2), (int)dmg, m);
}

/* ----------------------------------------------------------- grenades */

/* CProjectile::CProjectile */
static void projectile_create(world_t *w, vec2 pos, vec2 dir, int span) {
  if (w->num_projectiles == w->projectile_capacity) {
    const int capacity = w->projectile_capacity ? w->projectile_capacity * 2 : 8;
    projectile_t *projectiles = realloc(w->projectiles, (size_t)capacity * sizeof(*projectiles));
    if (!projectiles)
      return;
    w->projectiles = projectiles;
    w->projectile_capacity = capacity;
  }
  /* new entities go to the front of their list */
  memmove(&w->projectiles[1], &w->projectiles[0], (size_t)w->num_projectiles * sizeof(*w->projectiles));
  w->num_projectiles++;
  projectile_t *proj = &w->projectiles[0];
  proj->pos = pos;
  proj->u.projectile.direction = dir;
  proj->u.projectile.life_span = span;
  proj->u.projectile.start_tick = server_tick(w);
  proj->u.projectile.tune_zone = collision_is_tune(collision(w), collision_get_map_index(collision(w), pos));
}

/* CalcPos with the tuning of a grenade */
static ALWAYS_INLINE vec2 projectile_pos(const world_t *w, const projectile_t *proj, float time) {
  const tuning_t *tuning = &w->tuning_values[proj->u.projectile.tune_zone];
  const float curvature = tuning->grenade_curvature, speed = tuning->grenade_speed;
  vec2 n;
  time *= speed;
  n.x = proj->pos.x + proj->u.projectile.direction.x * time;
  n.y = proj->pos.y + proj->u.projectile.direction.y * time + curvature / 10000 * (time * time);
  return n;
}

ddnet_vec2_t ddnet_projectile_get_pos(const ddnet_world_t *world, const ddnet_entity_t *projectile, float time) {
  return projectile_pos(world, projectile, time);
}

/* CProjectile::Tick of every grenade, in list order, and CGameWorld::RemoveEntities for them */
static ALWAYS_INLINE void projectiles_tick(world_t *w, game_mode_t m) {
  const collision_t *col = collision(w);
  int kept = 0;
  for (int i = 0; i < w->num_projectiles; i++) {
    projectile_t *proj = &w->projectiles[i];
    ddnet_projectile_t *p = &proj->u.projectile;

    const float pt = (server_tick(w) - p->start_tick - 1) / (float)SERVER_TICK_SPEED;
    const float ct = (server_tick(w) - p->start_tick) / (float)SERVER_TICK_SPEED;
    const vec2 prev_pos = projectile_pos(w, proj, pt);
    const vec2 cur_pos = projectile_pos(w, proj, ct);
    /* the trail a client draws behind a grenade */
    EMIT_PARTICLE(w, prev_pos, DDNET_PARTICLE_SMOKE);
    vec2 col_pos, new_pos;
    const int collide = collision_intersect_line(col, prev_pos, cur_pos, &col_pos, &new_pos);

    if (p->life_span > -1)
      p->life_span--;

    bool destroy = false;
    if (!get_player_char(w) && w->config.sv_kill_grenades) {
      destroy = true;
    } else if (collide || game_layer_clipped(w, cur_pos) || p->life_span == -1) {
      create_explosion(w, col_pos, m);
      EMIT_SOUND(w, col_pos, DDNET_SOUND_GRENADE_EXPLODE);
      destroy = true;
    }
    /* (no weapon teleporters, see ddnet_map_unsupported()) */
    if (!destroy) {
      if (kept != i)
        w->projectiles[kept] = *proj;
      kept++;
    }
  }
  w->num_projectiles = kept;
}

/* ------------------------------------------------------------ pickups */

/* CPickup::Tick for a pickup that the tee touches (the part after "Check if a player intersected us") */
static ALWAYS_INLINE void pickup_touch(world_t *w, character_t *chr, int i, game_mode_t m) {
  const ddnet_pickup_t *pickup = &w->pickups[i];
  if (m.health && w->pickup_spawn_ticks[i] != -1)
    return;

  bool respawn = false;
  switch (pickup->type) {
  case DDNET_UNIQUE_POWERUP_HEALTH:
    if (m.health) {
      respawn = character_increase_health(chr, 1);
      if (respawn)
        EMIT_SOUND(w, pickup->pos, DDNET_SOUND_PICKUP_HEALTH);
    } else if (chr->freeze_time <= SV_FREEZE_DELAY * SERVER_TICK_SPEED &&
               (chr->freeze_time == 0 || chr->core.freeze_start < server_tick(w) - SERVER_TICK_SPEED)) {
      /* CCharacter::Freeze */
      chr->armor = 0;
      chr->freeze_time = SV_FREEZE_DELAY * SERVER_TICK_SPEED;
      chr->core.freeze_start = server_tick(w);
      EMIT_SOUND(w, pickup->pos, DDNET_SOUND_PICKUP_HEALTH);
    }
    break;

  case DDNET_UNIQUE_POWERUP_ARMOR:
    if (m.health) {
      respawn = character_increase_armor(chr, 1);
      if (respawn)
        EMIT_SOUND(w, pickup->pos, DDNET_SOUND_PICKUP_ARMOR);
    } else {
      bool sound = false;
      for (int j = WEAPON_SHOTGUN; j < NUM_WEAPONS; j++) {
        if (chr->core.weapons[j].got) {
          chr->core.weapons[j].got = false;
          chr->core.weapons[j].ammo = 0;
          sound = true;
        }
      }
      if (sound) {
        chr->last_weapon = WEAPON_GUN;
        EMIT_SOUND(w, pickup->pos, DDNET_SOUND_PICKUP_ARMOR);
      }
      if (chr->core.active_weapon >= WEAPON_SHOTGUN)
        chr->core.active_weapon = WEAPON_HAMMER;
    }
    break;

  case DDNET_UNIQUE_POWERUP_WEAPON: {
    ddnet_weapon_stat_t *weapon = &chr->core.weapons[WEAPON_GRENADE];
    if ((!weapon->got || weapon->ammo != -1) && (!m.health || weapon->ammo != GRENADE_MAX_AMMO)) {
      /* CCharacter::GiveWeapon */
      weapon->ammo = m.health ? GRENADE_MAX_AMMO : -1;
      weapon->got = true;
      respawn = m.health;
      EMIT_SOUND(w, pickup->pos, DDNET_SOUND_PICKUP_GRENADE);
    }
  } break;
  }

  if (m.health && respawn) {
    const int spawn_tick = server_tick(w) + SERVER_TICK_SPEED * PICKUP_RESPAWN_TIME;
    w->pickup_spawn_ticks[i] = spawn_tick;
    w->num_respawning++;
    w->next_respawn = mini(w->next_respawn, spawn_tick);
  }
}

/* CPickup::Tick of every pickup, in list order */
static ALWAYS_INLINE void pickups_tick(world_t *w, game_mode_t m, int here_flags) {
  const collision_t *col = collision(w);
  character_t *chr = get_player_char(w);

  /* wait for: a pickup is there again once the tick is past its spawn tick,
   * and all of them are when there is no tee */
  if (m.health && w->num_respawning && (!chr || server_tick(w) > w->next_respawn)) {
    int next = INT_MAX;
    for (int i = 0; i < w->num_pickups; i++) {
      int *spawn_tick = &w->pickup_spawn_ticks[i];
      if (*spawn_tick == -1)
        continue;
      if (!chr || (*spawn_tick > 0 && server_tick(w) > *spawn_tick)) {
        if (EVENT_BUILD && chr && w->pickups[i].type == DDNET_UNIQUE_POWERUP_WEAPON)
          EMIT_SOUND(w, w->pickups[i].pos, DDNET_SOUND_WEAPON_SPAWN);
        *spawn_tick = -1;
        w->num_respawning--;
      } else {
        next = mini(next, *spawn_tick);
      }
    }
    w->next_respawn = next;
  }

  /* Check if a player intersected us: the pickups of the tiles around the tee
   * (none if there are none near), newest first like the list, and they were
   * made row by row */
  if (!chr || !(here_flags & DDNET_TILEFLAG_NEAR_PICKUP))
    return;
  const float radius = PICKUP_PROXIMITY_RADIUS + 6;
  const int tx = clampi((int)chr->pos.x >> 5, 0, col->width - 1);
  const int ty = clampi((int)chr->pos.y >> 5, 0, col->height - 1);
  for (int y = mini(ty + 2, col->height - 1); y >= maxi(ty - 2, 0); y--) {
    for (int x = mini(tx + 2, col->width - 1); x >= maxi(tx - 2, 0); x--) {
      const int tile = y * col->width + x;
      const int first = col->tile_pickups[tile];
      if (first < 0)
        continue;
      int last = first;
      while (last + 1 < col->num_pickups && col->pickups[last + 1].tile == tile)
        last++;
      for (int k = last; k >= first; k--) {
        const int i = w->shared->pickup_of_map[k];
        if (i < 0 || !(vdistance(chr->pos, w->pickups[i].pos) < radius + PHYSICAL_SIZE))
          continue;
        pickup_touch(w, chr, i, m);
      }
    }
  }
}

/* ------------------------------------------------------------ weapons */

/* a ? b : c for a 0 or 1, without a branch */
static inline int select_int(int yes, int a, int b) { return b ^ ((a ^ b) & -yes); }

/* CCharacter::DoWeaponSwitch (and SetWeapon). Which weapon is wanted comes
 * from the input, which a branch predictor cannot know, so what the ifs of the
 * server do is done with numbers (as in ../ddnet_optimized/character.c):
 *   if (reload_timer != 0 || queued_weapon == -1) return;
 *   if (weapons[WEAPON_NINJA].got || !weapons[queued_weapon].got) return;
 *   SetWeapon(queued_weapon), which does nothing for the active weapon */
static ALWAYS_INLINE void character_do_weapon_switch(world_t *w, character_t *chr) {
  const int queued = chr->queued_weapon, active = chr->core.active_weapon;
  const int can = (chr->reload_timer == 0) & (queued != -1);
  const int index = queued & -can;
  const int change =
      can & !chr->core.weapons[WEAPON_NINJA].got & (chr->core.weapons[index].got != 0) & (queued != active);
  chr->last_weapon = select_int(change, active, chr->last_weapon);
  chr->queued_weapon = select_int(change, -1, queued);
  chr->core.active_weapon = select_int(change, queued & -((unsigned)queued < NUM_WEAPONS), active);
  if (EVENT_BUILD && change)
    EMIT_SOUND(w, chr->pos, DDNET_SOUND_WEAPON_SWITCH);
}

/* CCharacter::HandleWeaponSwitch, with the direct input before the latest one */
static ALWAYS_INLINE void character_handle_weapon_switch(world_t *w, character_t *chr, const input_t *prev_input) {
  int wanted_weapon = select_int(chr->queued_weapon != -1, chr->queued_weapon, chr->core.active_weapon);

  /* (the tee always has the hammer and the gun, so it has anything) */
  /* select Weapon (counters that did not change count no presses; next_weapon and prev_weapon are next to
   * each other: both at once) */
  uint64_t counters, prev_counters;
  memcpy(&counters, &chr->input.next_weapon, sizeof(counters));
  memcpy(&prev_counters, &prev_input->next_weapon, sizeof(prev_counters));
  if (counters != prev_counters) {
    int next = count_input(prev_input->next_weapon, chr->input.next_weapon).presses;
    int prev = count_input(prev_input->prev_weapon, chr->input.prev_weapon).presses;

    if (next < 128) {
      while (next) {
        wanted_weapon = (wanted_weapon + 1) % NUM_WEAPONS;
        if (chr->core.weapons[wanted_weapon].got)
          next--;
      }
    }

    if (prev < 128) {
      while (prev) {
        wanted_weapon = (wanted_weapon - 1) < 0 ? NUM_WEAPONS - 1 : wanted_weapon - 1;
        if (chr->core.weapons[wanted_weapon].got)
          prev--;
      }
    }
  }

  /* Direct Weapon selection (the value really is taken from the other input) */
  wanted_weapon = select_int(chr->input.wanted_weapon != 0, prev_input->wanted_weapon - 1, wanted_weapon);

  /* check for insane values */
  {
    const int sane = (unsigned)wanted_weapon < NUM_WEAPONS;
    const int take = sane & (wanted_weapon != chr->core.active_weapon) &
                     (chr->core.weapons[wanted_weapon & -sane].got != 0);
    chr->queued_weapon = select_int(take, wanted_weapon, chr->queued_weapon);
  }

  character_do_weapon_switch(w, chr);
}

/* CCharacter::FireWeapon after its DoWeaponSwitch(), with the fire counter of
 * the input before (the input itself on the ticks, where nothing is pressed). */
static ALWAYS_INLINE void character_fire_weapon_switched(world_t *w, character_t *chr, int prev_fire,
                                                         game_mode_t m) {
  const int fire = chr->input.fire;
  /* The key is up and was not pressed since the last input: nothing below
   * fires (no press to count, and a weapon that fires while the key is held
   * needs it down). */
  if (!(fire & 1) && fire == prev_fire)
    return;

  /* (only the grenade is full auto of what the tee can have) */
  const int active = chr->core.active_weapon;
  bool will_fire = count_input(prev_fire, fire).presses != 0;
  if (active == WEAPON_GRENADE && (fire & 1) && chr->core.weapons[active].ammo)
    will_fire = true;
  if (!will_fire)
    return;
  if (chr->freeze_time) {
    /* Timer stuff to avoid shrieking orchestra caused by unfreeze-plasma (the server counts its timer down
     * in CCharacter::HandleWeapons, after which this comes on the same tick, or before it on the next one:
     * the sound comes again on the tick 50 after it at the earliest) */
    if (EVENT_BUILD && server_tick(w) >= chr->pain_sound_tick && !(prev_fire & 1)) {
      chr->pain_sound_tick = server_tick(w) + 1 * SERVER_TICK_SPEED;
      EMIT_SOUND(w, chr->pos, DDNET_SOUND_PLAYER_PAIN_LONG);
    }
    return;
  }
  if (!chr->core.weapons[active].ammo)
    return;

  if (EVENT_BUILD && active <= WEAPON_GUN)
    EMIT_SOUND(w, chr->pos, active == WEAPON_HAMMER ? DDNET_SOUND_HAMMER_FIRE : DDNET_SOUND_GUN_FIRE);
  if (m.weapons && active == WEAPON_GRENADE) {
    const vec2 direction = vnormalize(v2(chr->input.target_x, chr->input.target_y));
    const vec2 proj_start_pos = vadd(chr->pos, vscale(vscale(direction, PHYSICAL_SIZE), 0.75f));
    const int lifetime = (int)(SERVER_TICK_SPEED * w->tuning_values[chr->tune_zone].grenade_lifetime);
    projectile_create(w, proj_start_pos, direction, lifetime);
    EMIT_SOUND(w, chr->pos, DDNET_SOUND_GRENADE_FIRE);
  }
  /* (the hammer and the gun hit nothing the tee could feel) */

  chr->attack_tick = server_tick(w);

  if (m.health && chr->core.weapons[active].ammo > 0)
    chr->core.weapons[active].ammo--;

  if (!chr->reload_timer) {
    const float fire_delay = (&w->tuning_values[chr->tune_zone].hammer_fire_delay)[active];
    chr->reload_timer = (int)(fire_delay * SERVER_TICK_SPEED / 1000);
  }
}

/* CCharacter::OnDirectInput, and CCharacter::OnPredictedInput with it: the
 * tee takes both for the same input, or neither (the same flags decide in
 * CPlayer::OnPredictedEarlyInput and OnPredictedInput), so the server's
 * m_LatestInput, m_LatestPrevInput and m_SavedInput are one input here,
 * chr->input; in this function the one before is m_LatestPrevInput, and
 * m_Input for the wanted weapon. */
static ALWAYS_INLINE void character_on_direct_input(world_t *w, character_t *chr, const input_t *new_input,
                                                    game_mode_t m) {
  const input_t prev_input = chr->input;
  chr->input = *new_input;
  if (chr->num_inputs < 2)
    chr->num_inputs++;

  /* it is not allowed to aim in the center */
  if (chr->input.target_x == 0 && chr->input.target_y == 0)
    chr->input.target_y = -1;

  if (chr->num_inputs > 1) {
    character_handle_weapon_switch(w, chr, &prev_input);
    /* CCharacter::FireWeapon, whose DoWeaponSwitch() changes nothing right after the one HandleWeaponSwitch()
     * ends with */
    if (chr->reload_timer == 0)
      character_fire_weapon_switched(w, chr, prev_input.fire, m);
  }
}

/* -------------------------------------------------------------- tiles */

/* True if one of the four sensor points of the tee touches a death tile. */
static bool character_touches_death_tile(const collision_t *col, vec2 pos) {
  return collision_get_collision_at(col, pos.x + PHYSICAL_SIZE / 3.f, pos.y - PHYSICAL_SIZE / 3.f) == TILE_DEATH ||
         collision_get_collision_at(col, pos.x + PHYSICAL_SIZE / 3.f, pos.y + PHYSICAL_SIZE / 3.f) == TILE_DEATH ||
         collision_get_collision_at(col, pos.x - PHYSICAL_SIZE / 3.f, pos.y - PHYSICAL_SIZE / 3.f) == TILE_DEATH ||
         collision_get_collision_at(col, pos.x - PHYSICAL_SIZE / 3.f, pos.y + PHYSICAL_SIZE / 3.f) == TILE_DEATH ||
         collision_get_front_collision_at(col, pos.x + PHYSICAL_SIZE / 3.f, pos.y - PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_front_collision_at(col, pos.x + PHYSICAL_SIZE / 3.f, pos.y + PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_front_collision_at(col, pos.x - PHYSICAL_SIZE / 3.f, pos.y - PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_front_collision_at(col, pos.x - PHYSICAL_SIZE / 3.f, pos.y + PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH;
}

/* The speedups of CCharacter::HandleSkippableTiles (the old ones: the maps have no others) */
__attribute__((noinline)) static void character_speedup(world_t *w, character_t *chr, int index) {
  const collision_t *col = collision(w);
  vec2 direction = v2(0, 0), temp_vel = chr->core.vel;
  int force = 0, type = 0, max_speed = 0;
  collision_get_speedup(col, index, &direction, &force, &max_speed, &type);
  if (type != TILE_SPEED_BOOST_OLD)
    return;

  float tee_angle, speeder_angle, diff_angle, speed_left, tee_speed;
  if (force == 255 && max_speed) {
    chr->core.vel = vscale(direction, (max_speed / 5));
  } else {
    if (max_speed > 0 && max_speed < 5)
      max_speed = 5;
    if (max_speed > 0) {
      if (direction.x > 0.0000001f)
        speeder_angle = -atanf(direction.y / direction.x);
      else if (direction.x < 0.0000001f)
        speeder_angle = atanf(direction.y / direction.x) + 2.0f * asinf(1.0f);
      else if (direction.y > 0.0000001f)
        speeder_angle = asinf(1.0f);
      else
        speeder_angle = asinf(-1.0f);

      if (speeder_angle < 0)
        speeder_angle = 4.0f * asinf(1.0f) + speeder_angle;

      if (temp_vel.x > 0.0000001f)
        tee_angle = -atanf(temp_vel.y / temp_vel.x);
      else if (temp_vel.x < 0.0000001f)
        tee_angle = atanf(temp_vel.y / temp_vel.x) + 2.0f * asinf(1.0f);
      else if (temp_vel.y > 0.0000001f)
        tee_angle = asinf(1.0f);
      else
        tee_angle = asinf(-1.0f);

      if (tee_angle < 0)
        tee_angle = 4.0f * asinf(1.0f) + tee_angle;

      tee_speed = sqrt(pow(temp_vel.x, 2) + pow(temp_vel.y, 2));

      diff_angle = speeder_angle - tee_angle;
      speed_left = max_speed / 5.0f - cosf(diff_angle) * tee_speed;
      if (absolutei((int)speed_left) > force && speed_left > 0.0000001f)
        temp_vel = vadd(temp_vel, vscale(direction, force));
      else if (absolutei((int)speed_left) > force)
        temp_vel = vadd(temp_vel, vscale(direction, -force));
      else
        temp_vel = vadd(temp_vel, vscale(direction, speed_left));
    } else {
      temp_vel = vadd(temp_vel, vscale(direction, force));
    }

    chr->core.vel = clamp_vel(chr->move_restrictions, temp_vel);
  }
}

/* The teleporters of CCharacter::HandleTiles, with the time Unique Race adds
 * for them. True if the tee was teleported. */
__attribute__((noinline)) static bool character_teleport(world_t *w, character_t *chr, int map_index,
                                                         float fraction_of_tick) {
  const collision_t *col = collision(w);
  core_t *core = &chr->core;
  int tele_out = -1;
  const ddnet_tele_list_t *outs = NULL;
  bool stop = false, start = false;

  int z = collision_is_teleport(col, map_index);
  const int evil_teleport = collision_is_evil_teleport(col, map_index);
  if (z && col->tele_outs[z - 1].count != 0) {
    outs = &col->tele_outs[z - 1];
  } else if (evil_teleport && col->tele_outs[evil_teleport - 1].count != 0) {
    outs = &col->tele_outs[evil_teleport - 1];
    stop = true;
  } else {
    const bool evil = collision_is_check_evil_teleport(col, map_index);
    if (!evil && !collision_is_check_teleport(col, map_index))
      return false;
    for (int k = chr->tele_checkpoint - 1; k >= 0 && !outs; k--)
      if (col->tele_check_outs[k].count != 0)
        outs = &col->tele_check_outs[k];
    stop = evil;
    start = !outs;
  }

  if (start) {
    /* to the spawn, without the time */
    vec2 spawn_pos;
    if (controller_can_spawn(w, &spawn_pos)) {
      core->pos = spawn_pos;
      if (stop)
        core->vel = v2(0, 0);
      core->hook_state = HOOK_RETRACTED;
      core->triggered_events |= COREEVENT_HOOK_RETRACT;
      core->hook_pos = core->pos;
    }
    return true;
  }

  tele_out = world_pick_tele_out(w, outs->count);
  core->pos = outs->positions[tele_out];
  if (stop)
    core->vel = v2(0, 0);
  chr->start_time -= 1;
  chr->start_time_offset -= fraction_of_tick;
  /* CCharacter::ResetHook */
  core->hook_state = HOOK_RETRACTED;
  core->triggered_events |= COREEVENT_HOOK_RETRACT;
  core->hook_pos = core->pos;
  return true;
}

/* CCharacter::HandleTiles */
static ALWAYS_INLINE void character_handle_tiles(world_t *w, character_t *chr, int index, float fraction_of_tick,
                                                 int pos_flags) {
  const collision_t *col = collision(w);
  core_t *core = &chr->core;

  chr->time = (fraction_of_tick - chr->start_time_offset + (float)(server_tick(w)) - chr->start_time) /
              ((float)SERVER_TICK_SPEED);

  /* (the four tiles around the position and the one of the index: with no stopper there, no restrictions) */
  if ((pos_flags | (index >= 0 ? col->tile_flags[index] : 0)) & DDNET_TILEFLAG_NEAR_STOPPER)
    chr->move_restrictions = collision_get_move_restrictions(col, NULL, NULL, chr->pos, 18.0f, index);
  else
    chr->move_restrictions = 0;
  if (index < 0)
    return;

  /* (what the tile has to be looked at for, see DDNET_TILEHANDLER_*) */
  const int handlers = col->tile_handlers[index];
  if (handlers & DDNET_TILEHANDLER_CHECKPOINT) {
    const int tele_checkpoint = collision_is_tele_checkpoint(col, index);
    if (tele_checkpoint)
      chr->tele_checkpoint = tele_checkpoint;
  }

  /* CGameControllerUnique::HandleCharacterTiles */
  if (handlers & DDNET_TILEHANDLER_RACE) {
    const int tile = col->game[index].index;
    const int ftile = col->front ? col->front[index].index : 0;
    const ddnet_race_state_t race_state = chr->race_state;
    if ((tile == TILE_START || ftile == TILE_START) && race_state != DDNET_RACE_CHEATED &&
        chr->race_state != DDNET_RACE_STARTED)
      race_start(w, chr, fraction_of_tick);
    if ((tile == TILE_FINISH || ftile == TILE_FINISH) && race_state == DDNET_RACE_STARTED)
      race_finish(w, chr, fraction_of_tick);
  }

  /* stopper */
  if (chr->move_restrictions) {
    if (core->vel.y > 0 && (chr->move_restrictions & DDNET_CANTMOVE_DOWN)) {
      core->jumped = 0;
      core->jumped_total = 0;
    }
    core->vel = clamp_vel(chr->move_restrictions, core->vel);
  }

  /* teleporters */
  if (handlers & DDNET_TILEHANDLER_TELE)
    character_teleport(w, chr, index, fraction_of_tick);
}

/* CCollision::GetMapIndicesWithTime with what the optimized collision knows
 * about where nothing exists (see collision_map_indices_next()). */
static ALWAYS_INLINE bool map_indices_with_time_next(map_indices_t *it, int *index, float *fraction) {
  const collision_t *col = it->col;
  if (!it->d) {
    if (it->i != 0)
      return false;
    it->i = 1;
    int nx = clampi((int)it->pos.x >> 5, 0, col->width - 1);
    int ny = clampi((int)it->pos.y >> 5, 0, col->height - 1);
    *index = ny * col->width + nx;
    *fraction = 0.0f;
    return collision_tile_exists(col, *index);
  }

  while (it->i < it->end) {
    const int i = it->i;
    const int tile = map_indices_tile(it, i);
    if (!(col->tile_flags[tile] & DDNET_TILEFLAG_EXISTS)) {
      const int dist = col->dist_exists[tile];
      it->i = dist >= 2 ? i + 32 * (dist - 1) : map_indices_leave_tile(it, i, tile);
      continue;
    }
    it->i = map_indices_leave_tile(it, i, tile);
    if (it->last_index != tile) {
      it->last_index = tile;
      *index = tile;
      *fraction = (float)(i) / it->d;
      return true;
    }
  }
  return false;
}

/* For a tee on a tile that does nothing (on the map): whether no tile that
 * exists is on its way from prev_pos to pos, for the short ways, as one number
 * (no branches: which way it is comes from the input). On the same tile, or
 * from one next to it onto a quiet one, the points of the way are on those
 * two. See character_tick_plain_fresh() of ../ddnet_optimized/character.c. */
static ALWAYS_INLINE bool way_is_near(const character_t *chr, int pos_flags, bool same_tile) {
  const int nearby = (fabsf(chr->pos.x - chr->prev_pos.x) < 32.0f) & (fabsf(chr->pos.y - chr->prev_pos.y) < 32.0f);
  return (int)same_tile | (((pos_flags & DDNET_TILEFLAG_QUIET) != 0) & nearby);
}

/* ... and for the longer ways */
static ALWAYS_INLINE bool way_is_inert(const collision_t *col, const character_t *chr) {
  const vec2 from = chr->prev_pos, to = chr->pos;
  /* With whole numbers for both ends, the points on the way are between the
   * ends: on the tiles of the rectangle that the two span (for a way shorter
   * than 2^12). */
  if (!(from.x >= 0 && from.y >= 0 && from.x < col->units_width && from.y < col->units_height) ||
      from.x != (float)(int)from.x || from.y != (float)(int)from.y || to.x != (float)(int)to.x ||
      to.y != (float)(int)to.y)
    return false;
  const int fx = (int)from.x >> 5, fy = (int)from.y >> 5, tx = (int)to.x >> 5, ty = (int)to.y >> 5;
  const int x0 = mini(fx, tx), y0 = mini(fy, ty), x1 = maxi(fx, tx) + 1, y1 = maxi(fy, ty) + 1;
  return x1 - x0 <= 64 && y1 - y0 <= 64 && rect_count(col, col->sat_exists, x0, y0, x1, y1) == 0;
}

/* The flags of fastcap (CCharacter::DDRacePostCoreTick) */
static ALWAYS_INLINE void character_fastcap_flags(world_t *w, character_t *chr) {
  if (chr->got_flag[TEAM_RED] && chr->got_flag[TEAM_BLUE])
    return;
  float d = vdistance(chr->prev_pos, chr->pos);
  if (!d)
    return;
  /* (no sample of the way is near a flag if its start is far from both) */
  const float reach = 14 + PHYSICAL_SIZE + d + 2;
  if (!(vdistance(chr->prev_pos, w->flag_pos[TEAM_RED]) < reach) &&
      !(vdistance(chr->prev_pos, w->flag_pos[TEAM_BLUE]) < reach))
    return;
  int end = (int)(d + 1);
  for (int i = 0; i < end; i++) {
    float fraction_of_tick = i / d;
    vec2 pos = vmix(chr->prev_pos, chr->pos, fraction_of_tick);

    for (int check_team = 0; check_team <= TEAM_BLUE; ++check_team) {
      int opposite_team = check_team == TEAM_RED ? TEAM_BLUE : TEAM_RED;
      if (!chr->got_flag[check_team] && vdistance(pos, w->flag_pos[check_team]) < 14 + PHYSICAL_SIZE) {
        if (!chr->got_flag[opposite_team]) {
          race_start(w, chr, fraction_of_tick);
          EMIT_SOUND(w, chr->pos, DDNET_SOUND_CTF_GRAB_PL);
        } else {
          race_finish(w, chr, fraction_of_tick);
          EMIT_SOUND(w, chr->pos, DDNET_SOUND_CTF_CAPTURE);
        }
        chr->got_flag[check_team] = true;
      }
    }
  }
}

/* --------------------------------------------------------------- tick */

/* CCharacter::Tick (and PreTick, DDRaceTick, HandleWeapons, DDRacePostCoreTick) and TickDeferred.
 * pos_flags: collision_flags_at() of chr->pos, which only changes in TickDeferred. */
static ALWAYS_INLINE void character_tick(world_t *w, character_t *chr, game_mode_t m, int pos_flags) {
  const collision_t *col = collision(w);
  core_t *core = &chr->core;

  /* CCharacter::PreTick */
  if (chr->start_time > server_tick(w))
    character_die(w, chr);

  /* CCharacter::DDRaceTick */
  if (!m.health)
    chr->armor = clampi(10 - (chr->freeze_time / 15), 0, 10);
  core->input = chr->input;
  if (chr->freeze_time > 0) {
    chr->freeze_time--;
    core->input.direction = 0;
    core->input.jump = 0;
    core->input.hook = 0;
  }
  /* HandleTuneLayer */
  chr->tune_zone = col->tune ? collision_is_tune(col, collision_get_map_index(col, chr->pos)) : 0;
  const tuning_t *tuning = &w->tuning_values[chr->tune_zone];

  core_tick(w, core, veq_fast(core->pos, chr->pos) ? pos_flags : collision_flags_at(col, core->pos), tuning);

  /* CCharacter::HandleWeapons */
  if (chr->reload_timer) {
    chr->reload_timer--;
  } else {
    /* CCharacter::FireWeapon (nothing is pressed, the input is the one of the direct input before) */
    if (chr->queued_weapon != -1)
      character_do_weapon_switch(w, chr);
    character_fire_weapon_switched(w, chr, chr->input.fire, m);
  }

  /* CCharacter::DDRacePostCoreTick */
  do {
    /* following jump rules can be overridden by tiles, like Refill Jumps, Stopper and Wall Jump */
    if (core->jumps == -1) {
      core->jumped |= 2;
    } else if (core->jumps == 0) {
      core->jumped |= 2;
    } else if (core->jumps == 1 && core->jumped > 0) {
      core->jumped |= 2;
    } else if (core->jumped_total < core->jumps - 1 && core->jumped > 1) {
      core->jumped = 1;
    }

    const int here_tile = collision_tile_at_pos(col, chr->pos);
    const bool same_tile = collision_tile_at_pos(col, chr->prev_pos) == here_tile;

    /* A tee on a tile that does nothing (no death tile and no stopper next
     * to it either, see DDNET_TILEFLAG_INERT) that came there on a way with
     * no tile that does something: that leaves the time of HandleTiles()
     * without a tile. */
    if ((pos_flags & DDNET_TILEFLAG_INERT) && (way_is_near(chr, pos_flags, same_tile) || way_is_inert(col, chr))) {
      if (m.fastcap)
        character_fastcap_flags(w, chr);
      chr->time = (1.0f - chr->start_time_offset + (float)(server_tick(w)) - chr->start_time) / ((float)SERVER_TICK_SPEED);
      chr->move_restrictions = 0;
      break;
    }

    /* collision_get_map_index() */
    const int current_index = (pos_flags & DDNET_TILEFLAG_EXISTS) ? here_tile : -1;

    /* CCharacter::HandleSkippableTiles */
    if (((pos_flags & DDNET_TILEFLAG_NEAR_DEATH) && character_touches_death_tile(col, chr->pos)) ||
        game_layer_clipped(w, chr->pos)) {
      character_die(w, chr);
      break;
    }
    if (current_index >= 0 && collision_is_speedup(col, current_index))
      character_speedup(w, chr, current_index);

    if (m.fastcap)
      character_fastcap_flags(w, chr);

    /* handle Anti-Skip tiles */
    if (same_tile) {
      /* Every point of the way is on this tile: the walk finds it at the start
       * of the tick if it exists, except tile 0 on a way that is not 0 long,
       * which it takes for the last one it found (that begins as 0, see
       * CCollision::GetMapIndicesWithTime). Then there is no tile on the way,
       * and the one where the tee is counts at the end of the tick. */
      if (current_index > 0 || (current_index == 0 && vdistance(chr->prev_pos, chr->pos) == 0))
        character_handle_tiles(w, chr, current_index, 0.0f, pos_flags);
      else
        character_handle_tiles(w, chr, current_index, 1.0f, pos_flags);
    } else {
      map_indices_t indices = collision_map_indices(col, chr->prev_pos, chr->pos);
      indices.end = (int)ceilf(indices.d);
      int index;
      float fraction_of_tick;
      bool any_index = false;
      while (map_indices_with_time_next(&indices, &index, &fraction_of_tick)) {
        any_index = true;
        character_handle_tiles(w, chr, index, fraction_of_tick, pos_flags);
      }
      if (!any_index)
        character_handle_tiles(w, chr, current_index, 1.0f, pos_flags);
    }
  } while (0);

  chr->prev_pos = core->pos;

  /* CCharacter::TickDeferred, for a tee that is still in the world */
  if (chr->alive) {
#ifdef __SSE4_1__
    core_move_quantize(w, core, tuning);
#else
    core_move(w, core, tuning);
    core_quantize(core);
#endif
    chr->pos = core->pos;

    /* the sounds of the events of the core */
    if (EVENT_BUILD) {
      const int events = core->triggered_events;
      if (events & COREEVENT_GROUND_JUMP)
        EMIT_SOUND(w, chr->pos, DDNET_SOUND_PLAYER_JUMP);
      if (events & COREEVENT_HOOK_ATTACH_GROUND)
        EMIT_SOUND(w, chr->pos, DDNET_SOUND_HOOK_ATTACH_GROUND);
      if (events & COREEVENT_HOOK_HIT_NOHOOK)
        EMIT_SOUND(w, chr->pos, DDNET_SOUND_HOOK_NOATTACH);
      /* (and the effect a client makes of a double jump, which the server leaves to it) */
      if (events & COREEVENT_AIR_JUMP)
        EMIT_PARTICLE(w, chr->pos, DDNET_PARTICLE_AIR_JUMP);
    }
  }
}

/* One iteration of the tick loop in CServer::Run, in a mode */
static ALWAYS_INLINE void world_tick_mode(world_t *w, game_mode_t m) {
  player_t *player = &w->player;
  character_t *chr = &w->character;

  if (chr->hit_num > 0)
    chr->hit_num--;
  chr->core.hook_accel = v2(0, 0);

  /* CPlayer::OnPredictedEarlyInput: weapons fire */
  const input_t *input = &player->input;
  player->player_flags = input->player_flags;
  if (!player->has_character && (input->fire & 1))
    player->spawning = true;
  if (player->has_character && !(player->player_flags & (DDNET_PLAYERFLAG_CHATTING | DDNET_PLAYERFLAG_SPEC_CAM)))
    character_on_direct_input(w, chr, input, m);

  w->tick++;

  /* (CPlayer::OnPredictedInput: taken with the direct input) */

  /* CGameWorld::Tick (where the tee is only changes in its TickDeferred) */
  const int here_flags = get_player_char(w) ? collision_flags_at(collision(w), chr->pos) : 0;
  if (m.weapons && w->num_projectiles)
    projectiles_tick(w, m);
  if (w->num_pickups)
    pickups_tick(w, m, here_flags);
  if (get_player_char(w))
    character_tick(w, chr, m, here_flags);

  player_tick(w);
}

void ddnet_world_tick(ddnet_world_t *w) {
  switch (w->config.gamemode) {
  case DDNET_MODE_UNIQUE_RACE:
  default:
    if (w->config.sv_health_and_ammo)
      world_tick_mode(w, (game_mode_t){.health = true, .fastcap = false, .weapons = true});
    else
      world_tick_mode(w, (game_mode_t){.health = false, .fastcap = false, .weapons = true});
    break;
  case DDNET_MODE_UNIQUE_FASTCAP:
    world_tick_mode(w, (game_mode_t){.health = true, .fastcap = true, .weapons = true});
    break;
  case DDNET_MODE_UNIQUE_FASTCAP_NO_WEAPONS:
    world_tick_mode(w, (game_mode_t){.health = true, .fastcap = true, .weapons = false});
    break;
  }
}
