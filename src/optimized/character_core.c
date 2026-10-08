/* Port of CCharacterCore from DDNet's src/game/gamecore.cpp: movement,
 * jumping, the hook and tee vs. tee collisions. */
#include "internal.h"
#include "powf.h"

/* VelocityRamp(): 1 / std::pow(curvature, ...), which is powf(). Most of the
 * time that is glibc's, for which the same is computed here without the call,
 * see powf.h. */
inline __attribute__((always_inline)) float velocity_ramp(const tuning_t *tuning, float value) {
  if (value < tuning->velramp_start)
    return 1.0f;
  PROF_COUNT(8, "powf");
  const float y = (value - tuning->velramp_start) / tuning->velramp_range;
#if DDNET_POWF_REPLICA
  double ylogx;
  if (tuning->velramp_powf != VELRAMP_POWF_LIBRARY && powf_fast(tuning->velramp_log2_curvature, y, &ylogx))
    return 1.0f / (tuning->velramp_powf == VELRAMP_POWF_FMA ? powf_exp2_fma(ylogx) : powf_exp2_sse2(ylogx));
#endif
  return 1.0f / powf(tuning->velramp_curvature, y);
}

/* Which powf() velocity_ramp() can compute itself: the one of glibc, of which
 * there are two (see powf.h). Which one this is, if any, is asked powf() for
 * inputs where the two differ, and checked with more. (volatile: the compiler
 * would rather work out powf() itself, and not like glibc does.) */
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

/* CountInput: fire, next_weapon and prev_weapon are counters whose lowest bit
 * is the button state. This counts the presses and releases between two values. */
DDNET_HOT input_count_t count_input(int prev, int cur) {
  /* DDNet counts from prev up to cur one by one: every odd value on the way
   * is a press, every even one a release. */
  prev &= INPUT_STATE_MASK;
  cur &= INPUT_STATE_MASK;
  const int steps = (cur - prev) & INPUT_STATE_MASK;
  input_count_t c;
  c.presses = (steps + 1 - (prev & 1)) >> 1;
  c.releases = steps - c.presses;
  return c;
}

/* CCharacterCore::SetHookedPlayer. DDNet also maintains the set of players
 * attached to each tee here, which is only used to decide what to send to
 * clients. */
void core_set_hooked_player(world_t *w, core_t *core, int hooked_player) {
  (void)w;
  core->hooked_player = hooked_player;
}

/* CCharacterCore::Reset */
void core_reset(world_t *w, core_t *core) {
  core->pos = v2(0, 0);
  core->vel = v2(0, 0);
  core->new_hook = false;
  core->hook_pos = v2(0, 0);
  core->hook_dir = v2(0, 0);
  core->hook_tele_base = v2(0, 0);
  core->hook_tick = 0;
  core->hook_state = HOOK_IDLE;
  core_set_hooked_player(w, core, -1);
  core->jumped = 0;
  core->jumped_total = 0;
  core->jumps = 2;
  core->triggered_events = 0;

  /* DDNet Character */
  core->solo = false;
  core->jetpack = false;
  core->collision_disabled = false;
  core->endless_hook = false;
  core->endless_jump = false;
  core->hammer_hit_disabled = false;
  core->grenade_hit_disabled = false;
  core->laser_hit_disabled = false;
  core->shotgun_hit_disabled = false;
  core->hook_hit_disabled = false;

  core->has_telegun_gun = false;
  core->has_telegun_grenade = false;
  core->has_telegun_laser = false;
  core->freeze_start = 0;
  core->freeze_end = 0;
  core->is_in_freeze = false;
  core->deep_frozen = false;
  core->live_frozen = false;

  /* never initialize both to 0 */
  core->input.target_x = 0;
  core->input.target_y = -1;
}

typedef struct switch_active_ctx_t {
  world_t *w;
  core_t *core;
} switch_active_ctx_t;

/* CCharacterCore::IsSwitchActiveCb */
static bool core_is_switch_active_cb(unsigned char number, void *user) {
  switch_active_ctx_t *ctx = user;
  world_t *w = ctx->w;
  core_t *core = ctx->core;
  if (w->num_switchers != 0)
    if (core->id != -1)
      return switchers(w)[number].status[teams_team(w, core->id)];
  return false;
}

/* CCharacterCore::Tick */
#ifdef __SSE2__
#include <emmintrin.h>
#ifdef __SSE4_1__
#include <smmintrin.h>
#endif
#ifdef __AVX__
#include <immintrin.h>
#endif
/* mask ? a : b */
static inline __m128 select_ss(__m128 mask, __m128 a, __m128 b) {
#ifdef __SSE4_1__
  /* (one instruction; the mask is all ones or all zeros) */
  return _mm_blendv_ps(b, a, mask);
#else
  return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
#endif
}
#endif

