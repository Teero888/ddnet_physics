/* Port of CCharacter from DDNet's src/game/server/entities/character.cpp:
 * everything a tee does on top of its core: weapons, freeze, special tiles,
 * teleporters. */
#include "internal.h"

#include <string.h>

static tuning_t *get_tuning(world_t *w, int zone) { return &tuning_list(w)[zone]; }

static player_t *player_of(world_t *w, character_t *chr) { return &w->players[character_id(chr)]; }

/* --------------------------------------------------------------- helpers */

int character_team(const world_t *w, const character_t *chr) { return teams_team(w, character_id(chr)); }

bool character_can_collide(const world_t *w, const character_t *chr, int client_id) {
  return teams_can_collide(w, character_id(chr), client_id);
}

void character_set_solo(world_t *w, character_t *chr, bool solo) {
  chr->core.solo = solo;
  w->teams.is_solo[character_id(chr)] = solo;
}

/* CCharacter::SetWeapon */
static void character_set_weapon(world_t *w, character_t *chr, int weapon) {
  if (weapon == chr->core.active_weapon)
    return;

  chr->last_weapon = chr->core.active_weapon;
  chr->queued_weapon = -1;
  chr->core.active_weapon = weapon;
  create_sound(w, chr->pos, DDNET_SOUND_WEAPON_SWITCH, character_id(chr));

  if (chr->core.active_weapon < 0 || chr->core.active_weapon >= NUM_WEAPONS)
    chr->core.active_weapon = 0;
}

bool character_increase_health(character_t *chr, int amount) {
  if (chr->health >= 10)
    return false;
  chr->health = clampi(chr->health + amount, 0, 10);
  return true;
}

/* CCharacter::TakeDamage. Nothing takes damage in DDRace, only the force is applied. */
bool character_take_damage(world_t *w, character_t *chr, vec2 force, int dmg, int from, int weapon) {
  (void)w, (void)dmg, (void)from, (void)weapon;
  vec2 temp = vadd(chr->core.vel, force);
  chr->core.vel = clamp_vel(chr->move_restrictions, temp);
  return true;
}

void character_set_velocity(character_t *chr, vec2 new_velocity) {
  chr->core.vel = clamp_vel(chr->move_restrictions, new_velocity);
}

/* The method is needed only to reproduce 'shotgun bug' ddnet#5258 */
void character_set_raw_velocity(character_t *chr, vec2 new_velocity) { chr->core.vel = new_velocity; }

void character_add_velocity(character_t *chr, vec2 addition) {
  character_set_velocity(chr, vadd(chr->core.vel, addition));
}

void character_apply_move_restrictions(character_t *chr) {
  chr->core.vel = clamp_vel(chr->move_restrictions, chr->core.vel);
}

void character_release_hook(world_t *w, character_t *chr) {
  core_set_hooked_player(w, &chr->core, -1);
  chr->core.hook_state = HOOK_RETRACTED;
  chr->core.triggered_events |= COREEVENT_HOOK_RETRACT;
}

static void character_reset_hook(world_t *w, character_t *chr) {
  character_release_hook(w, chr);
  chr->core.hook_pos = chr->core.pos;
}

/* ---------------------------------------------------------------- freeze */

/* CCharacter::Freeze(int Seconds) */
bool character_freeze_seconds(world_t *w, character_t *chr, int seconds) {
  if (seconds <= 0 || chr->freeze_time > seconds * SERVER_TICK_SPEED)
    return false;
  if (chr->freeze_time == 0 || chr->core.freeze_start < server_tick(w) - SERVER_TICK_SPEED) {
    chr->armor = 0;
    chr->freeze_time = seconds * SERVER_TICK_SPEED;
    chr->core.freeze_start = server_tick(w);
    return true;
  }
  return false;
}

bool character_freeze(world_t *w, character_t *chr) {
  return character_freeze_seconds(w, chr, w->config.sv_freeze_delay);
}

bool character_unfreeze(world_t *w, character_t *chr) {
  (void)w;
  if (chr->freeze_time > 0) {
    chr->armor = 10;
    if (chr->core.active_weapon >= 0 && !chr->core.weapons[chr->core.active_weapon].got)
      chr->core.active_weapon = WEAPON_GUN;
    chr->freeze_time = 0;
    chr->core.freeze_start = 0;
    chr->frozen_last_tick = true;
    return true;
  }
  return false;
}

/* --------------------------------------------------------------- weapons */

void character_give_ninja(world_t *w, character_t *chr) {
  chr->core.ninja.activation_tick = server_tick(w);
  chr->core.weapons[WEAPON_NINJA].got = true;
  chr->core.weapons[WEAPON_NINJA].ammo = -1;
  if (chr->core.active_weapon != WEAPON_NINJA)
    chr->last_weapon = chr->core.active_weapon;
  chr->core.active_weapon = WEAPON_NINJA;
}

static void character_remove_ninja(world_t *w, character_t *chr) {
  chr->core.ninja.activation_dir = v2(0, 0);
  chr->core.ninja.activation_tick = 0;
  chr->core.ninja.current_move_time = 0;
  chr->core.ninja.old_vel_amount = 0;
  chr->core.weapons[WEAPON_NINJA].got = false;
  chr->core.weapons[WEAPON_NINJA].ammo = 0;
  chr->core.active_weapon = chr->last_weapon;

  character_set_weapon(w, chr, chr->core.active_weapon);
}

void character_give_weapon(world_t *w, character_t *chr, int weapon, bool remove) {
  if (weapon == WEAPON_NINJA) {
    if (remove)
      character_remove_ninja(w, chr);
    else
      character_give_ninja(w, chr);
    return;
  }

  if (remove) {
    if (chr->core.active_weapon == weapon)
      chr->core.active_weapon = WEAPON_GUN;
  } else {
    chr->core.weapons[weapon].ammo = -1;
  }

  chr->core.weapons[weapon].got = !remove;
}

void character_reset_pickups(character_t *chr) {
  for (int i = WEAPON_SHOTGUN; i < NUM_WEAPONS - 1; i++) {
    chr->core.weapons[i].got = false;
    if (chr->core.active_weapon == i)
      chr->core.active_weapon = WEAPON_GUN;
  }
}

