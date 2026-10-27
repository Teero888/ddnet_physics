/* Port of CCharacter from src/game/server/entities/character.cpp of Unique
 * Race: everything the tee does on top of its core: weapons, damage, the
 * special tiles the rules allow, teleporters. */
#include "internal.h"

#include <string.h>

static tuning_t *get_tuning(world_t *w, int zone) { return &tuning_list(w)[zone]; }

/* --------------------------------------------------------------- helpers */

/* CCharacter::SetWeapon */
void character_set_weapon(world_t *w, character_t *chr, int weapon, bool silent) {
  if (weapon == chr->core.active_weapon)
    return;

  chr->last_weapon = chr->core.active_weapon;
  chr->queued_weapon = -1;
  chr->core.active_weapon = weapon;
  if (!silent)
    create_sound(w, chr->pos, DDNET_SOUND_WEAPON_SWITCH);

  if (chr->core.active_weapon < 0 || chr->core.active_weapon >= NUM_WEAPONS)
    chr->core.active_weapon = 0;
}

bool character_increase_health(character_t *chr, int amount) {
  if (chr->health >= 10)
    return false;
  chr->health = clampi(chr->health + amount, 0, 10);
  return true;
}

bool character_increase_armor(character_t *chr, int amount) {
  if (chr->armor >= 10)
    return false;
  chr->armor = clampi(chr->armor + amount, 0, 10);
  return true;
}

/* CCharacter::TakeDamage of Unique Race: the force is applied as it is (not
 * clamped by the stoppers, unlike DDNet), and with sv_health_and_ammo the
 * damage counts. Only the tee's own grenades hit it. */