/* The other tees the loops of DDNet over pairs of tees go on with for core: not core, in a team core can
 * collide with (or core->id is -1), neither of them solo and, with collision_only, collision not disabled.
 * Of those, the ones with the position in the rectangle from low to high (or the one with the id hooked),
 * in the order of the loops (core_ids). */
static inline __attribute__((always_inline)) int core_tees_in(const world_t *w, const core_t *core, vec2 low,
                                                              vec2 high, bool collision_only, int hooked,
                                                              int *out) {
  if (core->solo)
    return 0;
  const int num_cores = w->num_cores;
  const uint8_t *ids = w->core_ids;
  const character_t *chars = w->characters;
  /* The rectangle without branches: which tees are in it is not predictable. (Not the tee of core itself,
   * which is in its own rectangles.) */
  int count = 0;
#ifdef __SSE2__
  const tee_snapshot_t *snapshot = w->tee_snapshot;
  if (snapshot) {
    const int self = (int)((const character_t *)((const char *)core - offsetof(character_t, core)) - chars);
    /* four at once, of the positions next to each other (inside: none of the comparisons for outside
     * holds, like the ones below) */
    const int num = snapshot->num;
    uint64_t inside[(DDNET_MAX_CLIENTS + 63) / 64] = {0};
#ifdef __AVX__
    if (num <= 8) {
      const __m256 x = _mm256_loadu_ps(snapshot->x), y = _mm256_loadu_ps(snapshot->y);
      const __m256 outside = _mm256_or_ps(_mm256_or_ps(_mm256_cmp_ps(x, _mm256_set1_ps(low.x), _CMP_LT_OQ),
                                                       _mm256_cmp_ps(_mm256_set1_ps(high.x), x, _CMP_LT_OQ)),
                                          _mm256_or_ps(_mm256_cmp_ps(y, _mm256_set1_ps(low.y), _CMP_LT_OQ),
                                                       _mm256_cmp_ps(_mm256_set1_ps(high.y), y, _CMP_LT_OQ)));
      inside[0] = (uint64_t)(~_mm256_movemask_ps(outside) & 255);
    } else
#endif
      /* (each word of the mask in a register: or-ed into memory, every step would wait for the last) */
      for (int word = 0; word * 64 < num; word++) {
        uint64_t bits = 0;
        const int end = num < word * 64 + 64 ? num : word * 64 + 64;
#ifdef __AVX__
        /* (eight at once; the snapshot has room for the eight from a multiple of eight) */
        const __m256 low_x = _mm256_set1_ps(low.x), low_y = _mm256_set1_ps(low.y);
        const __m256 high_x = _mm256_set1_ps(high.x), high_y = _mm256_set1_ps(high.y);
        for (int base = word * 64; base < end; base += 8) {
          const __m256 x = _mm256_loadu_ps(&snapshot->x[base]), y = _mm256_loadu_ps(&snapshot->y[base]);
          const __m256 outside = _mm256_or_ps(
              _mm256_or_ps(_mm256_cmp_ps(x, low_x, _CMP_LT_OQ), _mm256_cmp_ps(high_x, x, _CMP_LT_OQ)),
              _mm256_or_ps(_mm256_cmp_ps(y, low_y, _CMP_LT_OQ), _mm256_cmp_ps(high_y, y, _CMP_LT_OQ)));
          bits |= (uint64_t)(~_mm256_movemask_ps(outside) & 255) << (base & 63);
        }
#else
      const __m128 low_x = _mm_set1_ps(low.x), low_y = _mm_set1_ps(low.y);
      const __m128 high_x = _mm_set1_ps(high.x), high_y = _mm_set1_ps(high.y);
      for (int base = word * 64; base < end; base += 4) {
        const __m128 x = _mm_load_ps(&snapshot->x[base]), y = _mm_load_ps(&snapshot->y[base]);
        const __m128 outside = _mm_or_ps(_mm_or_ps(_mm_cmplt_ps(x, low_x), _mm_cmplt_ps(high_x, x)),
                                         _mm_or_ps(_mm_cmplt_ps(y, low_y), _mm_cmplt_ps(high_y, y)));
        bits |= (uint64_t)(~_mm_movemask_ps(outside) & 15) << (base & 63);
      }
#endif
        inside[word] = bits;
      }
    /* (not past the tees; the hooked one in any case; not the tee itself) */
    if (num & 63)
      inside[num >> 6] &= ((uint64_t)1 << (num & 63)) - 1;
    if (hooked >= 0 && snapshot->slot[hooked] != 0xff)
      inside[snapshot->slot[hooked] >> 6] |= (uint64_t)1 << (snapshot->slot[hooked] & 63);
    if (snapshot->slot[self] != 0xff)
      inside[snapshot->slot[self] >> 6] &= ~((uint64_t)1 << (snapshot->slot[self] & 63));
    /* With more than eight, the other conditions right away for the tees in the rectangle (a second pass over
     * a list of many of them costs more than this one; with up to eight it is the other way round) */
    if (num > 8) {
      const int id = core->id;
      for (int word = 0; word * 64 < num; word++)
        for (uint64_t bits = inside[word]; bits; bits &= bits - 1) {
          const int p = snapshot->ids[word * 64 + __builtin_ctzll(bits)];
          const core_t *other = &chars[p].core;
          if (other->solo || (collision_only && other->collision_disabled) ||
              (id != -1 && !teams_can_collide(w, id, p)))
            continue;
          out[count++] = p;
        }
      return count;
    }
    for (uint64_t bits = inside[0]; bits; bits &= bits - 1)
      out[count++] = snapshot->ids[__builtin_ctzll(bits)];
  } else if (num_cores == 2 && (&chars[ids[0]].core == core || &chars[ids[1]].core == core)) {
    /* (two tees, one of them this one, which is the most common case with more than one: the other one) */
    const int p = ids[&chars[ids[0]].core == core];
    const vec2 pos = chars[p].core.pos;
    out[0] = p;
    count = (!(pos.x < low.x) & !(pos.x > high.x) & !(pos.y < low.y) & !(pos.y > high.y)) | (p == hooked);
  } else {
    const __m128 lo = _mm_setr_ps(low.x, low.y, 0, 0), hi = _mm_setr_ps(high.x, high.y, 0, 0);
    for (int n = 0; n < num_cores; n++) {
      const int p = ids[n];
      const __m128 pos = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&chars[p].core.pos));
      const int outside = _mm_movemask_ps(_mm_or_ps(_mm_cmplt_ps(pos, lo), _mm_cmplt_ps(hi, pos)));
      out[count] = p;
      count += ((outside == 0) | (p == hooked)) & (&chars[p].core != core);
    }
  }