/* CCharacter::HandleJetpack */
static void character_handle_jetpack(world_t *w, character_t *chr) {
  if (chr->core.active_weapon < 0)
    return;

  vec2 direction = vnormalize(v2(chr->latest_input.target_x, chr->latest_input.target_y));

  bool full_auto = false;
  if (chr->core.active_weapon == WEAPON_GRENADE || chr->core.active_weapon == WEAPON_SHOTGUN ||
      chr->core.active_weapon == WEAPON_LASER)
    full_auto = true;
  if (chr->core.jetpack && chr->core.active_weapon == WEAPON_GUN)
    full_auto = true;

  /* check if we gonna fire */
  bool will_fire = false;
  if (count_input(chr->latest_prev_input.fire, chr->latest_input.fire).presses)
    will_fire = true;

  if (full_auto && (chr->latest_input.fire & 1) && chr->core.weapons[chr->core.active_weapon].ammo)
    will_fire = true;

  if (!will_fire)
    return;

  /* check for ammo */
  if (!chr->core.weapons[chr->core.active_weapon].ammo || chr->freeze_time)
    return;

  switch (chr->core.active_weapon) {
  case WEAPON_GUN: {
    if (chr->core.jetpack) {
      float strength = ddnet_tune(get_tuning(w, chr->tune_zone)->jetpack_strength);
      character_take_damage(w, chr, vscale(vscale(direction, -1.0f), (strength / 100.0f / 6.11f)), 0,
                            character_id(chr), chr->core.active_weapon);
    }
  }
  }
}

/* CCharacter::HandleNinja */
static void character_handle_ninja(world_t *w, character_t *chr) {
  if (chr->core.active_weapon != WEAPON_NINJA)
    return;

  if ((server_tick(w) - chr->core.ninja.activation_tick) > (NINJA_DURATION * SERVER_TICK_SPEED / 1000)) {
    /* time's up, return */
    character_remove_ninja(w, chr);
    return;
  }

  int ninja_time =
      chr->core.ninja.activation_tick + (NINJA_DURATION * SERVER_TICK_SPEED / 1000) - server_tick(w);

  controller_set_armor_progress(chr, ninja_time);

  /* force ninja Weapon */
  character_set_weapon(w, chr, WEAPON_NINJA);

  chr->core.ninja.current_move_time--;

  if (chr->core.ninja.current_move_time == 0) {
    /* reset velocity */
    chr->core.vel = vscale(chr->core.ninja.activation_dir, chr->core.ninja.old_vel_amount);
  }

  if (chr->core.ninja.current_move_time > 0) {
    /* Set velocity */
    chr->core.vel = vscale(chr->core.ninja.activation_dir, NINJA_VELOCITY);
    vec2 old_pos = chr->pos;
    vec2 ground_elasticity = v2(ddnet_tune(get_tuning(w, chr->tune_zone)->ground_elasticity_x),
                                ddnet_tune(get_tuning(w, chr->tune_zone)->ground_elasticity_y));

    collision_move_box(collision(w), &chr->core.pos, &chr->core.vel, v2(PHYSICAL_SIZE, PHYSICAL_SIZE),
                       ground_elasticity, NULL);

    /* reset velocity so the client doesn't predict stuff */
    chr->core.vel = v2(0, 0);

    /* check if we Hit anything along the way */
    {
      int ents[MAX_CLIENTS];
      float radius = PHYSICAL_SIZE * 2.0f;
      int num = world_find_characters(w, old_pos, radius, ents, MAX_CLIENTS);

      /* check that we're not in solo part */
      if (teams_get_solo(w, character_id(chr)))
        return;

      for (int i = 0; i < num; ++i) {
        character_t *other = &w->characters[ents[i]];
        if (other == chr)
          continue;

        /* Don't hit players in other teams */
        if (character_team(w, chr) != character_team(w, other))
          continue;

        const int client_id = character_id(other);

        /* Don't hit players in solo parts */
        if (teams_get_solo(w, client_id))
          continue;

        /* make sure we haven't Hit this object before */
        bool already_hit = false;
        for (int j = 0; j < chr->num_objects_hit; j++) {
          if (chr->hit_objects[j] == client_id)
            already_hit = true;
        }
        if (already_hit)
          continue;

        /* check so we are sufficiently close */
        if (vdistance(other->pos, chr->pos) > radius)
          continue;

        /* Hit a player, give them damage and stuffs... */
        create_sound(w, other->pos, DDNET_SOUND_NINJA_HIT, character_id(chr));
        chr->hit_objects[chr->num_objects_hit++] = client_id;

        other->hit_num += NINJA_DAMAGE;
        character_take_damage(w, other, v2(0, -10.0f), NINJA_DAMAGE, character_id(chr), WEAPON_NINJA);
      }
    }

    return;
  }
}

