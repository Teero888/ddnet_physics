/* Port of CCharacterCore from DDNet's src/game/gamecore.cpp: movement,
 * jumping, the hook and tee vs. tee collisions. */
#include "internal.h"

float velocity_ramp(float value, float start, float range, float curvature) {
  if (value < start)
    return 1.0f;
  return 1.0f / powf(curvature, (value - start) / range);
}

/* CountInput: fire, next_weapon and prev_weapon are counters whose lowest bit
 * is the button state. This counts the presses and releases between two values. */
input_count_t count_input(int prev, int cur) {
  input_count_t c = {0, 0};
  prev &= INPUT_STATE_MASK;
  cur &= INPUT_STATE_MASK;
  int i = prev;
  while (i != cur) {
    i = (i + 1) & INPUT_STATE_MASK;
    if (i & 1)
      c.presses++;
    else
      c.releases++;
  }
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
void core_tick(world_t *w, core_t *core, bool use_input, bool do_deferred_tick) {
  const collision_t *col = collision(w);
  const tuning_t *tuning = &core->tuning;

  switch_active_ctx_t ctx = {w, core};
  core->move_restrictions = collision_get_move_restrictions(col, use_input ? core_is_switch_active_cb : NULL,
                                                            &ctx, core->pos, 18.0f, -1);
  core->triggered_events = 0;

  /* get ground state */
  const bool grounded = collision_is_on_ground(col, core->pos, PHYSICAL_SIZE);
  vec2 target_direction = vnormalize(v2(core->input.target_x, core->input.target_y));

  core->vel.y += ddnet_tune(tuning->gravity);

  float max_speed =
      grounded ? ddnet_tune(tuning->ground_control_speed) : ddnet_tune(tuning->air_control_speed);
  float accel = grounded ? ddnet_tune(tuning->ground_control_accel) : ddnet_tune(tuning->air_control_accel);
  float friction = grounded ? ddnet_tune(tuning->ground_friction) : ddnet_tune(tuning->air_friction);

  /* handle input */
  if (use_input) {
    core->direction = core->input.direction;

    /* setup angle; atan2 is the double version, called with integers */
    float tmp_angle = atan2(core->input.target_y, core->input.target_x);
    if (tmp_angle < -(PI / 2.0f))
      core->angle = (int)((tmp_angle + (2.0f * PI)) * 256.0f);
    else
      core->angle = (int)(tmp_angle * 256.0f);

    /* Special jump cases:
     * jumps == -1: A tee may only make one ground jump. Second jumped bit is always set
     * jumps == 0: A tee may not make a jump. Second jumped bit is always set
     * jumps == 1: A tee may do either a ground jump or an air jump. Second jumped bit is set after the first
     * jump The second jumped bit can be overridden by special tiles so that the tee can nevertheless jump. */

    /* handle jump */
    if (core->input.jump) {
      if (!(core->jumped & 1)) {
        if (grounded && (!(core->jumped & 2) || core->jumps != 0)) {
          core->triggered_events |= COREEVENT_GROUND_JUMP;
          core->vel.y = -ddnet_tune(tuning->ground_jump_impulse);
          if (core->jumps > 1)
            core->jumped |= 1;
          else
            core->jumped |= 3;
          core->jumped_total = 0;
        } else if (!(core->jumped & 2)) {
          core->triggered_events |= COREEVENT_AIR_JUMP;
          core->vel.y = -ddnet_tune(tuning->air_jump_impulse);
          core->jumped |= 3;
          core->jumped_total++;
        }
      }
    } else {
      core->jumped &= ~1;
    }

    /* handle hook */
    if (core->input.hook) {
      if (core->hook_state == HOOK_IDLE) {
        core->hook_state = HOOK_FLYING;
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
  if (grounded) {
    core->jumped &= ~2;
    core->jumped_total = 0;
  }

  /* add the speed modification according to players wanted direction */
  if (core->direction < 0)
    core->vel.x = saturated_add(-max_speed, max_speed, core->vel.x, -accel);
  if (core->direction > 0)
    core->vel.x = saturated_add(-max_speed, max_speed, core->vel.x, accel);
  if (core->direction == 0)
    core->vel.x *= friction;

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
    vec2 hook_base = core->pos;
    if (core->new_hook)
      hook_base = core->hook_tele_base;
    vec2 new_pos = vadd(core->hook_pos, vscale(core->hook_dir, ddnet_tune(tuning->hook_fire_speed)));
    if (vdistance(hook_base, new_pos) > ddnet_tune(tuning->hook_length)) {
      core->hook_state = HOOK_RETRACT_START;
      new_pos =
          vadd(hook_base, vscale(vnormalize(vsub(new_pos, hook_base)), ddnet_tune(tuning->hook_length)));
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

    /* Check against other players first */
    if (!core->hook_hit_disabled && ddnet_tune(tuning->player_hooking) &&
        (core->hook_state == HOOK_FLYING || !core->new_hook)) {
      float distance = 0.0f;
      for (int i = 0; i < MAX_CLIENTS; i++) {
        core_t *char_core = world_core(w, i);
        if (!char_core || char_core == core ||
            ((core->id != -1 && !teams_can_collide(w, i, core->id)) || char_core->solo || core->solo))
          continue;

        vec2 closest_point;
        if (closest_point_on_line(core->hook_pos, new_pos, char_core->pos, &closest_point)) {
          if (vdistance(char_core->pos, closest_point) < PHYSICAL_SIZE + 2.0f) {
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
      if ((hook_vel.x < 0 && core->direction < 0) || (hook_vel.x > 0 && core->direction > 0))
        hook_vel.x *= 0.95f;
      else
        hook_vel.x *= 0.75f;

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
    core_tick_deferred(w, core);
}

/* CCharacterCore::TickDeferred: the forces between tees. */
void core_tick_deferred(world_t *w, core_t *core) {
  const tuning_t *tuning = &core->tuning;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    core_t *char_core = world_core(w, i);
    if (!char_core)
      continue;

    if (char_core == core || (core->id != -1 && !teams_can_collide(w, core->id, i)))
      continue; /* make sure that we don't nudge our self */

    if (core->solo || char_core->solo)
      continue;

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

  if (core->hook_state != HOOK_FLYING)
    core->new_hook = false;

  /* clamp the velocity to something sane */
  if (vlength(core->vel) > 6000)
    core->vel = vscale(vnormalize(core->vel), 6000);
}

/* CCharacterCore::Move */
void core_move(world_t *w, core_t *core) {
  const collision_t *col = collision(w);
  const tuning_t *tuning = &core->tuning;

  float ramp_value = velocity_ramp(vlength(core->vel) * 50, ddnet_tune(tuning->velramp_start),
                                   ddnet_tune(tuning->velramp_range), ddnet_tune(tuning->velramp_curvature));

  core->vel.x = core->vel.x * ramp_value;

  vec2 new_pos = core->pos;

  vec2 old_vel = core->vel;
  bool grounded = false;
  collision_move_box(col, &new_pos, &core->vel, PHYSICAL_SIZE_VEC2,
                     v2(ddnet_tune(tuning->ground_elasticity_x), ddnet_tune(tuning->ground_elasticity_y)),
                     &grounded);

  if (grounded) {
    core->jumped &= ~2;
    core->jumped_total = 0;
  }

  core->colliding = 0;
  if (core->vel.x < 0.001f && core->vel.x > -0.001f) {
    if (old_vel.x > 0)
      core->colliding = 1;
    else if (old_vel.x < 0)
      core->colliding = 2;
  } else {
    core->left_wall = true;
  }

  core->vel.x = core->vel.x * (1.0f / ramp_value);

  if (ddnet_tune(tuning->player_collision) && !core->collision_disabled && !core->solo) {
    /* check player collision */
    float distance = vdistance(core->pos, new_pos);
    if (distance > 0) {
      int end = (int)(distance + 1);
      vec2 last_pos = core->pos;
      for (int i = 0; i < end; i++) {
        float a = i / distance;
        vec2 pos = vmix(core->pos, new_pos, a);
        for (int p = 0; p < MAX_CLIENTS; p++) {
          core_t *char_core = world_core(w, p);
          if (!char_core || char_core == core)
            continue;
          if (core->solo || char_core->solo || char_core->collision_disabled ||
              (core->id != -1 && !teams_can_collide(w, core->id, p)))
            continue;
          float d = vdistance(pos, char_core->pos);
          if (d < PHYSICAL_SIZE) {
            if (a > 0.0f)
              core->pos = last_pos;
            else if (vdistance(new_pos, char_core->pos) > d)
              core->pos = new_pos;
            return;
          }
        }
        last_pos = pos;
      }
    }
  }

  core->pos = new_pos;
}

/* CCharacterCore::Quantize: the server rounds the core to what fits into a
 * snapshot at the end of every tick (Write() followed by Read()). */
void core_quantize(world_t *w, core_t *core) {
  (void)w;
  core->pos.x = round_to_int(core->pos.x);
  core->pos.y = round_to_int(core->pos.y);
  core->vel.x = round_to_int(core->vel.x * 256.0f) / 256.0f;
  core->vel.y = round_to_int(core->vel.y * 256.0f) / 256.0f;
  core->hook_pos.x = round_to_int(core->hook_pos.x);
  core->hook_pos.y = round_to_int(core->hook_pos.y);
  core->hook_dir.x = round_to_int(core->hook_dir.x * 256.0f) / 256.0f;
  core->hook_dir.y = round_to_int(core->hook_dir.y * 256.0f) / 256.0f;
}