#else
  for (int n = 0; n < num_cores; n++) {
    const int p = ids[n];
    const vec2 pos = chars[p].core.pos;
    out[count] = p;
    count += ((!(pos.x < low.x) & !(pos.x > high.x) & !(pos.y < low.y) & !(pos.y > high.y)) | (p == hooked)) &
             (&chars[p].core != core);
  }
#endif
  /* the other conditions, for those (as they usually hold: with branches) */
  const int id = core->id;
  int kept = 0;
  for (int k = 0; k < count; k++) {
    const int p = out[k];
    const core_t *other = &chars[p].core;
    if (other->solo || (collision_only && other->collision_disabled) ||
        (id != -1 && !teams_can_collide(w, id, p)))
      continue;
    out[kept++] = p;
  }
  return kept;
}

/* CCharacterCore::Tick: the hook on its way to new_pos hits a tee. */
DDNET_NOINLINE DDNET_NO_CANARY static void core_hook_tees(world_t *w, core_t *core, vec2 new_pos) {
  float distance = 0.0f;
  /* The closest point is on the line, so in its rectangle: a tee further from that than the reach (a bit
   * more than PHYSICAL_SIZE + 2) is not hit. */
  const float reach = PHYSICAL_SIZE + 3.0f;
  const vec2 low = v2(minf(core->hook_pos.x, new_pos.x) - reach, minf(core->hook_pos.y, new_pos.y) - reach);
  const vec2 high = v2(maxf(core->hook_pos.x, new_pos.x) + reach, maxf(core->hook_pos.y, new_pos.y) + reach);
  int near[DDNET_MAX_CLIENTS];
  const int num_near = core_tees_in(w, core, low, high, false, -1, near);
  for (int n = 0; n < num_near; n++) {
    const int i = near[n];
    core_t *char_core = &w->characters[i].core;
    vec2 closest_point;
    if (closest_point_on_line(core->hook_pos, new_pos, char_core->pos, &closest_point)) {
      /* "vdistance(char_core->pos, closest_point) < PHYSICAL_SIZE + 2" without the root, see
       * core_move_into_tees() (the root of the largest float below 900 rounds to below 30) */
      const vec2 apart = vsub(char_core->pos, closest_point);
      if (vdot(apart, apart) < (PHYSICAL_SIZE + 2.0f) * (PHYSICAL_SIZE + 2.0f)) {
        if (core->hooked_player == -1 || vdistance(core->hook_pos, char_core->pos) < distance) {
          core->triggered_events |= COREEVENT_HOOK_ATTACH_PLAYER;
          core->hook_state = HOOK_GRABBED;
          core_set_hooked_player(w, core, i);
          distance = vdistance(core->hook_pos, char_core->pos);
        }
      }
    }
  }
}