/* CCharacter::DoWeaponSwitch */
static void character_do_weapon_switch(world_t *w, character_t *chr) {
  /* make sure we can switch */
  if (chr->reload_timer != 0 || chr->queued_weapon == -1)
    return;
  if (chr->core.weapons[WEAPON_NINJA].got || !chr->core.weapons[chr->queued_weapon].got)
    return;

  /* switch Weapon */
  character_set_weapon(w, chr, chr->queued_weapon);
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

/* CTuningParams::GetWeaponFireDelay */
static float get_weapon_fire_delay(const tuning_t *tuning, int weapon) {
  switch (weapon) {
  case WEAPON_HAMMER:
    return (float)ddnet_tune(tuning->hammer_fire_delay) / 1000.0f;
  case WEAPON_GUN:
    return (float)ddnet_tune(tuning->gun_fire_delay) / 1000.0f;
  case WEAPON_SHOTGUN:
    return (float)ddnet_tune(tuning->shotgun_fire_delay) / 1000.0f;
  case WEAPON_GRENADE:
    return (float)ddnet_tune(tuning->grenade_fire_delay) / 1000.0f;
  case WEAPON_LASER:
    return (float)ddnet_tune(tuning->laser_fire_delay) / 1000.0f;
  case WEAPON_NINJA:
    return (float)ddnet_tune(tuning->ninja_fire_delay) / 1000.0f;
  default:
    return 0.0f;
  }
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
  if (chr->core.jetpack && chr->core.active_weapon == WEAPON_GUN)
    full_auto = true;
  /* allow firing directly after coming out of freeze or being unfrozen
   * by something */
  if (chr->frozen_last_tick)
    full_auto = true;

  /* don't fire hammer when player is deep and sv_deepfly is disabled */
  if (!w->config.sv_deepfly && chr->core.active_weapon == WEAPON_HAMMER && chr->core.deep_frozen)
    return;

  /* check if we gonna fire */
  bool will_fire = false;
  if (count_input(chr->latest_prev_input.fire, chr->latest_input.fire).presses)
    will_fire = true;

  if (full_auto && (chr->latest_input.fire & 1) && chr->core.active_weapon >= 0 &&
      chr->core.weapons[chr->core.active_weapon].ammo)
    will_fire = true;

  if (!will_fire)
    return;

  if (chr->freeze_time) {
    /* Timer stuff to avoid shrieking orchestra caused by unfreeze-plasma */
    if (chr->pain_sound_timer <= 0 && !(chr->latest_prev_input.fire & 1)) {
      chr->pain_sound_timer = 1 * SERVER_TICK_SPEED;
      create_sound(w, chr->pos, DDNET_SOUND_PLAYER_PAIN_LONG, character_id(chr));
    }
    return;
  }

  /* check for ammo */
  if (chr->core.active_weapon < 0 || !chr->core.weapons[chr->core.active_weapon].ammo)
    return;

  vec2 proj_start_pos = vadd(chr->pos, vscale(vscale(direction, PHYSICAL_SIZE), 0.75f));

  switch (chr->core.active_weapon) {
  case WEAPON_HAMMER: {
    create_sound(w, chr->pos, DDNET_SOUND_HAMMER_FIRE, character_id(chr));

    if (chr->core.hammer_hit_disabled)
      break;

    int ents[MAX_CLIENTS];
    int hits = 0;
    int num = world_find_characters(w, proj_start_pos, PHYSICAL_SIZE * 0.5f, ents, MAX_CLIENTS);

    for (int i = 0; i < num; ++i) {
      character_t *target = &w->characters[ents[i]];

      if ((target == chr || (target->alive && !character_can_collide(w, chr, character_id(target)))))
        continue;

      /* set their velocity to fast upward (for now) */
      if (vlength(vsub(target->pos, proj_start_pos)) > 0.0f)
        create_particle(
            w,
            vsub(target->pos,
                 vscale(vscale(vnormalize(vsub(target->pos, proj_start_pos)), PHYSICAL_SIZE), 0.5f)),
            DDNET_PARTICLE_HAMMER_HIT, character_id(chr));
      else
        create_particle(w, proj_start_pos, DDNET_PARTICLE_HAMMER_HIT, character_id(chr));

      vec2 dir;
      if (vlength(vsub(target->pos, chr->pos)) > 0.0f)
        dir = vnormalize(vsub(target->pos, chr->pos));
      else
        dir = v2(0.f, -1.f);

      float strength = ddnet_tune(get_tuning(w, chr->tune_zone)->hammer_strength);

      vec2 temp = vadd(target->core.vel, vscale(vnormalize(vadd(dir, v2(0.f, -1.1f))), 10.0f));
      temp = clamp_vel(target->move_restrictions, temp);
      temp = vsub(temp, target->core.vel);
      target->hit_num += HAMMER_DAMAGE;
      character_take_damage(w, target, vscale(vadd(v2(0.f, -1.0f), temp), strength), HAMMER_DAMAGE,
                            character_id(chr), chr->core.active_weapon);
      character_unfreeze(w, target);

      hits++;
    }

    /* if we Hit anything, we have to wait for the reload */
    if (hits) {
      float fire_delay = ddnet_tune(get_tuning(w, chr->tune_zone)->hammer_hit_fire_delay);
      chr->reload_timer = (int)(fire_delay * SERVER_TICK_SPEED / 1000);
    }
  } break;

  case WEAPON_GUN: {
    if (!chr->core.jetpack || !player_of(w, chr)->ninja_jetpack || chr->core.has_telegun_gun) {
      int lifetime = (int)(SERVER_TICK_SPEED * ddnet_tune(get_tuning(w, chr->tune_zone)->gun_lifetime));

      projectile_create(w, WEAPON_GUN, character_id(chr), proj_start_pos, direction, lifetime, false, false,
                        LAYER_GAME, 0, 0);

      create_sound(w, chr->pos, DDNET_SOUND_GUN_FIRE, character_id(chr));
    }
  } break;

  case WEAPON_SHOTGUN: {
    float laser_reach = ddnet_tune(get_tuning(w, chr->tune_zone)->laser_reach);

    laser_create(w, chr->pos, direction, laser_reach, character_id(chr), WEAPON_SHOTGUN);
    create_sound(w, chr->pos, DDNET_SOUND_SHOTGUN_FIRE, character_id(chr));
  } break;

  case WEAPON_GRENADE: {
    int lifetime = (int)(SERVER_TICK_SPEED * ddnet_tune(get_tuning(w, chr->tune_zone)->grenade_lifetime));

    projectile_create(w, WEAPON_GRENADE, character_id(chr), proj_start_pos, direction, lifetime, false, true,
                      LAYER_GAME, 0, 0);

    create_sound(w, chr->pos, DDNET_SOUND_GRENADE_FIRE, character_id(chr));
  } break;

  case WEAPON_LASER: {
    float laser_reach = ddnet_tune(get_tuning(w, chr->tune_zone)->laser_reach);

    laser_create(w, chr->pos, direction, laser_reach, character_id(chr), WEAPON_LASER);
    create_sound(w, chr->pos, DDNET_SOUND_LASER_FIRE, character_id(chr));
  } break;

  case WEAPON_NINJA: {
    /* reset Hit objects */
    chr->num_objects_hit = 0;

    chr->core.ninja.activation_dir = direction;
    chr->core.ninja.current_move_time = NINJA_MOVETIME * SERVER_TICK_SPEED / 1000;

    /* clamp to prevent massive MoveBox calculation lag with SG bug */
    chr->core.ninja.old_vel_amount = (int)clampf(vlength(chr->core.vel), 0.0f, 6000.0f);

    create_sound(w, chr->pos, DDNET_SOUND_NINJA_FIRE, character_id(chr));
  } break;
  }

  chr->attack_tick = server_tick(w);

  /* -1 is no weapon, handled here so pain sound still plays when firing in freeze */
  if (!chr->reload_timer && chr->core.active_weapon != -1) {
    chr->reload_timer = (int)(get_weapon_fire_delay(get_tuning(w, chr->tune_zone), chr->core.active_weapon) *
                              SERVER_TICK_SPEED);
  }
}

/* CCharacter::HandleWeapons */
static void character_handle_weapons(world_t *w, character_t *chr) {
  /* ninja */
  character_handle_ninja(w, chr);
  character_handle_jetpack(w, chr);

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

  /* handle death-tiles and leaving gamelayer */
  if (character_touches_death_tile(w, chr) &&
      !(character_team(w, chr) && w->teams.tee_finished[character_id(chr)])) {
    character_die(w, chr, character_id(chr), WEAPON_WORLD);
    return;
  }

  if (game_layer_clipped(w, chr->pos)) {
    character_die(w, chr, character_id(chr), WEAPON_WORLD);
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

/* CCharacter::IsSwitchActiveCb */
typedef struct switch_active_ctx_t {
  world_t *w;
  character_t *chr;
} switch_active_ctx_t;

static bool character_is_switch_active_cb(unsigned char number, void *user) {
  switch_active_ctx_t *ctx = user;
  world_t *w = ctx->w;
  return w->num_switchers != 0 && switchers(w)[number].status[character_team(w, ctx->chr)];
}

/* CCharacter::SetTimeCheckpoint */
static void character_set_time_checkpoint(character_t *chr, int time_checkpoint) {
  if (time_checkpoint > -1 && chr->race_state == DDNET_RACE_STARTED &&
      chr->current_time_cp[time_checkpoint] == 0.0f && chr->time != 0.0f) {
    chr->last_time_cp = time_checkpoint;
    chr->current_time_cp[chr->last_time_cp] = chr->time;
  }
}

/* Set the state of a switch for the team of a tee (the four cases of the
 * switch tiles in CCharacter::HandleTiles). */
static void character_set_switch(world_t *w, character_t *chr, int number, bool status, int end_tick,
                                 int type) {
  ddnet_switcher_t *switcher = &switchers(w)[number];
  int team = character_team(w, chr);
  switcher->status[team] = status;
  switcher->end_tick[team] = end_tick;
  switcher->type[team] = type;
  switcher->last_update_tick[team] = server_tick(w);
}

/* True if the switched tile the tee stands on is active for its team. */
static bool character_switch_applies(world_t *w, character_t *chr, int switch_number) {
  return switch_number == 0 || switchers(w)[switch_number].status[character_team(w, chr)];
}

/* The time penalty and bonus tiles also move the start time of the whole team. */
static void character_share_start_time(world_t *w, character_t *chr) {
  int team = teams_team(w, chr->core.id);
  if (w->config.sv_team == DDNET_SV_TEAM_FORCED_SOLO || team != TEAM_FLOCK) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (teams_team(w, i) == team && i != chr->core.id && w->players[i].active) {
        character_t *other = get_player_char(w, i);
        if (other)
          other->start_time = chr->start_time;
      }
    }
  }
}

/* CCharacter::HandleTiles: the effect of the tile with a map index. Called for
 * every tile the tee passed through during the tick. */
static void character_handle_tiles(world_t *w, character_t *chr, int index) {
  const collision_t *col = collision(w);
  const ddnet_config_t *config = &w->config;
  core_t *core = &chr->core;

  int map_index = index;
  chr->tile_index = collision_get_tile_index(col, map_index);
  chr->tile_f_index = collision_get_front_tile_index(col, map_index);
  switch_active_ctx_t ctx = {w, chr};
  chr->move_restrictions =
      collision_get_move_restrictions(col, character_is_switch_active_cb, &ctx, chr->pos, 18.0f, map_index);
  if (index < 0) {
    chr->last_refill_jumps = false;
    chr->last_penalty = false;
    chr->last_bonus = false;
    return;
  }
  character_set_time_checkpoint(chr, collision_is_time_checkpoint(col, map_index));
  character_set_time_checkpoint(chr, collision_is_front_time_checkpoint(col, map_index));
  int tele_checkpoint = collision_is_tele_checkpoint(col, map_index);
  if (tele_checkpoint)
    chr->tele_checkpoint = tele_checkpoint;

  controller_handle_character_tiles(w, chr, index);
  if (!chr->alive)
    return;

  const int tile = chr->tile_index;
  const int ftile = chr->tile_f_index;

  /* freeze */
  if (((tile == TILE_FREEZE) || (ftile == TILE_FREEZE)) && !core->deep_frozen)
    character_freeze(w, chr);
  else if (((tile == TILE_UNFREEZE) || (ftile == TILE_UNFREEZE)) && !core->deep_frozen)
    character_unfreeze(w, chr);

  /* deep freeze */
  if (((tile == TILE_DFREEZE) || (ftile == TILE_DFREEZE)) && !core->deep_frozen)
    core->deep_frozen = true;
  else if (((tile == TILE_DUNFREEZE) || (ftile == TILE_DUNFREEZE)) && core->deep_frozen)
    core->deep_frozen = false;

  /* live freeze */
  if (((tile == TILE_LFREEZE) || (ftile == TILE_LFREEZE)))
    core->live_frozen = true;
  else if (((tile == TILE_LUNFREEZE) || (ftile == TILE_LUNFREEZE)))
    core->live_frozen = false;

  /* endless hook */
  if (((tile == TILE_EHOOK_ENABLE) || (ftile == TILE_EHOOK_ENABLE)))
    core->endless_hook = true;
  else if (((tile == TILE_EHOOK_DISABLE) || (ftile == TILE_EHOOK_DISABLE)))
    core->endless_hook = false;

  /* hit others */
  if (((tile == TILE_HIT_DISABLE) || (ftile == TILE_HIT_DISABLE)) &&
      (!core->hammer_hit_disabled || !core->shotgun_hit_disabled || !core->grenade_hit_disabled ||
       !core->laser_hit_disabled)) {
    core->hammer_hit_disabled = true;
    core->shotgun_hit_disabled = true;
    core->grenade_hit_disabled = true;
    core->laser_hit_disabled = true;
  } else if (((tile == TILE_HIT_ENABLE) || (ftile == TILE_HIT_ENABLE)) &&
             (core->hammer_hit_disabled || core->shotgun_hit_disabled || core->grenade_hit_disabled ||
              core->laser_hit_disabled)) {
    core->shotgun_hit_disabled = false;
    core->grenade_hit_disabled = false;
    core->hammer_hit_disabled = false;
    core->laser_hit_disabled = false;
  }

  /* collide with others */
  if (((tile == TILE_NPC_DISABLE) || (ftile == TILE_NPC_DISABLE)) && !core->collision_disabled)
    core->collision_disabled = true;
  else if (((tile == TILE_NPC_ENABLE) || (ftile == TILE_NPC_ENABLE)) && core->collision_disabled)
    core->collision_disabled = false;

  /* hook others */
  if (((tile == TILE_NPH_DISABLE) || (ftile == TILE_NPH_DISABLE)) && !core->hook_hit_disabled)
    core->hook_hit_disabled = true;
  else if (((tile == TILE_NPH_ENABLE) || (ftile == TILE_NPH_ENABLE)) && core->hook_hit_disabled)
    core->hook_hit_disabled = false;

  /* unlimited air jumps */
  if (((tile == TILE_UNLIMITED_JUMPS_ENABLE) || (ftile == TILE_UNLIMITED_JUMPS_ENABLE)) &&
      !core->endless_jump)
    core->endless_jump = true;
  else if (((tile == TILE_UNLIMITED_JUMPS_DISABLE) || (ftile == TILE_UNLIMITED_JUMPS_DISABLE)) &&
           core->endless_jump)
    core->endless_jump = false;

  /* walljump */
  if ((tile == TILE_WALLJUMP) || (ftile == TILE_WALLJUMP)) {
    if (core->vel.y > 0 && core->colliding && core->left_wall) {
      core->left_wall = false;
      core->jumped_total = core->jumps >= 2 ? core->jumps - 2 : 0;
      core->jumped = 1;
    }
  }

  /* jetpack gun */
  if (((tile == TILE_JETPACK_ENABLE) || (ftile == TILE_JETPACK_ENABLE)) && !core->jetpack)
    core->jetpack = true;
  else if (((tile == TILE_JETPACK_DISABLE) || (ftile == TILE_JETPACK_DISABLE)) && core->jetpack)
    core->jetpack = false;

  /* refill jumps */
  if (((tile == TILE_REFILL_JUMPS) || (ftile == TILE_REFILL_JUMPS)) && !chr->last_refill_jumps) {
    core->jumped_total = 0;
    core->jumped = 0;
    chr->last_refill_jumps = true;
  }
  if ((tile != TILE_REFILL_JUMPS) && (ftile != TILE_REFILL_JUMPS))
    chr->last_refill_jumps = false;

  /* Teleport gun */
  if (((tile == TILE_TELE_GUN_ENABLE) || (ftile == TILE_TELE_GUN_ENABLE)) && !core->has_telegun_gun)
    core->has_telegun_gun = true;
  else if (((tile == TILE_TELE_GUN_DISABLE) || (ftile == TILE_TELE_GUN_DISABLE)) && core->has_telegun_gun)
    core->has_telegun_gun = false;

  if (((tile == TILE_TELE_GRENADE_ENABLE) || (ftile == TILE_TELE_GRENADE_ENABLE)) &&
      !core->has_telegun_grenade)
    core->has_telegun_grenade = true;
  else if (((tile == TILE_TELE_GRENADE_DISABLE) || (ftile == TILE_TELE_GRENADE_DISABLE)) &&
           core->has_telegun_grenade)
    core->has_telegun_grenade = false;

  if (((tile == TILE_TELE_LASER_ENABLE) || (ftile == TILE_TELE_LASER_ENABLE)) && !core->has_telegun_laser)
    core->has_telegun_laser = true;
  else if (((tile == TILE_TELE_LASER_DISABLE) || (ftile == TILE_TELE_LASER_DISABLE)) &&
           core->has_telegun_laser)
    core->has_telegun_laser = false;

  /* stopper */
  if (core->vel.y > 0 && (chr->move_restrictions & DDNET_CANTMOVE_DOWN)) {
    core->jumped = 0;
    core->jumped_total = 0;
  }
  character_apply_move_restrictions(chr);

  /* handle switch tiles */
  const int switch_type = collision_get_switch_type(col, map_index);
  const int switch_number = collision_get_switch_number(col, map_index);
  const int switch_delay = collision_get_switch_delay(col, map_index);
  if (switch_type == TILE_SWITCHOPEN && switch_number > 0) {
    character_set_switch(w, chr, switch_number, true, 0, TILE_SWITCHOPEN);
  } else if (switch_type == TILE_SWITCHTIMEDOPEN && switch_number > 0) {
    character_set_switch(w, chr, switch_number, true, server_tick(w) + 1 + switch_delay * SERVER_TICK_SPEED,
                         TILE_SWITCHTIMEDOPEN);
  } else if (switch_type == TILE_SWITCHTIMEDCLOSE && switch_number > 0) {
    character_set_switch(w, chr, switch_number, false, server_tick(w) + 1 + switch_delay * SERVER_TICK_SPEED,
                         TILE_SWITCHTIMEDCLOSE);
  } else if (switch_type == TILE_SWITCHCLOSE && switch_number > 0) {
    character_set_switch(w, chr, switch_number, false, 0, TILE_SWITCHCLOSE);
  } else if (switch_type == TILE_FREEZE) {
    if (character_switch_applies(w, chr, switch_number))
      character_freeze_seconds(w, chr, switch_delay);
  } else if (switch_type == TILE_DFREEZE) {
    if (character_switch_applies(w, chr, switch_number))
      core->deep_frozen = true;
  } else if (switch_type == TILE_DUNFREEZE) {
    if (character_switch_applies(w, chr, switch_number))
      core->deep_frozen = false;
  } else if (switch_type == TILE_LFREEZE) {
    if (character_switch_applies(w, chr, switch_number))
      core->live_frozen = true;
  } else if (switch_type == TILE_LUNFREEZE) {
    if (character_switch_applies(w, chr, switch_number))
      core->live_frozen = false;
  } else if (switch_type == TILE_HIT_ENABLE && core->hammer_hit_disabled && switch_delay == WEAPON_HAMMER) {
    core->hammer_hit_disabled = false;
  } else if (switch_type == TILE_HIT_DISABLE && !(core->hammer_hit_disabled) &&
             switch_delay == WEAPON_HAMMER) {
    core->hammer_hit_disabled = true;
  } else if (switch_type == TILE_HIT_ENABLE && core->shotgun_hit_disabled && switch_delay == WEAPON_SHOTGUN) {
    core->shotgun_hit_disabled = false;
  } else if (switch_type == TILE_HIT_DISABLE && !(core->shotgun_hit_disabled) &&
             switch_delay == WEAPON_SHOTGUN) {
    core->shotgun_hit_disabled = true;
  } else if (switch_type == TILE_HIT_ENABLE && core->grenade_hit_disabled && switch_delay == WEAPON_GRENADE) {
    core->grenade_hit_disabled = false;
  } else if (switch_type == TILE_HIT_DISABLE && !(core->grenade_hit_disabled) &&
             switch_delay == WEAPON_GRENADE) {
    core->grenade_hit_disabled = true;
  } else if (switch_type == TILE_HIT_ENABLE && core->laser_hit_disabled && switch_delay == WEAPON_LASER) {
    core->laser_hit_disabled = false;
  } else if (switch_type == TILE_HIT_DISABLE && !(core->laser_hit_disabled) && switch_delay == WEAPON_LASER) {
    core->laser_hit_disabled = true;
  } else if (switch_type == TILE_JUMP) {
    int new_jumps = switch_delay;
    if (new_jumps == 255)
      new_jumps = -1;

    if (new_jumps != core->jumps)
      core->jumps = new_jumps;
  } else if (switch_type == TILE_ADD_TIME && !chr->last_penalty) {
    const int minutes = switch_delay;
    const int seconds = switch_number;

    chr->start_time -= (minutes * 60 + seconds) * SERVER_TICK_SPEED;
    character_share_start_time(w, chr);

    chr->last_penalty = true;
  } else if (switch_type == TILE_SUBTRACT_TIME && !chr->last_bonus) {
    const int minutes = switch_delay;
    const int seconds = switch_number;

    chr->start_time += (minutes * 60 + seconds) * SERVER_TICK_SPEED;
    if (chr->start_time > server_tick(w))
      chr->start_time = server_tick(w);
    character_share_start_time(w, chr);

    chr->last_bonus = true;
  }

  if (switch_type != TILE_ADD_TIME)
    chr->last_penalty = false;

  if (switch_type != TILE_SUBTRACT_TIME)
    chr->last_bonus = false;

  /* teleporters */
  int z = collision_is_teleport(col, map_index);
  if (!config->sv_old_teleport_hook && !config->sv_old_teleport_weapons && z &&
      col->tele_outs[z - 1].count != 0) {
    int tele_out = world_pick_tele_out(w, character_id(chr), col->tele_outs[z - 1].count);
    core->pos = col->tele_outs[z - 1].positions[tele_out];
    if (!config->sv_teleport_hold_hook)
      character_reset_hook(w, chr);
    if (config->sv_teleport_lose_weapons)
      character_reset_pickups(chr);
    return;
  }
  const int evil_teleport = collision_is_evil_teleport(col, map_index);
  if (evil_teleport && col->tele_outs[evil_teleport - 1].count != 0) {
    int tele_out = world_pick_tele_out(w, character_id(chr), col->tele_outs[evil_teleport - 1].count);
    core->pos = col->tele_outs[evil_teleport - 1].positions[tele_out];
    if (!config->sv_old_teleport_hook && !config->sv_old_teleport_weapons) {
      core->vel = v2(0, 0);

      if (!config->sv_teleport_hold_hook) {
        character_reset_hook(w, chr);
        world_release_hooked(w, character_id(chr));
      }
      if (config->sv_teleport_lose_weapons)
        character_reset_pickups(chr);
    }
    return;
  }
  if (collision_is_check_evil_teleport(col, map_index)) {
    /* first check if there is a TeleCheckOut for the current recorded checkpoint, if not check previous
     * checkpoints */
    for (int k = chr->tele_checkpoint - 1; k >= 0; k--) {
      if (col->tele_check_outs[k].count != 0) {
        int tele_out = world_pick_tele_out(w, character_id(chr), col->tele_check_outs[k].count);
        core->pos = col->tele_check_outs[k].positions[tele_out];
        core->vel = v2(0, 0);

        if (!config->sv_teleport_hold_hook) {
          character_reset_hook(w, chr);
          world_release_hooked(w, character_id(chr));
        }

        return;
      }
    }
    /* if no checkpointout have been found (or if there no recorded checkpoint), teleport to start */
    vec2 spawn_pos;
    if (controller_can_spawn(w, &spawn_pos, character_id(chr))) {
      core->pos = spawn_pos;
      core->vel = v2(0, 0);

      if (!config->sv_teleport_hold_hook) {
        character_reset_hook(w, chr);
        world_release_hooked(w, character_id(chr));
      }
    }
    return;
  }
  if (collision_is_check_teleport(col, map_index)) {
    /* first check if there is a TeleCheckOut for the current recorded checkpoint, if not check previous
     * checkpoints */
    for (int k = chr->tele_checkpoint - 1; k >= 0; k--) {
      if (col->tele_check_outs[k].count != 0) {
        int tele_out = world_pick_tele_out(w, character_id(chr), col->tele_check_outs[k].count);
        core->pos = col->tele_check_outs[k].positions[tele_out];

        if (!config->sv_teleport_hold_hook)
          character_reset_hook(w, chr);

        return;
      }
    }
    /* if no checkpointout have been found (or if there no recorded checkpoint), teleport to start */
    vec2 spawn_pos;
    if (controller_can_spawn(w, &spawn_pos, character_id(chr))) {
      core->pos = spawn_pos;

      if (!config->sv_teleport_hold_hook)
        character_reset_hook(w, chr);
    }
    return;
  }
}

/* CCharacter::HandleTuneLayer */
static void character_handle_tune_layer(world_t *w, character_t *chr) {
  chr->tune_zone_old = chr->tune_zone;
  int current_index = collision_get_map_index(collision(w), chr->pos);
  chr->tune_zone = collision_is_tune(collision(w), current_index);
  chr->core.tuning = tuning_list(w)[chr->tune_zone]; /* throw tunings from specific zone into gamecore */
}

/* ------------------------------------------------------------------ tick */

/* CCharacter::DDRaceTick: runs before the core. */
static void character_ddrace_tick(world_t *w, character_t *chr) {
  const collision_t *col = collision(w);

  chr->input = chr->saved_input;
  controller_set_armor_progress(chr, chr->freeze_time);

  if (chr->core.live_frozen) {
    chr->input.direction = 0;
    chr->input.jump = 0;
    /* Hook is possible in live freeze */
  }
  if (chr->freeze_time > 0) {
    chr->freeze_time--;
    chr->input.direction = 0;
    chr->input.jump = 0;
    chr->input.hook = 0;
    if (chr->freeze_time == 1)
      character_unfreeze(w, chr);
  }

  character_handle_tune_layer(w, chr); /* need this before coretick */

  /* check if the tee is in any type of freeze */
  int index = collision_get_pure_map_index(col, chr->pos);
  const int tiles[] = {collision_get_tile_index(col, index), collision_get_front_tile_index(col, index),
                       collision_get_switch_type(col, index)};
  chr->core.is_in_freeze = false;
  for (int i = 0; i < 3; i++) {
    if (tiles[i] == TILE_FREEZE || tiles[i] == TILE_DFREEZE || tiles[i] == TILE_LFREEZE ||
        tiles[i] == TILE_DEATH) {
      chr->core.is_in_freeze = true;
      break;
    }
  }
  chr->core.is_in_freeze |= character_touches_death_tile(w, chr);

  chr->core.id = character_id(chr);
}

/* CCharacter::DDRacePostCoreTick: runs after the core and the weapons. */
static void character_ddrace_post_core_tick(world_t *w, character_t *chr) {
  const collision_t *col = collision(w);
  core_t *core = &chr->core;

  chr->time = (float)(server_tick(w) - chr->start_time) / ((float)SERVER_TICK_SPEED);

  if (core->endless_hook)
    core->hook_tick = 0;

  chr->frozen_last_tick = false;

  if (core->deep_frozen)
    character_freeze(w, chr);

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

  if (core->endless_jump && core->jumped > 1) {
    /* Players with infinite jumps always have light feet */
    core->jumped = 1;
  }

  int current_index = collision_get_map_index(col, chr->pos);
  character_handle_skippable_tiles(w, chr, current_index);
  if (!chr->alive)
    return;

  /* handle Anti-Skip tiles */
  map_indices_t indices = collision_map_indices(col, chr->prev_pos, chr->pos);
  int index;
  bool any_index = false;
  while (collision_map_indices_next(&indices, &index)) {
    any_index = true;
    character_handle_tiles(w, chr, index);
    if (!chr->alive)
      return;
  }
  if (!any_index) {
    character_handle_tiles(w, chr, current_index);
    if (!chr->alive)
      return;
  }

  /* teleport gun */
  if (chr->tele_gun_teleport) {
    create_particle(w, chr->pos, DDNET_PARTICLE_PLAYER_DEATH, character_id(chr));
    core->pos = chr->tele_gun_pos;
    if (!chr->is_blue_tele_gun_teleport)
      core->vel = v2(0, 0);
    create_particle(w, chr->tele_gun_pos, DDNET_PARTICLE_PLAYER_DEATH, character_id(chr));
    create_sound(w, chr->tele_gun_pos, DDNET_SOUND_WEAPON_SPAWN, character_id(chr));
    chr->tele_gun_teleport = false;
    chr->is_blue_tele_gun_teleport = false;
  }
}

/* CCharacter::PreTick */
void character_pre_tick(world_t *w, character_t *chr) {
  if (chr->start_time > server_tick(w)) {
    /* Prevent the player from getting a negative time
     * The main reason why this can happen is because of time penalty tiles
     * However, other reasons are hereby also excluded */
    character_die(w, chr, character_id(chr), WEAPON_WORLD);
  }

  if (chr->paused)
    return;

  character_ddrace_tick(w, chr);

  chr->core.input = chr->input;
  core_tick(w, &chr->core, true, !w->config.sv_no_weak_hook);
}

/* CCharacter::Tick */
void character_tick(world_t *w, character_t *chr) {
  if (w->config.sv_no_weak_hook) {
    if (chr->paused)
      return;

    core_tick_deferred(w, &chr->core);
  } else {
    character_pre_tick(w, chr);
  }

  /* handle Weapons */
  character_handle_weapons(w, chr);

  character_ddrace_post_core_tick(w, chr);

  /* Previnput */
  chr->prev_input = chr->input;

  chr->prev_pos = chr->core.pos;
}

/* CCharacter::TickDeferred: runs for every tee after all of them ticked. */
void character_tick_deferred(world_t *w, character_t *chr) {
  chr->core.id = character_id(chr);
  core_move(w, &chr->core);
  core_quantize(w, &chr->core);
  chr->pos = chr->core.pos;

  /* the sounds of the events of the core */
  const int events = chr->core.triggered_events, id = character_id(chr);
  if (events & COREEVENT_GROUND_JUMP)
    create_sound(w, chr->pos, DDNET_SOUND_PLAYER_JUMP, id);
  if (events & COREEVENT_HOOK_ATTACH_PLAYER)
    create_sound(w, chr->pos, DDNET_SOUND_HOOK_ATTACH_PLAYER, id);
  if (events & COREEVENT_HOOK_ATTACH_GROUND)
    create_sound(w, chr->pos, DDNET_SOUND_HOOK_ATTACH_GROUND, id);
  if (events & COREEVENT_HOOK_HIT_NOHOOK)
    create_sound(w, chr->pos, DDNET_SOUND_HOOK_NOATTACH, id);
  /* (and the effect a client makes of a double jump, which the server leaves to it) */
  if (events & COREEVENT_AIR_JUMP)
    create_particle(w, chr->pos, DDNET_PARTICLE_AIR_JUMP, id);
}

/* ---------------------------------------------------------- life & death */

/* CCharacter::IsGrounded */
bool character_is_grounded(const world_t *w, const character_t *chr) {
  if (collision_is_on_ground(collision(w), chr->pos, PHYSICAL_SIZE))
    return true;

  int move_restrictions_below = collision_get_move_restrictions(
      collision(w), NULL, NULL, vadd(chr->pos, v2(0, PHYSICAL_SIZE / 2 + 4)), 0.0f, -1);
  return (move_restrictions_below & DDNET_CANTMOVE_DOWN) != 0;
}

/* CCharacter::Pause */
void character_pause(world_t *w, character_t *chr, bool pause) {
  chr->paused = pause;
  if (pause) {
    world_remove_entity(w, DDNET_ENTTYPE_CHARACTER, character_id(chr));

    if (chr->core.hooked_player != -1) { /* Keeping hook would allow cheats */
      character_reset_hook(w, chr);
      world_release_hooked(w, character_id(chr));
    }
    chr->paused_tick = server_tick(w);
  } else {
    chr->core.vel = v2(0, 0);
    world_insert_entity(w, DDNET_ENTTYPE_CHARACTER, character_id(chr));
    if (chr->core.freeze_start > 0 && chr->paused_tick >= 0)
      chr->core.freeze_start += server_tick(w) - chr->paused_tick;
  }
}

/* CCharacter::DDRaceInit */
static void character_ddrace_init(world_t *w, character_t *chr) {
  chr->race_state = DDNET_RACE_NONE;
  chr->prev_pos = chr->pos;

  chr->core.id = character_id(chr);
  chr->tele_checkpoint = 0;
  chr->core.endless_hook = w->config.sv_endless_drag;
  if (w->config.sv_hit) {
    chr->core.hammer_hit_disabled = false;
    chr->core.shotgun_hit_disabled = false;
    chr->core.grenade_hit_disabled = false;
    chr->core.laser_hit_disabled = false;
  } else {
    chr->core.hammer_hit_disabled = true;
    chr->core.shotgun_hit_disabled = true;
    chr->core.grenade_hit_disabled = true;
    chr->core.laser_hit_disabled = true;
  }
  chr->core.jumps = 2;

  int team = teams_team(w, chr->core.id);

  /* a tee that spawns into its locked team joins the race of the team */
  if (teams_team_locked(w, team)) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (teams_team(w, i) == team && i != chr->core.id && w->players[i].active) {
        character_t *other = get_player_char(w, i);

        if (other) {
          chr->race_state = other->race_state;
          chr->start_time = other->start_time;
        }
      }
    }
  }
}