bool character_take_damage(world_t *w, character_t *chr, vec2 force, int dmg, int weapon) {
  vec2 temp = vadd(chr->core.vel, force);
  chr->core.vel = temp;

  if (w->config.sv_health_and_ammo) {
    /* m_pPlayer only inflicts half damage on self */
    dmg = maxi(1, dmg / 2);

    chr->damage_taken++;

    /* create healthmod indicator */
    if (server_tick(w) < chr->damage_taken_tick + 25) {
      /* make sure that the damage indicators doesn't group together */
      create_damage_ind(w, chr->pos, chr->damage_taken * 0.25f, dmg);
    } else {
      chr->damage_taken = 0;
      create_damage_ind(w, chr->pos, 0, dmg);
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

    chr->damage_taken_tick = server_tick(w);

    /* check for death */
    if (chr->health <= 0) {
      character_die(w, chr, weapon);
      return false;
    }

    if (dmg > 2)
      create_sound(w, chr->pos, DDNET_SOUND_PLAYER_PAIN_LONG);
    else
      create_sound(w, chr->pos, DDNET_SOUND_PLAYER_PAIN_SHORT);
  }

  return true;
}

static void character_apply_move_restrictions(character_t *chr) {
  chr->core.vel = clamp_vel(chr->move_restrictions, chr->core.vel);
}

/* CCharacter::ResetHook */
static void character_reset_hook(character_t *chr) {
  chr->core.hook_state = HOOK_RETRACTED;
  chr->core.triggered_events |= COREEVENT_HOOK_RETRACT;
  chr->core.hook_pos = chr->core.pos;
}

/* ---------------------------------------------------------------- freeze */

/* CCharacter::Freeze(int Seconds) */
static bool character_freeze_seconds(world_t *w, character_t *chr, int seconds) {
  if (seconds <= 0 || chr->freeze_time > seconds * SERVER_TICK_SPEED)
    return false;
  if (chr->freeze_time == 0 || chr->core.freeze_start < server_tick(w) - SERVER_TICK_SPEED) {
    if (!w->config.sv_health_and_ammo)
      chr->armor = 0;
    chr->freeze_time = seconds * SERVER_TICK_SPEED;
    chr->core.freeze_start = server_tick(w);
    return true;
  }
  return false;
}

bool character_freeze(world_t *w, character_t *chr) { return character_freeze_seconds(w, chr, SV_FREEZE_DELAY); }

/* (CCharacter::UnFreeze does nothing in Unique Race.) */

/* --------------------------------------------------------------- weapons */

/* CCharacter::GiveWeapon, which Unique Race only calls to give */
void character_give_weapon(world_t *w, character_t *chr, int weapon) {
  if (weapon == WEAPON_GRENADE && w->config.sv_health_and_ammo)
    chr->core.weapons[weapon].ammo = GRENADE_MAX_AMMO;
  else
    chr->core.weapons[weapon].ammo = -1;

  chr->core.weapons[weapon].got = true;
}

/* CCharacter::DoWeaponSwitch */
static void character_do_weapon_switch(world_t *w, character_t *chr) {
  /* make sure we can switch */
  if (chr->reload_timer != 0 || chr->queued_weapon == -1)
    return;
  if (chr->core.weapons[WEAPON_NINJA].got || !chr->core.weapons[chr->queued_weapon].got)
    return;

  /* switch Weapon */
  character_set_weapon(w, chr, chr->queued_weapon, false);
}

/* CCharacter::HandleWeaponSwitch */
static void character_handle_weapon_switch(world_t *w, character_t *chr) {
  int wanted_weapon = chr->core.active_weapon;
  if (chr->queued_weapon != -1)
    wanted_weapon = chr->queued_weapon;

  bool anything = false;
  for (int i = 0; i < NUM_WEAPONS - 1; ++i)
    if (chr->core.weapons[i].got)
      anything = true;
  if (!anything)
    return;
  /* select Weapon */
  int next = count_input(chr->latest_prev_input.next_weapon, chr->latest_input.next_weapon).presses;
  int prev = count_input(chr->latest_prev_input.prev_weapon, chr->latest_input.prev_weapon).presses;

  if (next < 128) { /* make sure we only try sane stuff */
    while (next) {  /* Next Weapon selection */
      wanted_weapon = (wanted_weapon + 1) % NUM_WEAPONS;
      if (chr->core.weapons[wanted_weapon].got)
        next--;
    }
  }

  if (prev < 128) { /* make sure we only try sane stuff */
    while (prev) {  /* Prev Weapon selection */
      wanted_weapon = (wanted_weapon - 1) < 0 ? NUM_WEAPONS - 1 : wanted_weapon - 1;
      if (chr->core.weapons[wanted_weapon].got)
        prev--;
    }
  }

  /* Direct Weapon selection (the value really is taken from the other input) */
  if (chr->latest_input.wanted_weapon)
    wanted_weapon = chr->input.wanted_weapon - 1;

  /* check for insane values */
  if (wanted_weapon >= 0 && wanted_weapon < NUM_WEAPONS && wanted_weapon != chr->core.active_weapon &&
      chr->core.weapons[wanted_weapon].got)
    chr->queued_weapon = wanted_weapon;

  character_do_weapon_switch(w, chr);
}

/* CCharacter::FireWeapon */
static void character_fire_weapon(world_t *w, character_t *chr) {
  if (chr->reload_timer != 0)
    return;

  character_do_weapon_switch(w, chr);
  vec2 mouse_target = v2(chr->latest_input.target_x, chr->latest_input.target_y);
  vec2 direction = vnormalize(mouse_target);

  bool full_auto = false;
  if (chr->core.active_weapon == WEAPON_GRENADE || chr->core.active_weapon == WEAPON_SHOTGUN ||
      chr->core.active_weapon == WEAPON_LASER)
    full_auto = true;
  /* (a frozen tee is never unfrozen early, which would make the next shot full auto) */

  /* check if we gonna fire */
  bool will_fire = false;
  if (count_input(chr->latest_prev_input.fire, chr->latest_input.fire).presses)
    will_fire = true;

  if (full_auto && (chr->latest_input.fire & 1) && chr->core.weapons[chr->core.active_weapon].ammo)
    will_fire = true;

  if (!will_fire)
    return;

  if (chr->freeze_time) {
    /* Timer stuff to avoid shrieking orchestra caused by unfreeze-plasma */
    if (chr->pain_sound_timer <= 0 && !(chr->latest_prev_input.fire & 1)) {
      chr->pain_sound_timer = 1 * SERVER_TICK_SPEED;
      create_sound(w, chr->pos, DDNET_SOUND_PLAYER_PAIN_LONG);
    }
    return;
  }

  /* check for ammo */
  if (!chr->core.weapons[chr->core.active_weapon].ammo)
    return;

  vec2 proj_start_pos = vadd(chr->pos, vscale(vscale(direction, PHYSICAL_SIZE), 0.75f));

  switch (chr->core.active_weapon) {
  case WEAPON_HAMMER:
    create_sound(w, chr->pos, DDNET_SOUND_HAMMER_FIRE);
    /* (the hammer only hits other tees, so it never hits, and the reload
     * of a hit never applies) */
    break;

  case WEAPON_GUN:
    /* (the bullet hits nothing the tee could feel and is left out) */
    create_sound(w, chr->pos, DDNET_SOUND_GUN_FIRE);
    break;

  case WEAPON_GRENADE: {
    int lifetime = (int)(SERVER_TICK_SPEED * ddnet_tune(get_tuning(w, chr->tune_zone)->grenade_lifetime));

    projectile_create(w, proj_start_pos, direction, lifetime);

    create_sound(w, chr->pos, DDNET_SOUND_GRENADE_FIRE);
  } break;
  }

  chr->attack_tick = server_tick(w);

  if (w->config.sv_health_and_ammo && chr->core.weapons[chr->core.active_weapon].ammo > 0) /* -1 == unlimited */
    chr->core.weapons[chr->core.active_weapon].ammo--;

  if (!chr->reload_timer) {
    /* the fire delay tuning of the weapon, read as a float */
    float fire_delay;
    ddnet_tuning_get(get_tuning(w, chr->tune_zone),
                     (int)(offsetof(ddnet_tuning_t, hammer_fire_delay) / sizeof(ddnet_tune_param_t)) +
                         chr->core.active_weapon,
                     &fire_delay);
    chr->reload_timer = (int)(fire_delay * SERVER_TICK_SPEED / 1000);
  }
}

/* CCharacter::HandleWeapons */
static void character_handle_weapons(world_t *w, character_t *chr) {
  /* (no ninja, no jetpack) */

  if (chr->pain_sound_timer > 0)
    chr->pain_sound_timer--;

  /* check reload timer */
  if (chr->reload_timer) {
    chr->reload_timer--;
    return;
  }

  /* fire Weapon, if wanted */
  character_fire_weapon(w, chr);
}

/* ----------------------------------------------------------------- input */

/* CCharacter::OnPredictedInput */
void character_on_predicted_input(world_t *w, character_t *chr, const input_t *new_input) {
  (void)w;
  /* copy new input */
  chr->input = *new_input;

  /* it is not allowed to aim in the center */
  if (chr->input.target_x == 0 && chr->input.target_y == 0)
    chr->input.target_y = -1;

  chr->saved_input = chr->input;
}

/* CCharacter::OnDirectInput */
void character_on_direct_input(world_t *w, character_t *chr, const input_t *new_input) {
  chr->latest_prev_input = chr->latest_input;
  chr->latest_input = *new_input;
  chr->num_inputs++;

  /* it is not allowed to aim in the center */
  if (chr->latest_input.target_x == 0 && chr->latest_input.target_y == 0)
    chr->latest_input.target_y = -1;

  if (chr->num_inputs > 1) {
    character_handle_weapon_switch(w, chr);
    character_fire_weapon(w, chr);
  }

  chr->latest_prev_input = chr->latest_input;
}

/* ----------------------------------------------------------------- tiles */

/* True if one of the four sensor points of the tee touches a death tile. */
static bool character_touches_death_tile(const world_t *w, const character_t *chr) {
  const collision_t *col = collision(w);
  vec2 pos = chr->pos;
  return collision_get_collision_at(col, pos.x + PHYSICAL_SIZE / 3.f, pos.y - PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_collision_at(col, pos.x + PHYSICAL_SIZE / 3.f, pos.y + PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_collision_at(col, pos.x - PHYSICAL_SIZE / 3.f, pos.y - PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_collision_at(col, pos.x - PHYSICAL_SIZE / 3.f, pos.y + PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_front_collision_at(col, pos.x + PHYSICAL_SIZE / 3.f, pos.y - PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_front_collision_at(col, pos.x + PHYSICAL_SIZE / 3.f, pos.y + PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_front_collision_at(col, pos.x - PHYSICAL_SIZE / 3.f, pos.y - PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH ||
         collision_get_front_collision_at(col, pos.x - PHYSICAL_SIZE / 3.f, pos.y + PHYSICAL_SIZE / 3.f) ==
             TILE_DEATH;
}

/* CCharacter::HandleSkippableTiles: death tiles and speedups, which are only
 * checked at the final position of a tick. */
static void character_handle_skippable_tiles(world_t *w, character_t *chr, int index) {
  const collision_t *col = collision(w);

  /* handle death-tiles and leaving gamelayer (a solo tee never counts as
   * finished in its team) */
  if (character_touches_death_tile(w, chr)) {
    character_die(w, chr, WEAPON_WORLD);
    return;
  }

  if (game_layer_clipped(w, chr->pos)) {
    character_die(w, chr, WEAPON_WORLD);
    return;
  }

  if (index < 0)
    return;

  /* handle speedup tiles */
  if (collision_is_speedup(col, index)) {
    vec2 direction, temp_vel = chr->core.vel;
    int force, type, max_speed = 0;
    collision_get_speedup(col, index, &direction, &force, &max_speed, &type);

    if (type == TILE_SPEED_BOOST_OLD) {
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

          /* std::pow(float, int) and the sqrt around it are evaluated in double */
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
    } else if (type == TILE_SPEED_BOOST) {
      const float max_speed_scale = 5.0f;
      if (max_speed == 0) {
        const tuning_t *tuning = get_tuning(w, chr->tune_zone);
        /* log is the double version here */
        float max_ramp_speed = ddnet_tune(tuning->velramp_range) /
                               (50 * log(maxf((float)ddnet_tune(tuning->velramp_curvature), 1.01f)));
        max_speed = (int)(maxf(max_ramp_speed, ddnet_tune(tuning->velramp_start) / 50) * max_speed_scale);
      }

      /* (signed) length of projection */
      float current_directional_speed = vdot(direction, chr->core.vel);
      float temp_max_speed = max_speed / max_speed_scale;
      if (current_directional_speed + force > temp_max_speed)
        temp_vel = vadd(temp_vel, vscale(direction, (temp_max_speed - current_directional_speed)));
      else
        temp_vel = vadd(temp_vel, vscale(direction, force));

      chr->core.vel = clamp_vel(chr->move_restrictions, temp_vel);
    }
  }
}

/* The time Unique Race adds to the race for going through a teleporter:
 * the rest of the tick and a tick more. */
static void character_tele_time(character_t *chr, float fraction_of_tick) {
  chr->start_time -= 1;
  chr->start_time_offset -= fraction_of_tick;
}

/* CCharacter::HandleTiles: the effect of the tile with a map index. Called for
 * every tile the tee passed through during the tick, with the fraction of the
 * tick at which the tee reached it. */
static void character_handle_tiles(world_t *w, character_t *chr, int index, float fraction_of_tick) {
  const collision_t *col = collision(w);
  core_t *core = &chr->core;

  /* handle tiles more precise */
  chr->time = (fraction_of_tick - chr->start_time_offset + (float)(server_tick(w)) - chr->start_time) /
              ((float)SERVER_TICK_SPEED);

  int map_index = index;
  chr->move_restrictions = collision_get_move_restrictions(col, NULL, NULL, chr->pos, 18.0f, map_index);
  if (index < 0)
    return;
  /* (the time checkpoints are only shown) */
  int tele_checkpoint = collision_is_tele_checkpoint(col, map_index);
  if (tele_checkpoint)
    chr->tele_checkpoint = tele_checkpoint;

  controller_handle_character_tiles(w, chr, index, fraction_of_tick);
  if (!chr->alive)
    return;

  /* (freeze, the other special tiles and the switch layer are not in the
   * maps of Unique Race) */

  /* stopper */
  if (core->vel.y > 0 && (chr->move_restrictions & DDNET_CANTMOVE_DOWN)) {
    core->jumped = 0;
    core->jumped_total = 0;
  }
  character_apply_move_restrictions(chr);

  /* teleporters */
  /* (sv_old_teleport_hook, sv_old_teleport_weapons, sv_teleport_hold_hook and
   * sv_teleport_lose_weapons are 0) */
  int z = collision_is_teleport(col, map_index);
  if (z && col->tele_outs[z - 1].count != 0) {
    int tele_out = world_pick_tele_out(w, col->tele_outs[z - 1].count);
    core->pos = col->tele_outs[z - 1].positions[tele_out];
    character_tele_time(chr, fraction_of_tick);
    character_reset_hook(chr);
    return;
  }
  const int evil_teleport = collision_is_evil_teleport(col, map_index);
  if (evil_teleport && col->tele_outs[evil_teleport - 1].count != 0) {
    int tele_out = world_pick_tele_out(w, col->tele_outs[evil_teleport - 1].count);
    core->pos = col->tele_outs[evil_teleport - 1].positions[tele_out];
    core->vel = v2(0, 0);
    character_tele_time(chr, fraction_of_tick);
    character_reset_hook(chr);
    return;
  }
  if (collision_is_check_evil_teleport(col, map_index)) {
    /* first check if there is a TeleCheckOut for the current recorded checkpoint, if not check previous
     * checkpoints */
    for (int k = chr->tele_checkpoint - 1; k >= 0; k--) {
      if (col->tele_check_outs[k].count != 0) {
        int tele_out = world_pick_tele_out(w, col->tele_check_outs[k].count);
        core->pos = col->tele_check_outs[k].positions[tele_out];
        core->vel = v2(0, 0);
        character_tele_time(chr, fraction_of_tick);

        character_reset_hook(chr);

        return;
      }
    }
    /* if no checkpointout have been found (or if there no recorded checkpoint), teleport to start */
    vec2 spawn_pos;
    if (controller_can_spawn(w, &spawn_pos)) {
      core->pos = spawn_pos;
      core->vel = v2(0, 0);

      character_reset_hook(chr);
    }
    return;
  }
  if (collision_is_check_teleport(col, map_index)) {
    /* first check if there is a TeleCheckOut for the current recorded checkpoint, if not check previous
     * checkpoints */
    for (int k = chr->tele_checkpoint - 1; k >= 0; k--) {
      if (col->tele_check_outs[k].count != 0) {
        int tele_out = world_pick_tele_out(w, col->tele_check_outs[k].count);
        core->pos = col->tele_check_outs[k].positions[tele_out];
        character_tele_time(chr, fraction_of_tick);

        character_reset_hook(chr);

        return;
      }
    }
    /* if no checkpointout have been found (or if there no recorded checkpoint), teleport to start */
    vec2 spawn_pos;
    if (controller_can_spawn(w, &spawn_pos)) {
      core->pos = spawn_pos;

      character_reset_hook(chr);
    }
    return;
  }
}

/* CCharacter::HandleTuneLayer */
static void character_handle_tune_layer(world_t *w, character_t *chr) {
  int current_index = collision_get_map_index(collision(w), chr->pos);
  chr->tune_zone = collision_is_tune(collision(w), current_index);
  chr->core.tuning = tuning_list(w)[chr->tune_zone]; /* throw tunings from specific zone into gamecore */
}

/* ------------------------------------------------------------------ tick */

/* CCharacter::DDRaceTick: runs before the core. */
static void character_ddrace_tick(world_t *w, character_t *chr) {
  chr->input = chr->saved_input;
  controller_set_armor_progress(w, chr, chr->freeze_time);

  if (chr->freeze_time > 0) {
    chr->freeze_time--;
    chr->input.direction = 0;
    chr->input.jump = 0;
    chr->input.hook = 0;
    /* (CCharacter::UnFreeze on the last tick does nothing in Unique Race) */
  }

  character_handle_tune_layer(w, chr); /* need this before coretick */
}

/* The flags of fastcap (CCharacter::DDRacePostCoreTick): touching the first
 * starts the race, touching the other one finishes it, with the fraction of
 * the tick at which the tee reached them. */
static void character_fastcap_flags(world_t *w, character_t *chr) {
  float d = vdistance(chr->prev_pos, chr->pos);
  if (d) {
    int end = (int)(d + 1);
    for (int i = 0; i < end; i++) {
      float fraction_of_tick = i / d;
      vec2 pos = vmix(chr->prev_pos, chr->pos, fraction_of_tick);

      for (int check_team = 0; check_team <= TEAM_BLUE; ++check_team) {
        int opposite_team = check_team == TEAM_RED ? TEAM_BLUE : TEAM_RED;
        if (!chr->got_flag[check_team] && vdistance(pos, w->flag_pos[check_team]) < 14 + PHYSICAL_SIZE) {
          if (!chr->got_flag[opposite_team]) {
            teams_on_character_start(w, chr, fraction_of_tick);
            create_sound(w, chr->pos, DDNET_SOUND_CTF_GRAB_PL);
          } else {
            teams_on_character_finish(w, chr, fraction_of_tick);
            create_sound(w, chr->pos, DDNET_SOUND_CTF_CAPTURE);
          }
          chr->got_flag[check_team] = true;
        }
      }
    }
  }
}

/* CCharacter::DDRacePostCoreTick: runs after the core and the weapons. */
static void character_ddrace_post_core_tick(world_t *w, character_t *chr) {
  const collision_t *col = collision(w);
  core_t *core = &chr->core;

  /* (the race time is taken in character_handle_tiles) */

  /* following jump rules can be overridden by tiles, like Refill Jumps, Stopper and Wall Jump */
  if (core->jumps == -1) {
    /* The player has only one ground jump, so their feet are always dark */
    core->jumped |= 2;
  } else if (core->jumps == 0) {
    /* The player has no jumps at all, so their feet are always dark */
    core->jumped |= 2;
  } else if (core->jumps == 1 && core->jumped > 0) {
    /* If the player has only one jump, each jump is the last one */
    core->jumped |= 2;
  } else if (core->jumped_total < core->jumps - 1 && core->jumped > 1) {
    /* The player has not yet used up all their jumps, so their feet remain light */
    core->jumped = 1;
  }

  int current_index = collision_get_map_index(col, chr->pos);
  character_handle_skippable_tiles(w, chr, current_index);
  if (!chr->alive)
    return;

  if (w->config.gamemode != DDNET_MODE_UNIQUE_RACE)
    character_fastcap_flags(w, chr);

  /* handle Anti-Skip tiles */
  map_indices_with_time_t indices = collision_map_indices_with_time(col, chr->prev_pos, chr->pos);
  int index;
  float fraction_of_tick;
  bool any_index = false;
  while (collision_map_indices_with_time_next(&indices, &index, &fraction_of_tick)) {
    any_index = true;
    character_handle_tiles(w, chr, index, fraction_of_tick);
    if (!chr->alive)
      return;
  }
  if (!any_index) {
    character_handle_tiles(w, chr, current_index, 1.0f);
    if (!chr->alive)
      return;
  }
}

/* CCharacter::PreTick */
static void character_pre_tick(world_t *w, character_t *chr) {
  if (chr->start_time > server_tick(w)) {
    /* Prevent the player from getting a negative time
     * The main reason why this can happen is because of time penalty tiles
     * However, other reasons are hereby also excluded */
    character_die(w, chr, WEAPON_WORLD);
  }

  character_ddrace_tick(w, chr);

  chr->core.input = chr->input;
  core_tick(w, &chr->core, true);
}

/* CCharacter::Tick */
void character_tick(world_t *w, character_t *chr) {
  character_pre_tick(w, chr);

  /* handle Weapons */
  character_handle_weapons(w, chr);

  character_ddrace_post_core_tick(w, chr);

  /* Previnput */
  chr->prev_input = chr->input;

  chr->prev_pos = chr->core.pos;
}

/* CCharacter::TickDeferred */
void character_tick_deferred(world_t *w, character_t *chr) {
  core_move(w, &chr->core);
  core_quantize(&chr->core);
  chr->pos = chr->core.pos;

  /* the sounds of the events of the core */
  const int events = chr->core.triggered_events;
  if (events & COREEVENT_GROUND_JUMP)
    create_sound(w, chr->pos, DDNET_SOUND_PLAYER_JUMP);
  if (events & COREEVENT_HOOK_ATTACH_GROUND)
    create_sound(w, chr->pos, DDNET_SOUND_HOOK_ATTACH_GROUND);
  if (events & COREEVENT_HOOK_HIT_NOHOOK)
    create_sound(w, chr->pos, DDNET_SOUND_HOOK_NOATTACH);
  /* (and the effect a client makes of a double jump, which the server leaves to it) */
  if (events & COREEVENT_AIR_JUMP)
    create_particle(w, chr->pos, DDNET_PARTICLE_AIR_JUMP);
}

/* ---------------------------------------------------------- life & death */

/* CCharacter::DDRaceInit */
static void character_ddrace_init(world_t *w, character_t *chr) {
  (void)w;
  chr->race_state = DDNET_RACE_NONE;
  chr->prev_pos = chr->pos;

  chr->tele_checkpoint = 0;
  chr->core.jumps = 2;

  chr->got_flag[TEAM_RED] = false;
  chr->got_flag[TEAM_BLUE] = false;
}

/* CCharacter::CCharacter and CCharacter::Spawn */
void character_spawn(world_t *w, vec2 pos) {
  character_t *chr = &w->character;
  player_t *player = &w->player;

  /* The server allocates characters from a zeroed pool. */
  memset(chr, 0, sizeof(*chr));

  /* constructor: the character starts with the last input of the player */
  chr->input = player->input;
  /* never initialize both to zero */
  chr->input.target_x = 0;
  chr->input.target_y = -1;

  chr->latest_prev_input = chr->latest_input = chr->prev_input = chr->saved_input = chr->input;

  chr->reload_timer = SV_SPAWN_RELOAD_DELAY;

  /* Spawn */
  chr->last_weapon = WEAPON_HAMMER;
  chr->queued_weapon = -1;

  chr->pos = pos;

  chr->num_inputs = 0;
  chr->spawn_tick = server_tick(w);

  core_reset(&chr->core);
  chr->core.active_weapon = WEAPON_GUN;
  chr->core.pos = chr->pos;
  int tune_zone = collision_is_tune(collision(w), collision_get_map_index(collision(w), pos));
  chr->core.tuning = tuning_list(w)[tune_zone];

  chr->health = 10;
  if (w->config.sv_health_and_ammo && w->config.gamemode == DDNET_MODE_UNIQUE_RACE)
    chr->armor = 0;
  else
    chr->armor = 10;
  if (w->config.gamemode == DDNET_MODE_UNIQUE_FASTCAP) {
    character_give_weapon(w, chr, WEAPON_GRENADE);
    character_set_weapon(w, chr, WEAPON_GRENADE, true);
  }

  player->has_character = true;
  chr->alive = true;

  controller_on_character_spawn(w, chr);

  character_ddrace_init(w, chr);

  chr->tune_zone = tune_zone;
}

/* CCharacter::Die */
void character_die(world_t *w, character_t *chr, int weapon) {
  (void)weapon;
  player_t *player = &w->player;

  /* a nice sound, and bursting tee death effect */
  create_sound(w, chr->pos, DDNET_SOUND_PLAYER_DIE);
  create_particle(w, chr->pos, DDNET_PARTICLE_PLAYER_DEATH);

  /* this is to rate limit respawning to 3 secs */
  player->previous_die_tick = player->die_tick;
  player->die_tick = server_tick(w);

  chr->alive = false;

  /* (CGameTeams::OnCharacterDeath: the team of the tee is open to start again) */
}