/* CCharacterCore::Tick for a hook that is on its way. */
DDNET_NOINLINE static void core_hook_fly(world_t *w, core_t *core, const tuning_t *tuning) {
  const collision_t *col = collision(w);
  vec2 hook_base = core->pos;
  if (core->new_hook)
    hook_base = core->hook_tele_base;
  vec2 new_pos = vadd(core->hook_pos, vscale(core->hook_dir, ddnet_tune(tuning->hook_fire_speed)));
  /* "vdistance(hook_base, new_pos) > hook_length" without the root, see hook_length_squared */
  const vec2 from_base = vsub(hook_base, new_pos);
  if (vdot(from_base, from_base) >= tuning->hook_length_squared) {
    core->hook_state = HOOK_RETRACT_START;
    new_pos = vadd(hook_base, vscale(vnormalize(vsub(new_pos, hook_base)), ddnet_tune(tuning->hook_length)));
  }

  /* make sure that the hook doesn't go though the ground */
  bool going_to_hit_ground = false;
  bool going_to_retract = false;
  bool going_through_tele = false;
  int tele_nr = 0;
  int hit = collision_intersect_line_tele_hook(col, w->config.sv_old_teleport_hook, core->hook_pos, new_pos,
                                               &new_pos, NULL, &tele_nr);

  if (hit) {
    if (hit == TILE_NOHOOK)
      going_to_retract = true;
    else if (hit == TILE_TELEINHOOK)
      going_through_tele = true;
    else
      going_to_hit_ground = true;
  }

  /* Check against other players first (none for a tee that is alone in the world, which the loop of DDNet
   * only finds itself in) */
  if (!core->hook_hit_disabled && ddnet_tune(tuning->player_hooking) &&
      (core->hook_state == HOOK_FLYING || !core->new_hook) &&
      !(w->num_cores == 1 && &w->characters[w->core_ids[0]].core == core))
    core_hook_tees(w, core, new_pos);

  if (core->hook_state == HOOK_FLYING) {
    /* check against ground */
    if (going_to_hit_ground) {
      core->triggered_events |= COREEVENT_HOOK_ATTACH_GROUND;
      core->hook_state = HOOK_GRABBED;
    } else if (going_to_retract) {
      core->triggered_events |= COREEVENT_HOOK_HIT_NOHOOK;
      core->hook_state = HOOK_RETRACT_START;
    }

    if (going_through_tele && col->tele_outs[tele_nr - 1].count != 0) {
      core->triggered_events = 0;
      core_set_hooked_player(w, core, -1);

      core->new_hook = true;
      const vec2 target_direction = vnormalize(v2(core->input.target_x, core->input.target_y));
      int random_out = world_pick_tele_out(w, core->id, col->tele_outs[tele_nr - 1].count);
      core->hook_pos = vadd(col->tele_outs[tele_nr - 1].positions[random_out],
                            vscale(vscale(target_direction, PHYSICAL_SIZE), 1.5f));
      core->hook_dir = target_direction;
      core->hook_tele_base = core->hook_pos;
    } else {
      core->hook_pos = new_pos;
    }
  }
}

#ifdef __SSE2__
/* What CCharacterCore::Tick does to the horizontal velocity for a direction:
 * SaturatedAdd(-max_speed, max_speed, vel, -accel or accel), or vel * friction
 * without a direction. The comparisons are the ones of saturated_add(). */
static inline float move_select(int direction, float vel_x, float max_speed, float accel, float friction) {
  const __m128 vel = _mm_set_ss(vel_x);
  const __m128 max = _mm_set_ss(max_speed), min = _mm_set_ss(-max_speed);
  /* (out of a table: picked with a branch it would be mispredicted) */
  const float either[2] = {accel, -accel};
  const __m128 modifier = _mm_set_ss(either[direction < 0]);
  const __m128 sum = _mm_add_ss(vel, modifier);
  /* modifier < 0: vel < min ? vel : (sum < min ? min : sum) */
  const __m128 down = select_ss(_mm_cmplt_ss(vel, min), vel, select_ss(_mm_cmplt_ss(sum, min), min, sum));
  /* else: vel > max ? vel : (sum > max ? max : sum) */
  const __m128 up = select_ss(_mm_cmpgt_ss(vel, max), vel, select_ss(_mm_cmpgt_ss(sum, max), max, sum));
  const __m128 moved = select_ss(_mm_cmplt_ss(modifier, _mm_setzero_ps()), down, up);
  const __m128 slowed = _mm_mul_ss(vel, _mm_set_ss(friction));
  const __m128 still = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_cvtsi32_si128(direction), _mm_setzero_si128()));
  return _mm_cvtss_f32(select_ss(still, slowed, moved));
}
#endif

static inline void core_tick_deferred_tuned(world_t *w, core_t *core, const tuning_t *tuning);

/* For the jump in core_tick(): bits 0 and 1 the new two bits of jumped, 4 a
 * jump from the ground, 8 one in the air. The index is: key down (1), the two
 * bits of jumped (2, 4), on the ground (8), and 0, 16 or 32 for no jumps, one
 * or -1, or more than one. Worked out from the rules of
 * CCharacterCore::Tick, including the one that clears bit 1 and the count of
 * air jumps on the ground. */