/* CCharacter::CCharacter and CCharacter::Spawn */
void character_spawn(world_t *w, int client_id, vec2 pos) {
  character_t *chr = &w->characters[client_id];
  player_t *player = &w->players[client_id];

  /* DDNet allocates characters from a zeroed pool. The controls of the caller
   * are not part of that. */
  const int spec = chr->spec, tele_out = chr->tele_out;
  memset(chr, 0, sizeof(*chr));
  chr->spec = spec;
  chr->tele_out = tele_out;
  chr->link.prev = -1;
  chr->link.next = -1;

  /* constructor: the character starts with the last input of the player */
  chr->input = player->input;
  /* never initialize both to zero */
  chr->input.target_x = 0;
  chr->input.target_y = -1;

  chr->latest_prev_input = chr->latest_input = chr->prev_input = chr->saved_input = chr->input;

  chr->last_time_cp = -1;

  /* Spawn */
  chr->last_weapon = WEAPON_HAMMER;
  chr->queued_weapon = -1;

  chr->pos = pos;

  chr->num_inputs = 0;
  chr->spawn_tick = server_tick(w);

  core_reset(w, &chr->core);
  chr->core.active_weapon = WEAPON_GUN;
  chr->core.pos = chr->pos;
  chr->core.id = client_id;
  int tune_zone = collision_is_tune(collision(w), collision_get_map_index(collision(w), pos));
  chr->core.tuning = tuning_list(w)[tune_zone];

  player->has_character = true;
  world_insert_entity(w, DDNET_ENTTYPE_CHARACTER, client_id);
  chr->alive = true;

  controller_on_character_spawn(w, chr);

  character_ddrace_init(w, chr);

  chr->tune_zone = tune_zone;
  chr->tune_zone_old = -1; /* no zone leave msg on spawn */
}

/* CCharacter::Die */
void character_die(world_t *w, character_t *chr, int killer, int weapon) {
  (void)killer;
  player_t *player = player_of(w, chr);

  /* a nice sound, and bursting tee death effect */
  create_sound(w, chr->pos, DDNET_SOUND_PLAYER_DIE, character_id(chr));
  create_particle(w, chr->pos, DDNET_PARTICLE_PLAYER_DEATH, character_id(chr));

  /* this is to rate limit respawning to 3 secs */
  player->previous_die_tick = player->die_tick;
  player->die_tick = server_tick(w);

  chr->alive = false;
  character_set_solo(w, chr, false);

  world_remove_entity(w, DDNET_ENTTYPE_CHARACTER, character_id(chr));
  teams_on_character_death(w, character_id(chr), weapon);
}
