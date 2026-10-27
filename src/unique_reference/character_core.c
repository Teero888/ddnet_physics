/* Port of CCharacterCore from src/game/gamecore.cpp: movement, jumping and
 * the hook. Unique Race is solo, so what a core does with the cores of other
 * tees (hooking them, pushing them, colliding with them) never happens. */
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

/* CCharacterCore::Reset */
void core_reset(core_t *core) {
  core->pos = v2(0, 0);
  core->vel = v2(0, 0);
  core->hook_pos = v2(0, 0);
  core->hook_dir = v2(0, 0);
  core->hook_tick = 0;
  core->hook_state = HOOK_IDLE;
  core->jumped = 0;
  core->jumped_total = 0;
  core->jumps = 2;
  core->triggered_events = 0;

  core->freeze_start = 0;

  /* never initialize both to 0 */
  core->input.target_x = 0;
  core->input.target_y = -1;
}

/* CCharacterCore::Tick */
void core_tick(world_t *w, core_t *core, bool use_input) {
  const collision_t *col = collision(w);
  const tuning_t *tuning = &core->tuning;

  /* (no doors: the maps have no switch layer) */
  core->move_restrictions = collision_get_move_restrictions(col, NULL, NULL, core->pos, 18.0f, -1);
  core->triggered_events = 0;

  /* get ground state */
  const bool grounded = collision_check_point(col, core->pos.x + PHYSICAL_SIZE / 2, core->pos.y + PHYSICAL_SIZE / 2 + 5) ||
                        collision_check_point(col, core->pos.x - PHYSICAL_SIZE / 2, core->pos.y + PHYSICAL_SIZE / 2 + 5);
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
        core->hook_tick = (int)((float)SERVER_TICK_SPEED * (1.25f - ddnet_tune(tuning->hook_duration)));
        core->triggered_events |= COREEVENT_HOOK_LAUNCH;
      }
    } else {
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
    core->hook_pos = core->pos;
  } else if (core->hook_state >= HOOK_RETRACT_START && core->hook_state < HOOK_RETRACT_END) {
    core->hook_state++;
  } else if (core->hook_state == HOOK_RETRACT_END) {
    core->triggered_events |= COREEVENT_HOOK_RETRACT;
    core->hook_state = HOOK_RETRACTED;
  } else if (core->hook_state == HOOK_FLYING) {
    /* (the hook never went through a teleporter: the maps have no hook teleporters) */
    vec2 hook_base = core->pos;
    vec2 new_pos = vadd(core->hook_pos, vscale(core->hook_dir, ddnet_tune(tuning->hook_fire_speed)));
    if (vdistance(hook_base, new_pos) > ddnet_tune(tuning->hook_length)) {
      core->hook_state = HOOK_RETRACT_START;
      new_pos =
          vadd(hook_base, vscale(vnormalize(vsub(new_pos, hook_base)), ddnet_tune(tuning->hook_length)));
    }

    /* make sure that the hook doesn't go though the ground */
    bool going_to_hit_ground = false;
    bool going_to_retract = false;
    int hit = collision_intersect_line_tele_hook(col, false, core->hook_pos, new_pos, &new_pos, NULL, NULL);

    if (hit) {
      if (hit == TILE_NOHOOK)
        going_to_retract = true;
      else
        going_to_hit_ground = true;
    }

    /* (the check against other players: there are none) */

    if (core->hook_state == HOOK_FLYING) {
      /* check against ground */
      if (going_to_hit_ground) {
        core->triggered_events |= COREEVENT_HOOK_ATTACH_GROUND;
        core->hook_state = HOOK_GRABBED;
      } else if (going_to_retract) {
        core->triggered_events |= COREEVENT_HOOK_HIT_NOHOOK;
        core->hook_state = HOOK_RETRACT_START;
      }

      core->hook_pos = new_pos;
    }
  }

  if (core->hook_state == HOOK_GRABBED) {
    /* (the hook is never on a player) */
    if (vdistance(core->hook_pos, core->pos) > 46.0f) {
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
      if (new_vel_length < ddnet_tune(tuning->hook_drag_speed) || new_vel_length < vlength(core->vel)) {
        core->vel = new_vel; /* no problem. apply */
        core->hook_accel = hook_vel;
      }
    }

    /* release hook (max default hook time is 1.25 s) */
    core->hook_tick++;
  }

  /* (sv_no_weak_hook only changes the order of the tees) */
  core_tick_deferred(w, core);
}

/* CCharacterCore::TickDeferred: the forces between tees, of which there are none. */
void core_tick_deferred(world_t *w, core_t *core) {
  (void)w;
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

  /* (the check against the other players: the tee is solo) */

  core->pos = new_pos;
}

/* CCharacterCore::Quantize: the server rounds the core to what fits into a
 * snapshot at the end of every tick (Write() followed by Read()). */
void core_quantize(core_t *core) {
  core->pos.x = round_to_int(core->pos.x);
  core->pos.y = round_to_int(core->pos.y);
  core->vel.x = round_to_int(core->vel.x * 256.0f) / 256.0f;
  core->vel.y = round_to_int(core->vel.y * 256.0f) / 256.0f;
  core->hook_pos.x = round_to_int(core->hook_pos.x);
  core->hook_pos.y = round_to_int(core->hook_pos.y);
  core->hook_dir.x = round_to_int(core->hook_dir.x * 256.0f) / 256.0f;
  core->hook_dir.y = round_to_int(core->hook_dir.y * 256.0f) / 256.0f;
}