static const uint8_t jump_table[48] = {0, 11, 0, 1, 2, 2, 2, 3, 0, 5, 0, 1, 0, 0, 0, 1,
                                       0, 11, 0, 1, 2, 2, 2, 3, 0, 5, 0, 1, 0, 5, 0, 1,
                                       0, 11, 0, 1, 2, 2, 2, 3, 0, 5, 0, 1, 0, 5, 0, 1};

/* core_tick() with the tuning of the tee, for the callers that have it */
static inline __attribute__((always_inline)) void core_tick_tuned(world_t *w, core_t *core, bool use_input,
                                                                  bool do_deferred_tick, int flags,
                                                                  const tuning_t *tuning, bool on_map) {
  const collision_t *col = collision(w);

  /* flags: collision_flags_at() of the position. No stopper or door around
   * means no restrictions, and nothing solid around means in the air. */
  bool grounded = false;
  if (!(flags & DDNET_TILEFLAG_NEAR_STOPPER)) {
    core->move_restrictions = 0;
  } else {
    switch_active_ctx_t ctx = {w, core};
    core->move_restrictions = collision_get_move_restrictions(
        col, use_input ? core_is_switch_active_cb : NULL, &ctx, core->pos, 18.0f, -1);
  }
  /* get ground state (on_map: the position is known to be on the map) */
  if (flags & DDNET_TILEFLAG_NEAR_SOLID)
    grounded = on_map ? collision_is_on_ground_on_map(col, core->pos, PHYSICAL_SIZE)
                      : collision_is_on_ground(col, core->pos, PHYSICAL_SIZE);
  core->triggered_events = 0;

  core->vel.y += ddnet_tune(tuning->gravity);

  float max_speed =
      grounded ? ddnet_tune(tuning->ground_control_speed) : ddnet_tune(tuning->air_control_speed);
  float accel = grounded ? ddnet_tune(tuning->ground_control_accel) : ddnet_tune(tuning->air_control_accel);
  float friction = grounded ? ddnet_tune(tuning->ground_friction) : ddnet_tune(tuning->air_friction);

  /* handle input */
  if (use_input) {
    core->direction = core->input.direction;

    /* the angle only depends on the input, it is computed when somebody asks for it */
    core->has_ticked = true;

    /* Special jump cases:
     * jumps == -1: A tee may only make one ground jump. Second jumped bit is always set
     * jumps == 0: A tee may not make a jump. Second jumped bit is always set
     * jumps == 1: A tee may do either a ground jump or an air jump. Second jumped bit is set after the first
     * jump The second jumped bit can be overridden by special tiles so that the tee can nevertheless jump. */

    /* handle jump, and the rule for standing on the ground further down (see
     * below). The input is what decides, which a branch predictor cannot
     * know. Everything the nested ifs of DDNet do here follows from whether
     * the key is down, the two bits of jumped, whether the tee stands on the
     * ground and whether it has no jumps, one (or -1) or more: a table of the
     * 48 cases (jump_table), with the new two bits of jumped, and whether the
     * jump is from the ground (4) or in the air (8). */
    {
      const int jumped = core->jumped;
      const int pressed = core->input.jump != 0;
      const int jumps = (core->jumps != 0) + (core->jumps > 1);
      const int e = jump_table[pressed | (jumped & 3) << 1 | (int)grounded << 3 | jumps << 4];
      const int ground = (e >> 2) & 1, air = e >> 3;

      core->triggered_events |= (-ground & COREEVENT_GROUND_JUMP) | (-air & COREEVENT_AIR_JUMP);
      const float vel_y[3] = {core->vel.y, -ddnet_tune(tuning->air_jump_impulse),
                              -ddnet_tune(tuning->ground_jump_impulse)};
      core->vel.y = vel_y[air + 2 * ground];
      /* (the bits above the two are left as they are) */
      core->jumped = (jumped & ~3) | (e & 3);
      /* on the ground it is 0 after the rule below, otherwise only an air jump counts */
      core->jumped_total = (core->jumped_total + air) & -!grounded;
    }

    /* handle hook (left with its branches: without them it was not faster) */
    if (core->input.hook) {
      if (core->hook_state == HOOK_IDLE) {
        core->hook_state = HOOK_FLYING;
        const vec2 target_direction = vnormalize(v2(core->input.target_x, core->input.target_y));
        core->hook_pos = vadd(core->pos, vscale(vscale(target_direction, PHYSICAL_SIZE), 1.5f));
        core->hook_dir = target_direction;
        core_set_hooked_player(w, core, -1);
        core->hook_tick = (int)((float)SERVER_TICK_SPEED * (1.25f - ddnet_tune(tuning->hook_duration)));
        core->triggered_events |= COREEVENT_HOOK_LAUNCH;
      }
    } else {
      core_set_hooked_player(w, core, -1);
      core->hook_state = HOOK_IDLE;
      core->hook_pos = core->pos;
    }
  }

  /* handle jumping
   * 1 bit = to keep track if a jump has been made on this input (player is holding space bar)
   * 2 bit = to track if all air-jumps have been used up (tee gets dark feet) */
  if (!use_input && grounded) {
    /* (with input, the jump table did this already) */
    core->jumped &= ~2;
    core->jumped_total = 0;
  }

  /* add the speed modification according to players wanted direction */
#ifdef __SSE2__
  /* The direction is input, which a branch predictor cannot know. All three
   * outcomes are worked out and one is picked. */
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
    core_set_hooked_player(w, core, -1);
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
    if (core->hooked_player != -1) {
      core_t *char_core = world_core(w, core->hooked_player);
      if (char_core && core->id != -1 && teams_can_keep_hook(w, core->id, char_core->id)) {
        core->hook_pos = char_core->pos;
      } else {
        /* release hook */
        core_set_hooked_player(w, core, -1);
        core->hook_state = HOOK_RETRACTED;
        core->hook_pos = core->pos;
      }
    }

    /* don't do this hook routine when we are already hooked to a player */
    if (core->hooked_player == -1 && vdistance(core->hook_pos, core->pos) > 46.0f) {
      vec2 hook_vel =
          vscale(vnormalize(vsub(core->hook_pos, core->pos)), ddnet_tune(tuning->hook_drag_accel));
      /* the hook as more power to drag you up then down.
       * this makes it easier to get on top of an platform */
      if (hook_vel.y > 0)
        hook_vel.y *= 0.3f;

      /* the hook will boost it's power if the player wants to move
       * in that direction. otherwise it will dampen everything abit */
      /* (the factor picked without a branch: it follows the input) */
      const float factor[2] = {0.75f, 0.95f};
      hook_vel.x *=
          factor[((hook_vel.x < 0) & (core->direction < 0)) | ((hook_vel.x > 0) & (core->direction > 0))];

      vec2 new_vel = vadd(core->vel, hook_vel);

      /* check if we are under the legal limit for the hook */
      const float new_vel_length = vlength(new_vel);
      if (new_vel_length < ddnet_tune(tuning->hook_drag_speed) || new_vel_length < vlength(core->vel))
        core->vel = new_vel; /* no problem. apply */
    }

    /* release hook (max default hook time is 1.25 s) */
    core->hook_tick++;
    if (core->hooked_player != -1 && (core->hook_tick > SERVER_TICK_SPEED + SERVER_TICK_SPEED / 5 ||
                                      !world_core(w, core->hooked_player))) {
      core_set_hooked_player(w, core, -1);
      core->hook_state = HOOK_RETRACTED;
      core->hook_pos = core->pos;
    }
  }

  if (do_deferred_tick)
    core_tick_deferred_tuned(w, core, tuning);
}

DDNET_HOT void core_tick(world_t *w, core_t *core, bool use_input, bool do_deferred_tick, int flags) {
  core_tick_tuned(w, core, use_input, do_deferred_tick, flags, core_tuning(w, core), false);
}

/* CCharacterCore::TickDeferred: the forces between tees. */
/* CCharacterCore::TickDeferred: the other tees, which push this one and are pulled by its hook. */
DDNET_NOINLINE DDNET_NO_CANARY static void core_tick_deferred_tees(world_t *w, core_t *core,
                                                                   const tuning_t *tuning) {
  /* Further than 40 apart in x or y: not pushed, which is below 35, and only dragged by the hook if it is the
   * hooked one. */
  const vec2 reach = v2(40.0f, 40.0f);
  int near[DDNET_MAX_CLIENTS];
  const int num_near =
      core_tees_in(w, core, vsub(core->pos, reach), vadd(core->pos, reach), false, core->hooked_player, near);
  for (int n = 0; n < num_near; n++) {
    const int i = near[n];
    core_t *char_core = &w->characters[i].core;

    /* handle player <-> player collision */
    float distance = vdistance(core->pos, char_core->pos);
    if (distance > 0) {
      vec2 dir = vnormalize(vsub(core->pos, char_core->pos));

      bool can_collide =
          !core->collision_disabled && !char_core->collision_disabled && ddnet_tune(tuning->player_collision);

      if (can_collide && distance < PHYSICAL_SIZE * 1.25f) {
        float a = (PHYSICAL_SIZE * 1.45f - distance);
        float velocity = 0.5f;

        /* make sure that we don't add excess force by checking the
         * direction against the current velocity. if not zero. */
        if (vlength(core->vel) > 0.0001f)
          velocity = 1 - (vdot(vnormalize(core->vel), dir) + 1) / 2;

        core->vel = vadd(core->vel, vscale(vscale(dir, a), velocity * 0.75f));
        core->vel = vscale(core->vel, 0.85f);
      }

      /* handle hook influence */
      if (!core->hook_hit_disabled && core->hooked_player == i && ddnet_tune(tuning->player_hooking)) {
        if (distance > PHYSICAL_SIZE * 1.50f) {
          float hook_accel =
              ddnet_tune(tuning->hook_drag_accel) * (distance / ddnet_tune(tuning->hook_length));
          float drag_speed = ddnet_tune(tuning->hook_drag_speed);

          vec2 temp;
          /* add force to the hooked player */
          temp.x = saturated_add(-drag_speed, drag_speed, char_core->vel.x, hook_accel * dir.x * 1.5f);
          temp.y = saturated_add(-drag_speed, drag_speed, char_core->vel.y, hook_accel * dir.y * 1.5f);
          char_core->vel = clamp_vel(char_core->move_restrictions, temp);
          /* add a little bit force to the guy who has the grip */
          temp.x = saturated_add(-drag_speed, drag_speed, core->vel.x, -hook_accel * dir.x * 0.25f);
          temp.y = saturated_add(-drag_speed, drag_speed, core->vel.y, -hook_accel * dir.y * 0.25f);
          core->vel = clamp_vel(core->move_restrictions, temp);
        }
      }
    }
  }
}

static inline void core_tick_deferred_tuned(world_t *w, core_t *core, const tuning_t *tuning) {

  /* (alone, there is nobody to be nudged by) */
  if (w->num_cores > 1)
    core_tick_deferred_tees(w, core, tuning);

  /* if (core->hook_state != HOOK_FLYING) core->new_hook = false; (as a number: the hook follows the input) */
  core->new_hook = core->new_hook & (core->hook_state == HOOK_FLYING);

  /* clamp the velocity to something sane */
  /* "vlength(vel) > 6000" without the root, see speed_clamp_squared */
  if (vdot(core->vel, core->vel) >= tuning->speed_clamp_squared)
    core->vel = vscale(vnormalize(core->vel), 6000);
}

DDNET_HOT void core_tick_deferred(world_t *w, core_t *core) {
  core_tick_deferred_tuned(w, core, core_tuning(w, core));
}

/* CCharacterCore::Move: the tee stops at other tees on its way, of which only the ones in near
 * (core_tees_in()) can be close enough. True if it did, and its position is set. */
DDNET_NOINLINE static bool core_move_into_tees(world_t *w, core_t *core, vec2 new_pos, const int *near,
                                               int num_near) {
  /* check player collision */
  float distance = vdistance(core->pos, new_pos);
  if (distance > 0) {
    int end = (int)(distance + 1);
    vec2 last_pos = core->pos;
    for (int i = 0; i < end; i++) {
      float a = i / distance;
      vec2 pos = vmix(core->pos, new_pos, a);
      for (int n = 0; n < num_near; n++) {
        const core_t *char_core = &w->characters[near[n]].core;
        /* "vdistance(pos, char_core->pos) < PHYSICAL_SIZE" without the root, which is below 28 for exactly
         * the squares below 28 * 28 (the root of the largest float below 784 rounds to below 28) */
        const vec2 apart = vsub(pos, char_core->pos);
        const float d2 = vdot(apart, apart);
        if (d2 < PHYSICAL_SIZE * PHYSICAL_SIZE) {
          const float d = sqrtf(d2);
          if (a > 0.0f)
            core->pos = last_pos;
          else if (vdistance(new_pos, char_core->pos) > d)
            core->pos = new_pos;
          return true;
        }
      }
      last_pos = pos;
    }
  }
  return false;
}

/* The position is rounded right after the move (core_quantize), and until then only the check for other
 * tees at its end looks at it. Which it only does for a tee that can be close enough to a position on the
 * way: each coordinate gets less far than the velocity (the steps add up to it, and a collision stops or
 * reverses the steps, with an elasticity of 1 at most). With none, the move only has to round like DDNet's.
 */
DDNET_HOT DDNET_NO_CANARY int core_move_tees(world_t *w, const core_t *core, const tuning_t *tuning,
                                             int *near) {
  if (!(w->num_cores > 1 && ddnet_tune(tuning->player_collision) && !core->collision_disabled && !core->solo))
    return 0;
  const vec2 extent = v2(fabsf(core->vel.x), fabsf(core->vel.y));
  /* (a bit more than PHYSICAL_SIZE: within that of a position in the rectangle, for the roundings) */
  const vec2 reach = v2(PHYSICAL_SIZE + 2.0f, PHYSICAL_SIZE + 2.0f);
  return core_tees_in(w, core, vsub(vsub(core->pos, extent), reach), vadd(vadd(core->pos, extent), reach),
                      true, -1, near);
}

/* CCharacterCore::Move */
DDNET_HOT void core_move(world_t *w, core_t *core, const tuning_t *tuning, bool ramp, float ramp_value,
                         const int *near, int num_near) {
  const collision_t *col = collision(w);
  const vec2 elasticity =
      v2(ddnet_tune(tuning->ground_elasticity_x), ddnet_tune(tuning->ground_elasticity_y));
  vec2 new_pos = core->pos;

  vec2 old_vel = core->vel;
  bool grounded = false;
  const bool check_tees = num_near > 0;
  if (check_tees)
    collision_move_box(col, &new_pos, &core->vel, PHYSICAL_SIZE_VEC2, elasticity, &grounded);
  else
    collision_move_box_rounded(col, &new_pos, &core->vel, PHYSICAL_SIZE_VEC2, elasticity, &grounded);

  if (grounded) {
    core->jumped &= ~2;
    core->jumped_total = 0;
  }

  /* Whether the tee was stopped at a wall, and on which side. As numbers:
   * with branches this is mispredicted a lot.
   *   colliding = 0; if (stopped) { if (old_vel.x > 0) colliding = 1; else if (old_vel.x < 0) colliding = 2;
   * } else left_wall = true; */
  {
    const int stopped = (core->vel.x < 0.001f) & (core->vel.x > -0.001f);
    const int side = (old_vel.x > 0) + 2 * (old_vel.x < 0);
    core->colliding = -stopped & side;
    core->left_wall = (core->left_wall | !stopped) != 0;
  }

  if (ramp)
    core->vel.x = core->vel.x * (1.0f / ramp_value);

  if (check_tees && core_move_into_tees(w, core, new_pos, near, num_near))
    return;

  core->pos = new_pos;
}

/* CCharacterCore::Quantize: the server rounds the core to what fits into a
 * snapshot at the end of every tick (Write() followed by Read()). */
#ifdef __SSE2__
#include <emmintrin.h>
/* round_fast(v * scale) * inverse for four values, with the operations of the plain version below */
static inline __m128 quantize4(__m128 v, __m128 scale, __m128 inverse) {
  v = _mm_mul_ps(v, scale);
  /* half with the sign of the value, see round_fast() */
  const __m128 half = _mm_or_ps(_mm_and_ps(v, _mm_set1_ps(-0.0f)), _mm_set1_ps(0.5f));
  return _mm_mul_ps(_mm_cvtepi32_ps(_mm_cvttps_epi32(_mm_add_ps(v, half))), inverse);
}
/* four floats, loaded one by one (the compiler would make one load of it) */
static inline __m128 load4(const float *values) {
  __m128 a = _mm_load_ss(&values[0]), b = _mm_load_ss(&values[1]);
  __m128 c = _mm_load_ss(&values[2]), d = _mm_load_ss(&values[3]);
#if defined(__GNUC__) && !defined(DDNET_PHYSICS_AB)
  __asm__("" : "+x"(a), "+x"(b), "+x"(c), "+x"(d));
#endif
  return _mm_movelh_ps(_mm_unpacklo_ps(a, b), _mm_unpacklo_ps(c, d));
}

#endif

DDNET_HOT void core_quantize(world_t *w, core_t *core) {
  (void)w;
#ifdef __SSE2__
  /* pos and vel, hook_pos and hook_dir are next to each other. Multiplying by
   * 1 changes nothing and multiplying by 1/256 is dividing by 256. */
  _Static_assert(offsetof(core_t, vel) == offsetof(core_t, pos) + 2 * sizeof(float), "layout");
  _Static_assert(offsetof(core_t, hook_dir) == offsetof(core_t, hook_pos) + 2 * sizeof(float), "layout");
  const __m128 scale = _mm_setr_ps(1.0f, 1.0f, 256.0f, 256.0f);
  const __m128 inverse = _mm_setr_ps(1.0f, 1.0f, 1.0f / 256.0f, 1.0f / 256.0f);
  /* The values are put together from single ones: most of them were just
   * written one by one, and a load of four at once would have to wait for
   * all of those to arrive in memory. */
  _mm_storeu_ps(&core->pos.x, quantize4(load4(&core->pos.x), scale, inverse));
  _mm_storeu_ps(&core->hook_pos.x, quantize4(load4(&core->hook_pos.x), scale, inverse));
#else
  core->pos.x = round_fast(core->pos.x);
  core->pos.y = round_fast(core->pos.y);
  core->vel.x = round_fast(core->vel.x * 256.0f) / 256.0f;
  core->vel.y = round_fast(core->vel.y * 256.0f) / 256.0f;
  core->hook_pos.x = round_fast(core->hook_pos.x);
  core->hook_pos.y = round_fast(core->hook_pos.y);
  core->hook_dir.x = round_fast(core->hook_dir.x * 256.0f) / 256.0f;
  core->hook_dir.y = round_fast(core->hook_dir.y * 256.0f) / 256.0f;
#endif
}
