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
  EMIT_SOUND(w, chr->pos, DDNET_SOUND_WEAPON_SWITCH, character_id(chr));

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
DDNET_HOT static void character_handle_jetpack(world_t *w, character_t *chr) {
  if (chr->core.active_weapon < 0)
    return;

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
      vec2 direction = vnormalize(v2(chr->latest_input.target_x, chr->latest_input.target_y));
      float strength = ddnet_tune(get_tuning(w, chr->tune_zone)->jetpack_strength);
      character_take_damage(w, chr, vscale(vscale(direction, -1.0f), (strength / 100.0f / 6.11f)), 0,
                            character_id(chr), chr->core.active_weapon);
    }
  }
  }
}

/* CCharacter::HandleNinja */
DDNET_HOT static void character_handle_ninja(world_t *w, character_t *chr) {
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
        if (chr->hit_objects[client_id >> 6] >> (client_id & 63) & 1)
          continue;

        /* check so we are sufficiently close */
        if (vdistance(other->pos, chr->pos) > radius)
          continue;

        /* Hit a player, give them damage and stuffs... */
        EMIT_SOUND(w, other->pos, DDNET_SOUND_NINJA_HIT, character_id(chr));
        chr->hit_objects[client_id >> 6] |= (uint64_t)1 << (client_id & 63);

        character_take_damage(w, other, v2(0, -10.0f), NINJA_DAMAGE, character_id(chr), WEAPON_NINJA);
      }
    }

    return;
  }
}

/* CCharacter::DoWeaponSwitch */
DDNET_HOT static void character_do_weapon_switch(world_t *w, character_t *chr) {
  /* Which weapon is wanted comes from the input, which a branch predictor
   * cannot know. What DDNet does here in ifs is done with numbers:
   *   make sure we can switch:
   *   if (reload_timer != 0 || queued_weapon == -1) return;
   *   if (weapons[WEAPON_NINJA].got || !weapons[queued_weapon].got) return;
   *   SetWeapon(queued_weapon), which does nothing for the active weapon */
  const int queued = chr->queued_weapon, active = chr->core.active_weapon;
  const int can = (chr->reload_timer == 0) & (queued != -1);
  const int index = queued & -can;
  const int change =
      can & !chr->core.weapons[WEAPON_NINJA].got & (chr->core.weapons[index].got != 0) & (queued != active);

  /* switch Weapon */
  chr->last_weapon = select_int(change, active, chr->last_weapon);
  chr->queued_weapon = select_int(change, -1, queued);
  chr->core.active_weapon = select_int(change, queued & -((unsigned)queued < NUM_WEAPONS), active);
#ifdef DDNET_PHYSICS_EVENTS
  if (change)
    emit_sound(w, chr->pos, DDNET_SOUND_WEAPON_SWITCH, character_id(chr));
#endif
}

/* CCharacter::HandleWeaponSwitch */
DDNET_HOT static void character_handle_weapon_switch(world_t *w, character_t *chr) {
  int wanted_weapon = chr->core.active_weapon;
  if (chr->queued_weapon != -1)
    wanted_weapon = chr->queued_weapon;

  /* (whether anything below the ninja is owned: nearly always the hammer, which says so on its own) */
  if (!chr->core.weapons[WEAPON_HAMMER].got) {
    bool anything = false;
    for (int i = 1; i < NUM_WEAPONS - 1; ++i)
      if (chr->core.weapons[i].got)
        anything = true;
    if (!anything)
      return;
  }
  /* select Weapon (counters that did not change count no presses) */
  /* (next_weapon and prev_weapon are next to each other: both at once) */
  uint64_t counters, prev_counters;
  memcpy(&counters, &chr->latest_input.next_weapon, sizeof(counters));
  memcpy(&prev_counters, &chr->latest_prev_input.next_weapon, sizeof(prev_counters));
  if (counters != prev_counters) {
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
  }

  /* Direct Weapon selection (the value really is taken from the other input:
   * m_Input, which has it from m_SavedInput at this time). Without branches,
   * like in character_do_weapon_switch(). */
  wanted_weapon =
      select_int(chr->latest_input.wanted_weapon != 0, chr->saved_input.wanted_weapon - 1, wanted_weapon);

  /* check for insane values */
  {
    const int sane = (unsigned)wanted_weapon < NUM_WEAPONS;
    const int take = sane & (wanted_weapon != chr->core.active_weapon) &
                     (chr->core.weapons[wanted_weapon & -sane].got != 0);
    chr->queued_weapon = select_int(take, wanted_weapon, chr->queued_weapon);
  }

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
/* CCharacter::FireWeapon from the point on where it is known that the weapon fires. */
DDNET_NOINLINE static void character_fire(world_t *w, character_t *chr) {
  vec2 mouse_target = v2(chr->latest_input.target_x, chr->latest_input.target_y);
  vec2 direction = vnormalize(mouse_target);
  vec2 proj_start_pos = vadd(chr->pos, vscale(vscale(direction, PHYSICAL_SIZE), 0.75f));

  switch (chr->core.active_weapon) {
  case WEAPON_HAMMER: {
    EMIT_SOUND(w, chr->pos, DDNET_SOUND_HAMMER_FIRE, character_id(chr));

    if (chr->core.hammer_hit_disabled)
      break;

    int ents[MAX_CLIENTS];
    int hits = 0;
    int num = world_find_characters(w, proj_start_pos, PHYSICAL_SIZE * 0.5f, ents, MAX_CLIENTS);

    for (int i = 0; i < num; ++i) {
      character_t *target = &w->characters[ents[i]];

      if ((target == chr || (target->alive && !character_can_collide(w, chr, character_id(target)))))
        continue;

#ifdef DDNET_PHYSICS_EVENTS
      if (vlength(vsub(target->pos, proj_start_pos)) > 0.0f)
        emit_particle(
            w,
            vsub(target->pos,
                 vscale(vscale(vnormalize(vsub(target->pos, proj_start_pos)), PHYSICAL_SIZE), 0.5f)),
            DDNET_PARTICLE_HAMMER_HIT, character_id(chr));
      else
        emit_particle(w, proj_start_pos, DDNET_PARTICLE_HAMMER_HIT, character_id(chr));
#endif

      vec2 dir;
      if (vlength(vsub(target->pos, chr->pos)) > 0.0f)
        dir = vnormalize(vsub(target->pos, chr->pos));
      else
        dir = v2(0.f, -1.f);

      float strength = ddnet_tune(get_tuning(w, chr->tune_zone)->hammer_strength);

      vec2 temp = vadd(target->core.vel, vscale(vnormalize(vadd(dir, v2(0.f, -1.1f))), 10.0f));
      temp = clamp_vel(target->move_restrictions, temp);
      temp = vsub(temp, target->core.vel);
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

      EMIT_SOUND(w, chr->pos, DDNET_SOUND_GUN_FIRE, character_id(chr));
    }
  } break;

  case WEAPON_SHOTGUN: {
    float laser_reach = ddnet_tune(get_tuning(w, chr->tune_zone)->laser_reach);

    laser_create(w, chr->pos, direction, laser_reach, character_id(chr), WEAPON_SHOTGUN);
    EMIT_SOUND(w, chr->pos, DDNET_SOUND_SHOTGUN_FIRE, character_id(chr));
  } break;

  case WEAPON_GRENADE: {
    int lifetime = (int)(SERVER_TICK_SPEED * ddnet_tune(get_tuning(w, chr->tune_zone)->grenade_lifetime));

    projectile_create(w, WEAPON_GRENADE, character_id(chr), proj_start_pos, direction, lifetime, false, true,
                      LAYER_GAME, 0, 0);

    EMIT_SOUND(w, chr->pos, DDNET_SOUND_GRENADE_FIRE, character_id(chr));
  } break;

  case WEAPON_LASER: {
    float laser_reach = ddnet_tune(get_tuning(w, chr->tune_zone)->laser_reach);

    laser_create(w, chr->pos, direction, laser_reach, character_id(chr), WEAPON_LASER);
    EMIT_SOUND(w, chr->pos, DDNET_SOUND_LASER_FIRE, character_id(chr));
  } break;

  case WEAPON_NINJA: {
    /* reset Hit objects */
    memset(chr->hit_objects, 0, sizeof(chr->hit_objects));

    chr->core.ninja.activation_dir = direction;
    chr->core.ninja.current_move_time = NINJA_MOVETIME * SERVER_TICK_SPEED / 1000;

    /* clamp to prevent massive MoveBox calculation lag with SG bug */
    chr->core.ninja.old_vel_amount = (int)clampf(vlength(chr->core.vel), 0.0f, 6000.0f);

    EMIT_SOUND(w, chr->pos, DDNET_SOUND_NINJA_FIRE, character_id(chr));
  } break;
  }

  chr->attack_tick = server_tick(w);

  /* -1 is no weapon, handled here so pain sound still plays when firing in freeze */
  if (!chr->reload_timer && chr->core.active_weapon != -1) {
    chr->reload_timer = (int)(get_weapon_fire_delay(get_tuning(w, chr->tune_zone), chr->core.active_weapon) *
                              SERVER_TICK_SPEED);
  }
}

/* CCharacter::FireWeapon after its DoWeaponSwitch() */
DDNET_HOT static void character_fire_weapon_switched(world_t *w, character_t *chr) {
  /* The key is up and was not pressed since the last input: nothing below
   * fires (no press to count, and a weapon that fires while the key is held
   * needs it down). */
  if (!(chr->latest_input.fire & 1) && chr->latest_input.fire == chr->latest_prev_input.fire)
    return;

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
    /* Timer stuff to avoid shrieking orchestra caused by unfreeze-plasma (DDNet counts its timer down in
     * CCharacter::HandleWeapons, after which this comes on the same tick, or before it on the next one: the
     * sound comes again on the tick 50 after it at the earliest, moved on by the ticks the tee misses: see
     * character_pause() and world_remove_entities_from_player()) */
    if (server_tick(w) >= chr->pain_sound_tick && !(chr->latest_prev_input.fire & 1)) {
      chr->pain_sound_tick = server_tick(w) + 1 * SERVER_TICK_SPEED;
      EMIT_SOUND(w, chr->pos, DDNET_SOUND_PLAYER_PAIN_LONG, character_id(chr));
    }
    return;
  }

  /* check for ammo */
  if (chr->core.active_weapon < 0 || !chr->core.weapons[chr->core.active_weapon].ammo)
    return;

  character_fire(w, chr);
}

DDNET_HOT static void character_fire_weapon(world_t *w, character_t *chr) {
  if (chr->reload_timer != 0)
    return;

  /* (nearly never queued here: CCharacter::HandleWeaponSwitch has switched to it already) */
  if (chr->queued_weapon != -1)
    character_do_weapon_switch(w, chr);

  character_fire_weapon_switched(w, chr);
}

/* CCharacter::HandleWeapons */
DDNET_HOT static void character_handle_weapons(world_t *w, character_t *chr) {
  /* ninja */
  character_handle_ninja(w, chr);
  character_handle_jetpack(w, chr);

  /* (the pain sound timer: a tick, see character_fire_weapon_switched()) */

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
DDNET_HOT void character_on_predicted_input(world_t *w, character_t *chr, const input_t *new_input) {
  (void)w;
  /* copy new input. DDNet copies it to m_Input too, which nothing looks at
   * before the tick of the tee sets it from m_SavedInput again. */
  chr->saved_input = *new_input;

  /* it is not allowed to aim in the center */
  if (chr->saved_input.target_x == 0 && chr->saved_input.target_y == 0)
    chr->saved_input.target_y = -1;
}

/* CCharacter::OnDirectInput */
DDNET_HOT void character_on_direct_input(world_t *w, character_t *chr, const input_t *new_input) {
  /* (latest_prev_input is latest_input already, from the end of this function the last time) */
  chr->latest_input = *new_input;
  chr->num_inputs++;

  /* it is not allowed to aim in the center */
  if (chr->latest_input.target_x == 0 && chr->latest_input.target_y == 0)
    chr->latest_input.target_y = -1;

  if (chr->num_inputs > 1) {
    character_handle_weapon_switch(w, chr);
    /* CCharacter::FireWeapon, whose DoWeaponSwitch() changes nothing right after the one HandleWeaponSwitch()
     * ends with: a switch either happened there and nothing is queued anymore, or did not happen for a
     * reason that is still there (or nothing is owned to switch to, when that one was not reached) */
    if (chr->reload_timer == 0)
      character_fire_weapon_switched(w, chr);
  }

  chr->latest_prev_input = chr->latest_input;
}

/* ----------------------------------------------------------------- tiles */

/* True if one of the four sensor points of the tee touches a death tile. */
DDNET_HOT static bool character_touches_death_tile(const world_t *w, character_t *chr) {
  const collision_t *col = collision(w);
  vec2 pos = chr->pos;
  /* the sensors are a third of a tile away from the center at most */
  if (!(chr->here_flags & DDNET_TILEFLAG_NEAR_DEATH))
    return false;
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
DDNET_HOT static void character_handle_skippable_tiles(world_t *w, character_t *chr, int index) {
  const collision_t *col = collision(w);

  /* handle death-tiles and leaving gamelayer */
  if (character_touches_death_tile(w, chr) &&
      !(character_team(w, chr) && w->teams.tee_finished[character_id(chr)])) {
    character_die(w, chr, character_id(chr), WEAPON_WORLD);
    return;
  }

  /* a tee that is on the map has not left it */
  if (!chr->here_on_map && game_layer_clipped(w, chr->pos)) {
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
typedef struct chr_switch_ctx_t {
  world_t *w;
  character_t *chr;
} chr_switch_ctx_t;

static bool character_is_switch_active_cb(unsigned char number, void *user) {
  chr_switch_ctx_t *ctx = user;
  world_t *w = ctx->w;
  return w->num_switchers != 0 && switch_status(w, number, character_team(w, ctx->chr));
}

/* CCharacter::SetTimeCheckpoint */
static void character_set_time_checkpoint(world_t *w, character_t *chr, int time_checkpoint) {
  if (time_checkpoint <= -1)
    return;
  /* m_Time, which DDNet updates on every tick before it gets here */
  const float time = (float)(server_tick(w) - chr->start_time) / ((float)SERVER_TICK_SPEED);
  if (time_checkpoint > -1 && chr->race_state == DDNET_RACE_STARTED &&
      chr->current_time_cp[time_checkpoint] == 0.0f && time != 0.0f) {
    chr->last_time_cp = time_checkpoint;
    chr->current_time_cp[chr->last_time_cp] = time;
  }
}

/* Set the state of a switch for the team of a tee (the four cases of the
 * switch tiles in CCharacter::HandleTiles). */
static void character_set_switch(world_t *w, character_t *chr, int number, bool status, int end_tick,
                                 int type) {
  int team = character_team(w, chr);
  ddnet_switch_state_t *switcher = world_switch_write(w, number, team);
  if (!switcher)
    return;
  w->switches_touched[team] = true;
  switcher->status = status;
  switcher->end_tick = end_tick;
  switcher->type = (uint8_t)type;
  switcher->last_update_tick = server_tick(w);
  if (type == TILE_SWITCHTIMEDOPEN || type == TILE_SWITCHTIMEDCLOSE)
    world_switch_timer_started(w, number, team);
}

/* True if the switched tile the tee stands on is active for its team. */
static bool character_switch_applies(world_t *w, character_t *chr, int switch_number) {
  return switch_number == 0 || switch_status(w, switch_number, character_team(w, chr));
}

/* The time penalty and bonus tiles also move the start time of the whole team. */
static void character_share_start_time(world_t *w, character_t *chr) {
  int team = teams_team(w, chr->core.id);
  if (w->config.sv_team == DDNET_SV_TEAM_FORCED_SOLO || team != TEAM_FLOCK) {
    for (int i = 0; i < w->num_clients; i++) {
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
/* CCharacter::HandleTiles for a tile that does something, after the part that every tick has. */
/* The tiles of the game and the front layer that change something about the tee, for two different
 * tiles in the two layers: the ifs of CCharacter::HandleTiles as they are. */
DDNET_NOINLINE static void character_handle_tile_layers(world_t *w, character_t *chr, int tile, int ftile) {
  core_t *core = &chr->core;
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
}

DDNET_NOINLINE static void character_handle_tile(world_t *w, character_t *chr, int index) {
  const collision_t *col = collision(w);
  const ddnet_config_t *config = &w->config;
  core_t *core = &chr->core;
  int map_index = index;
  /* Which of the groups of checks below can find something on this tile is
   * known from the map (tile_handlers): the others are not gone through.
   * Each group is DDNet's code as it is. */
  const int handlers = col->tile_handlers[map_index];
  if (handlers & DDNET_TILEHANDLER_CHECKPOINT) {
    character_set_time_checkpoint(w, chr, collision_is_time_checkpoint(col, map_index));
    character_set_time_checkpoint(w, chr, collision_is_front_time_checkpoint(col, map_index));
    int tele_checkpoint = collision_is_tele_checkpoint(col, map_index);
    if (tele_checkpoint)
      chr->tele_checkpoint = tele_checkpoint;
  }

  /* (the start and the finish line are also looked for around the tee, wherever the tile is) */
  if ((handlers & DDNET_TILEHANDLER_RACE) || !chr->quiet_known || vne(chr->pos, chr->quiet_pos) ||
      !chr->here_on_map || (chr->here_flags & DDNET_TILEFLAG_NEAR_RACE)) {
    controller_handle_character_tiles(w, chr, index);
    if (!chr->alive)
      return;
  }

  /* The tiles of the game and the front layer that change something about
   * the tee. In DDNet these are 15 groups of ifs one after the other, each
   * looking at both layers. Each tile value belongs to at most one group, so
   * with one value of those groups on the tile (in one layer or the same in
   * both), which is nearly always so, only its group can find something: a
   * switch over the value, which is known from the map (layer_tiles). Each
   * case is what the ifs of its group do for that value. Two different values
   * go through the ifs. */
  if (handlers & DDNET_TILEHANDLER_LAYERS_TWO) {
    character_handle_tile_layers(w, chr, chr->tile_index, chr->tile_f_index);
    goto layers_done;
  }
  const int one = col->layer_tiles[map_index];
  switch (one) {
  case TILE_FREEZE:
    if (!core->deep_frozen)
      character_freeze(w, chr);
    break;
  case TILE_UNFREEZE:
    if (!core->deep_frozen)
      character_unfreeze(w, chr);
    break;
  case TILE_DFREEZE:
    core->deep_frozen = true;
    break;
  case TILE_DUNFREEZE:
    core->deep_frozen = false;
    break;
  case TILE_LFREEZE:
    core->live_frozen = true;
    break;
  case TILE_LUNFREEZE:
    core->live_frozen = false;
    break;
  case TILE_EHOOK_ENABLE:
    core->endless_hook = true;
    break;
  case TILE_EHOOK_DISABLE:
    core->endless_hook = false;
    break;
  case TILE_HIT_DISABLE:
    core->hammer_hit_disabled = true;
    core->shotgun_hit_disabled = true;
    core->grenade_hit_disabled = true;
    core->laser_hit_disabled = true;
    break;
  case TILE_HIT_ENABLE:
    core->shotgun_hit_disabled = false;
    core->grenade_hit_disabled = false;
    core->hammer_hit_disabled = false;
    core->laser_hit_disabled = false;
    break;
  case TILE_NPC_DISABLE:
    core->collision_disabled = true;
    break;
  case TILE_NPC_ENABLE:
    core->collision_disabled = false;
    break;
  case TILE_NPH_DISABLE:
    core->hook_hit_disabled = true;
    break;
  case TILE_NPH_ENABLE:
    core->hook_hit_disabled = false;
    break;
  case TILE_UNLIMITED_JUMPS_ENABLE:
    core->endless_jump = true;
    break;
  case TILE_UNLIMITED_JUMPS_DISABLE:
    core->endless_jump = false;
    break;
  case TILE_WALLJUMP:
    if (core->vel.y > 0 && core->colliding && core->left_wall) {
      core->left_wall = false;
      core->jumped_total = core->jumps >= 2 ? core->jumps - 2 : 0;
      core->jumped = 1;
    }
    break;
  case TILE_JETPACK_ENABLE:
    core->jetpack = true;
    break;
  case TILE_JETPACK_DISABLE:
    core->jetpack = false;
    break;
  case TILE_REFILL_JUMPS:
    if (!chr->last_refill_jumps) {
      core->jumped_total = 0;
      core->jumped = 0;
      chr->last_refill_jumps = true;
    }
    break;
  case TILE_TELE_GUN_ENABLE:
    core->has_telegun_gun = true;
    break;
  case TILE_TELE_GUN_DISABLE:
    core->has_telegun_gun = false;
    break;
  case TILE_TELE_GRENADE_ENABLE:
    core->has_telegun_grenade = true;
    break;
  case TILE_TELE_GRENADE_DISABLE:
    core->has_telegun_grenade = false;
    break;
  case TILE_TELE_LASER_ENABLE:
    core->has_telegun_laser = true;
    break;
  case TILE_TELE_LASER_DISABLE:
    core->has_telegun_laser = false;
    break;
  default:
    break;
  }
  /* (refill jumps: the jumps are refilled again only after a tick on another tile) */
  if (one != TILE_REFILL_JUMPS)
    chr->last_refill_jumps = false;

layers_done:
  /* stopper (without restrictions there is nothing to stop) */
  if (chr->move_restrictions) {
    if (core->vel.y > 0 && (chr->move_restrictions & DDNET_CANTMOVE_DOWN)) {
      core->jumped = 0;
      core->jumped_total = 0;
    }
    character_apply_move_restrictions(chr);
  }

  if (!(handlers & DDNET_TILEHANDLER_SWITCH)) {
    /* what the switch tiles below leave behind without one */
    chr->last_penalty = false;
    chr->last_bonus = false;
    goto switch_done;
  }

  /* handle switch tiles */
  {
    const int switch_type = collision_get_switch_type(col, map_index);
    const int switch_number = collision_get_switch_number(col, map_index);
    const int switch_delay = collision_get_switch_delay(col, map_index);
    /* (the else-ifs of DDNet each look for another switch_type first: a switch over it) */
    switch (switch_type) {
    case TILE_SWITCHOPEN:
      if (switch_number > 0)
        character_set_switch(w, chr, switch_number, true, 0, TILE_SWITCHOPEN);
      break;
    case TILE_SWITCHTIMEDOPEN:
      if (switch_number > 0)
        character_set_switch(w, chr, switch_number, true,
                             server_tick(w) + 1 + switch_delay * SERVER_TICK_SPEED, TILE_SWITCHTIMEDOPEN);
      break;
    case TILE_SWITCHTIMEDCLOSE:
      if (switch_number > 0)
        character_set_switch(w, chr, switch_number, false,
                             server_tick(w) + 1 + switch_delay * SERVER_TICK_SPEED, TILE_SWITCHTIMEDCLOSE);
      break;
    case TILE_SWITCHCLOSE:
      if (switch_number > 0)
        character_set_switch(w, chr, switch_number, false, 0, TILE_SWITCHCLOSE);
      break;
    case TILE_FREEZE:
      if (character_switch_applies(w, chr, switch_number))
        character_freeze_seconds(w, chr, switch_delay);
      break;
    case TILE_DFREEZE:
      if (character_switch_applies(w, chr, switch_number))
        core->deep_frozen = true;
      break;
    case TILE_DUNFREEZE:
      if (character_switch_applies(w, chr, switch_number))
        core->deep_frozen = false;
      break;
    case TILE_LFREEZE:
      if (character_switch_applies(w, chr, switch_number))
        core->live_frozen = true;
      break;
    case TILE_LUNFREEZE:
      if (character_switch_applies(w, chr, switch_number))
        core->live_frozen = false;
      break;
    case TILE_HIT_ENABLE:
      /* for the weapon in the delay */
      if (switch_delay == WEAPON_HAMMER)
        core->hammer_hit_disabled = false;
      else if (switch_delay == WEAPON_SHOTGUN)
        core->shotgun_hit_disabled = false;
      else if (switch_delay == WEAPON_GRENADE)
        core->grenade_hit_disabled = false;
      else if (switch_delay == WEAPON_LASER)
        core->laser_hit_disabled = false;
      break;
    case TILE_HIT_DISABLE:
      if (switch_delay == WEAPON_HAMMER)
        core->hammer_hit_disabled = true;
      else if (switch_delay == WEAPON_SHOTGUN)
        core->shotgun_hit_disabled = true;
      else if (switch_delay == WEAPON_GRENADE)
        core->grenade_hit_disabled = true;
      else if (switch_delay == WEAPON_LASER)
        core->laser_hit_disabled = true;
      break;
    case TILE_JUMP: {
      int new_jumps = switch_delay;
      if (new_jumps == 255)
        new_jumps = -1;

      if (new_jumps != core->jumps)
        core->jumps = new_jumps;
      break;
    }
    case TILE_ADD_TIME:
      if (!chr->last_penalty) {
        const int minutes = switch_delay;
        const int seconds = switch_number;

        chr->start_time -= (minutes * 60 + seconds) * SERVER_TICK_SPEED;
        character_share_start_time(w, chr);

        chr->last_penalty = true;
      }
      break;
    case TILE_SUBTRACT_TIME:
      if (!chr->last_bonus) {
        const int minutes = switch_delay;
        const int seconds = switch_number;

        chr->start_time += (minutes * 60 + seconds) * SERVER_TICK_SPEED;
        if (chr->start_time > server_tick(w))
          chr->start_time = server_tick(w);
        character_share_start_time(w, chr);

        chr->last_bonus = true;
      }
      break;
    default:
      break;
    }

    if (switch_type != TILE_ADD_TIME)
      chr->last_penalty = false;

    if (switch_type != TILE_SUBTRACT_TIME)
      chr->last_bonus = false;
  }

switch_done:
  if (!(handlers & DDNET_TILEHANDLER_TELE))
    return;

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

DDNET_HOT static void character_handle_tiles(world_t *w, character_t *chr, int index) {
  PROF_COUNT(10 + (index >= 0), index >= 0 ? "handle_tiles(tile)" : "handle_tiles(none)");
  const collision_t *col = collision(w);

  int map_index = index;
  chr->tile_index = collision_get_tile_index(col, map_index);
  chr->tile_f_index = collision_get_front_tile_index(col, map_index);
  chr_switch_ctx_t ctx = {w, chr};
  /* no stopper or door around the tee, nor around the tile that is handled in place of the one it is on */
  if (!(chr->here_flags & DDNET_TILEFLAG_NEAR_STOPPER) &&
      (map_index < 0 || !(col->tile_flags[map_index] & DDNET_TILEFLAG_NEAR_STOPPER)))
    chr->move_restrictions = 0;
  else
    chr->move_restrictions =
        collision_get_move_restrictions(col, character_is_switch_active_cb, &ctx, chr->pos, 18.0f, map_index);
  if (index < 0) {
    chr->last_refill_jumps = false;
    chr->last_penalty = false;
    chr->last_bonus = false;
    return;
  }
  character_handle_tile(w, chr, index);
}

/* CCharacter::HandleTuneLayer */
static void character_handle_tune_layer(world_t *w, character_t *chr) {
  chr->tune_zone_old = chr->tune_zone;
  if (!collision(w)->tune) {
    chr->tune_zone = 0;
    return;
  }
  int current_index = collision_get_map_index(collision(w), chr->pos);
  chr->tune_zone = collision_is_tune(collision(w), current_index);
}

/* ------------------------------------------------------------------ tick */

/* CCharacter::DDRaceTick: runs before the core. */
DDNET_HOT static void character_ddrace_tick(world_t *w, character_t *chr) {
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

  if (chr->quiet) {
    /* no tune zone, freeze or death tile here */
    chr->tune_zone_old = chr->tune_zone;
    chr->tune_zone = 0;
    chr->core.is_in_freeze = false;
    chr->core.id = character_id(chr);
    return;
  }

  character_handle_tune_layer(w, chr); /* need this before coretick */

  /* check if the tee is in any type of freeze */
  int index = collision_get_pure_map_index(col, chr->pos);
  chr->core.is_in_freeze = (col->tile_flags[index] & DDNET_TILEFLAG_FREEZE) != 0;
  chr->core.is_in_freeze |= character_touches_death_tile(w, chr);

  chr->core.id = character_id(chr);
}

/* CCharacter::DDRacePostCoreTick: runs after the core and the weapons. */
/* The tiles between where the tee was and where it is, for a tee that got onto another tile. */
DDNET_NOINLINE static void character_handle_tiles_on_the_way(world_t *w, character_t *chr,
                                                             int current_index) {
  const collision_t *col = collision(w);
  PROF_COUNT(12, "map indices walks");
  map_indices_t indices = collision_map_indices(col, chr->prev_pos, chr->pos);
  int index;
  bool any_index = false;
  while (collision_map_indices_next(&indices, &index)) {
    any_index = true;
    character_handle_tiles(w, chr, index);
    if (!chr->alive)
      return;
  }
  if (!any_index)
    character_handle_tiles(w, chr, current_index);
}

/* The jump rules at the end of CCharacter::DDRacePostCoreTick. */
static inline void character_jump_rules(core_t *core) {
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
}

/* The teleport gun at the end of CCharacter::DDRacePostCoreTick. */
static inline void character_teleport_by_gun(world_t *w, character_t *chr) {
  core_t *core = &chr->core;
  /* teleport gun */
  if (chr->tele_gun_teleport) {
    EMIT_PARTICLE(w, chr->pos, DDNET_PARTICLE_PLAYER_DEATH, character_id(chr));
    core->pos = chr->tele_gun_pos;
    if (!chr->is_blue_tele_gun_teleport)
      core->vel = v2(0, 0);
    EMIT_PARTICLE(w, chr->tele_gun_pos, DDNET_PARTICLE_PLAYER_DEATH, character_id(chr));
    EMIT_SOUND(w, chr->tele_gun_pos, DDNET_SOUND_WEAPON_SPAWN, character_id(chr));
    chr->tele_gun_teleport = false;
    chr->is_blue_tele_gun_teleport = false;
  }
}

DDNET_HOT static void character_ddrace_post_core_tick(world_t *w, character_t *chr) {
  const collision_t *col = collision(w);
  core_t *core = &chr->core;

  if (core->endless_hook)
    core->hook_tick = 0;

  chr->frozen_last_tick = false;

  if (core->deep_frozen)
    character_freeze(w, chr);

  character_jump_rules(core);

  /* A tee that came less than a tile far in both directions came from this
   * tile or one next to it, and every point of its way that DDNet looks at is
   * on those two or between them: on a quiet tile, on none that exists. */
  const bool came_from_nearby =
      fabsf(chr->pos.x - chr->prev_pos.x) < 32.0f && fabsf(chr->pos.y - chr->prev_pos.y) < 32.0f;
  if (chr->quiet && (came_from_nearby || collision_tile_at_pos(col, chr->prev_pos) == chr->here_tile)) {
    /* On the map, no death tile around and no tile to handle, which leaves
     * what HandleTiles() does without one. */
    chr->tile_index = 0;
    chr->tile_f_index = 0;
    chr->move_restrictions = 0;
    chr->last_refill_jumps = false;
    chr->last_penalty = false;
    chr->last_bonus = false;
    goto tiles_done;
  }

  /* collision_get_map_index(col, chr->pos), with what is known about that tile */
  int current_index = (chr->here_flags & DDNET_TILEFLAG_EXISTS) ? chr->here_tile : -1;
  character_handle_skippable_tiles(w, chr, current_index);
  if (!chr->alive)
    return;

  /* handle Anti-Skip tiles */
  if (collision_tile_at_pos(col, chr->prev_pos) == chr->here_tile) {
    /* The tee stayed on one tile, and so did every point of its way that
     * DDNet looks at. That tile is handled if it does something, which is what
     * both branches below come down to. */
    character_handle_tiles(w, chr, current_index);
    if (!chr->alive)
      return;
  } else {
    character_handle_tiles_on_the_way(w, chr, current_index);
    if (!chr->alive)
      return;
  }

tiles_done:
  character_teleport_by_gun(w, chr);
}

/* CCharacter::PreTick */
DDNET_HOT void character_pre_tick(world_t *w, character_t *chr) {
  /* What is known about where the tee is (quiet, here_tile, here_flags) is
   * brought up to date here, and holds for the rest of its tick: the tee only
   * moves after it. */
  character_is_quiet(collision(w), chr);

  if (chr->start_time > server_tick(w)) {
    /* Prevent the player from getting a negative time
     * The main reason why this can happen is because of time penalty tiles
     * However, other reasons are hereby also excluded */
    character_die(w, chr, character_id(chr), WEAPON_WORLD);
  }

  if (chr->paused)
    return;

  PROF_BEGIN(PROF_DDRACE_TICK);
  character_ddrace_tick(w, chr);
  PROF_END(PROF_DDRACE_TICK);

  PROF_BEGIN(PROF_CORE_TICK);
  chr->core.input = chr->input;
  core_tick(w, &chr->core, true, !w->config.sv_no_weak_hook,
            veq(chr->pos, chr->core.pos) ? chr->here_flags : collision_flags_at(collision(w), chr->core.pos));
  PROF_END(PROF_CORE_TICK);
}

/* CCharacter::Tick */
static void character_tick_any(world_t *w, character_t *chr);

/* CCharacter::Tick for the tee most ticks are about: one that is not frozen,
 * paused or a ninja, on a tile with nothing around, which it did not come to
 * from far away. Everything that the functions below do one after the other
 * for such a tee, without asking again and again whether it is one. False if
 * it is not, with nothing done. */
static inline __attribute__((always_inline)) bool character_tick_plain_fresh(world_t *w, character_t *chr) {
  core_t *core = &chr->core;
  const collision_t *col = collision(w);
  if (!(chr->here_flags & DDNET_TILEFLAG_INERT) ||
      chr->paused | (chr->freeze_time != 0) | core->live_frozen | core->deep_frozen |
          (core->active_weapon == WEAPON_NINJA) | core->jetpack | (chr->start_time > server_tick(w)))
    return false;
  /* The tiles on its way, which DDNet handles if they do something: none of
   * them does (see character_ddrace_post_core_tick() about the first case). */
  /* (both coordinates at once: less than 32 apart, which is false for numbers that are not ones) */
  const __m128 apart = _mm_andnot_ps(
      _mm_set1_ps(-0.0f), _mm_sub_ps(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&chr->pos)),
                                     _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&chr->prev_pos))));
  const bool nearby = (_mm_movemask_ps(_mm_cmplt_ps(apart, _mm_set1_ps(32.0f))) & 3) == 3;
  if (!(chr->quiet && nearby)) {
    const vec2 from = chr->prev_pos, to = chr->pos;
    if (collision_tile_at_pos(col, from) != chr->here_tile) {
      /* From another tile (this one is on the map, see character_here_refresh()).
       * With whole numbers for both ends, the points on the way are between the
       * ends: on the tiles of the rectangle that the two span. Point i is
       * a + (b - a) * (i / d) for i below (int)(d + 1): b - a is exact, i / d is
       * at most 1, the product is not larger than b - a, and the sum is rounded
       * to a number between the ends, which are numbers. (i / d is larger than
       * 1 if d + 1 rounds up to a whole number, for d the number right below a
       * power of two. Not for a way shorter than 2^12 between whole numbers,
       * whose length is the square root of a whole number: the one closest to
       * 2^k from below is 2^k - 1 / 2^(k+1) or further, and that needs k > 11 to
       * round to 2^k - 2^(k-24).) */
#ifdef __SSE4_1__
      /* (all four coordinates at once: from.x, from.y, to.x, to.y; the start on the map and all of them
       * whole numbers, where (float)(int)f == f, which is false for one that is not an int either) */
      const __m128 ends = _mm_movelh_ps(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&chr->prev_pos)),
                                        _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&chr->pos)));
      const __m128i whole = _mm_cvttps_epi32(ends);
      const __m128 units = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&col->units_width));
      if (_mm_movemask_ps(_mm_cmpeq_ps(ends, _mm_cvtepi32_ps(whole))) != 15 ||
          (_mm_movemask_ps(_mm_and_ps(_mm_cmpge_ps(ends, _mm_setzero_ps()), _mm_cmplt_ps(ends, units))) &
           3) != 3)
        return false;
      const __m128i tiles = _mm_srai_epi32(whole, 5),
                    other = _mm_shuffle_epi32(tiles, _MM_SHUFFLE(1, 0, 3, 2));
      const __m128i low = _mm_min_epi32(tiles, other);
      const __m128i high = _mm_add_epi32(_mm_max_epi32(tiles, other), _mm_set1_epi32(1));
      const int x0 = _mm_cvtsi128_si32(low), y0 = _mm_extract_epi32(low, 1);
      const int x1 = _mm_cvtsi128_si32(high), y1 = _mm_extract_epi32(high, 1);
      (void)to;
#else
      if (!(from.x >= 0 && from.y >= 0 && from.x < col->units_width && from.y < col->units_height) ||
          from.x != (float)(int)from.x || from.y != (float)(int)from.y || to.x != (float)(int)to.x ||
          to.y != (float)(int)to.y)
        return false;
      const int fx = (int)from.x >> 5, fy = (int)from.y >> 5, tx = (int)to.x >> 5, ty = (int)to.y >> 5;
      const int x0 = mini(fx, tx), y0 = mini(fy, ty), x1 = maxi(fx, tx) + 1, y1 = maxi(fy, ty) + 1;
#endif
      /* (64 tiles: shorter than 64 * 32 * sqrt(2) < 2^12) */
      if (x1 - x0 > 64 || y1 - y0 > 64 || rect_count(col, col->sat_exists, x0, y0, x1, y1))
        return false;
    }
  }

  /* CCharacter::DDRaceTick */
  chr->input = chr->saved_input;
  chr->armor = 10; /* controller_set_armor_progress() without freeze time */
  chr->tune_zone_old = chr->tune_zone;
  chr->tune_zone = 0;
  core->is_in_freeze = false;
  core->id = character_id(chr);

  core->input = chr->input;
  /* (the tune zone was just set to 0) */
  const bool same = veq_fast(chr->pos, core->pos);
  core_tick_tuned(w, core, true, true, same ? chr->here_flags : collision_flags_at(collision(w), core->pos),
                  &w->tuning_values[0], same && chr->here_on_map);

  /* CCharacter::HandleWeapons, where neither ninja nor jetpack have something to do */
  if (chr->reload_timer)
    chr->reload_timer--;
  else
    character_fire_weapon(w, chr);

  /* CCharacter::DDRacePostCoreTick */
  if (core->endless_hook)
    core->hook_tick = 0;
  chr->frozen_last_tick = false;
  character_jump_rules(core);
  chr->tile_index = 0;
  chr->tile_f_index = 0;
  chr->move_restrictions = 0;
  chr->last_refill_jumps = false;
  chr->last_penalty = false;
  chr->last_bonus = false;
  character_teleport_by_gun(w, chr);

  chr->prev_pos = core->pos;
  return true;
}

/* ... with what is known about where the tee is looked up first (character_tick_plain_fresh() is for a
 * caller that has just done that) */
static inline bool character_tick_plain(world_t *w, character_t *chr) {
  character_is_quiet(collision(w), chr);
  return character_tick_plain_fresh(w, chr);
}

DDNET_HOT void character_tick(world_t *w, character_t *chr) {
  if (!w->config.sv_no_weak_hook && character_tick_plain(w, chr))
    return;
  character_tick_any(w, chr);
}

/* CCharacter::Tick for any tee (kept out of the function above, which most ticks only need the start of) */
DDNET_NOINLINE static void character_tick_any(world_t *w, character_t *chr) {
  if (w->config.sv_no_weak_hook) {
    if (chr->paused)
      return;

    core_tick_deferred(w, &chr->core);
  } else {
    character_pre_tick(w, chr);
  }

  /* handle Weapons */
  PROF_BEGIN(PROF_WEAPONS);
  character_handle_weapons(w, chr);
  PROF_END(PROF_WEAPONS);

  PROF_BEGIN(PROF_POST_CORE);
  character_ddrace_post_core_tick(w, chr);
  PROF_END(PROF_POST_CORE);

  chr->prev_pos = chr->core.pos;
}

/* The sounds of the events of the core, after its move (CCharacter::TickDeferred), and the effect a client
 * makes of a double jump. */
static inline void character_core_events(world_t *w, const character_t *chr) {
#ifdef DDNET_PHYSICS_EVENTS
  const int events = chr->core.triggered_events, id = character_id(chr);
  if (events & COREEVENT_GROUND_JUMP)
    emit_sound(w, chr->pos, DDNET_SOUND_PLAYER_JUMP, id);
  if (events & COREEVENT_HOOK_ATTACH_PLAYER)
    emit_sound(w, chr->pos, DDNET_SOUND_HOOK_ATTACH_PLAYER, id);
  if (events & COREEVENT_HOOK_ATTACH_GROUND)
    emit_sound(w, chr->pos, DDNET_SOUND_HOOK_ATTACH_GROUND, id);
  if (events & COREEVENT_HOOK_HIT_NOHOOK)
    emit_sound(w, chr->pos, DDNET_SOUND_HOOK_NOATTACH, id);
  if (events & COREEVENT_AIR_JUMP)
    emit_particle(w, chr->pos, DDNET_PARTICLE_AIR_JUMP, id);
#else
  (void)w;
  (void)chr;
#endif
}

#ifdef __SSE4_1__
#include <smmintrin.h>
/* character_tick_deferred() for a tee whose move no other tee can stop
 * (core_move_tees()), after core_ramp(): what core_move(),
 * collision_move_box_rounded(), core_quantize() and character_here_refresh() do
 * one after the other, in one piece. The position stays in a register from the
 * move to the rounding, and the tile it is on comes from the whole numbers the
 * rounding has anyway. Every float operation is the one of those functions. */
static inline __attribute__((always_inline)) void
character_move_alone(world_t *w, character_t *chr, const tuning_t *tuning, bool ramp, float ramp_value) {
  const collision_t *col = collision(w);
  core_t *core = &chr->core;
  const float old_vel_x = core->vel.x;

  const __m128 sign = _mm_set1_ps(-0.0f);
  const __m128 p = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&core->pos));
  /* (the two coordinates of the velocity one by one: they were stored one by one a moment ago, and a
   * load of both at once would have to wait for those stores to get out of the store buffer) */
  const __m128 v = _mm_unpacklo_ps(_mm_load_ss(&core->vel.x), _mm_load_ss(&core->vel.y));
  __m128 end = p;
  bool at_once = false, grounded = false;
  {
    /* collision_move_box_rounded(): the sum at once if it rounds like the
     * steps do and nothing solid is on the way. rounds_like_steps() with one
     * bound for both coordinates, which is not smaller than the one of each:
     * the position has no negative coordinate here. */
    const float travel = fabsf(core->vel.x) + fabsf(core->vel.y);
    if (travel < 4096.0f && (_mm_movemask_ps(p) & 3) == 0) {
      const float largest = (core->pos.x + core->pos.y) + (travel + 1.0f);
      const float error = largest * (((travel + 2.0f) + 4.0f) * 1.2e-7f) + 2e-5f;
      const __m128 direct = _mm_add_ps(p, v);
      const __m128 size = _mm_andnot_ps(sign, direct);
      const __m128 fraction = _mm_sub_ps(size, _mm_round_ps(size, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));
      const __m128 distance = _mm_andnot_ps(sign, _mm_sub_ps(fraction, _mm_set1_ps(0.5f)));
      if (largest < 1e6f && error < 0.2f &&
          (_mm_movemask_ps(_mm_cmpgt_ps(distance, _mm_set1_ps(error))) & 3) == 3) {
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
            PROF_COUNT(29, "move_box: sum at once");
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
                   v2(ddnet_tune(tuning->ground_elasticity_x), ddnet_tune(tuning->ground_elasticity_y)),
                   &grounded, true);
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

  /* CCharacterCore::Quantize, see core_quantize(): the position and the velocity ... */
  const __m128 scale = _mm_setr_ps(1.0f, 1.0f, 256.0f, 256.0f);
  const __m128 inverse = _mm_setr_ps(1.0f, 1.0f, 1.0f / 256.0f, 1.0f / 256.0f);
  const __m128 moved = _mm_mul_ps(
      _mm_movelh_ps(end, _mm_unpacklo_ps(_mm_load_ss(&core->vel.x), _mm_load_ss(&core->vel.y))), scale);
  const __m128i whole =
      _mm_cvttps_epi32(_mm_add_ps(moved, _mm_or_ps(_mm_and_ps(moved, sign), _mm_set1_ps(0.5f))));
  const __m128 rounded = _mm_mul_ps(_mm_cvtepi32_ps(whole), inverse);
  _mm_storeu_ps(&core->pos.x, rounded);
  /* ... and the hook */
  _mm_storeu_ps(&core->hook_pos.x, quantize4(load4(&core->hook_pos.x), scale, inverse));
  _mm_storel_epi64((__m128i *)&chr->pos, _mm_castps_si128(rounded));

  /* character_here_refresh(): the position is the whole numbers it was rounded to */
  /* (on the map: 0 <= x < units_width and the same for y, as floats like character_here_refresh() does) */
  const __m128 units = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&col->units_width));
  if ((_mm_movemask_ps(_mm_and_ps(_mm_cmpge_ps(rounded, _mm_setzero_ps()), _mm_cmplt_ps(rounded, units))) &
       3) == 3) {
    const int x = _mm_cvtsi128_si32(whole), y = _mm_extract_epi32(whole, 1);
    chr->here_on_map = true;
    chr->here_tile = (y >> 5) * col->width + (x >> 5);
    chr->here_flags = col->tile_flags[chr->here_tile];
    chr->quiet = (chr->here_flags & DDNET_TILEFLAG_QUIET) != 0;
    chr->quiet_pos = chr->pos;
    chr->quiet_known = true;
  } else {
    character_here_refresh(col, chr);
  }
  character_core_events(w, chr);
}

/* character_tick_deferred() for a tee that is alone in the world */
static inline void character_tick_deferred_lone(world_t *w, character_t *chr, const tuning_t *tuning) {
  chr->core.id = character_id(chr);
  float ramp_value;
  const bool ramp = core_ramp(&chr->core, tuning, &ramp_value);
  character_move_alone(w, chr, tuning, ramp, ramp_value);
}
#endif

/* CCharacter::TickDeferred: runs for every tee after all of them ticked. */
DDNET_HOT DDNET_NO_CANARY void character_tick_deferred(world_t *w, character_t *chr) {
  core_t *core = &chr->core;
  core->id = character_id(chr);
  const tuning_t *tuning = core_tuning(w, core);
  PROF_BEGIN(PROF_MOVE);
  float ramp_value;
  const bool ramp = core_ramp(core, tuning, &ramp_value);
  int near[DDNET_MAX_CLIENTS];
  const int num_near = core_move_tees(w, core, tuning, near);
#ifdef __SSE4_1__
  /* (no other tee on its way: in one piece) */
  if (num_near == 0) {
    character_move_alone(w, chr, tuning, ramp, ramp_value);
    PROF_END(PROF_MOVE);
    return;
  }
#endif
  core_move(w, core, tuning, ramp, ramp_value, near, num_near);
  PROF_END(PROF_MOVE);
  PROF_BEGIN(PROF_QUANTIZE);
  core_quantize(w, core);
  PROF_END(PROF_QUANTIZE);
  chr->pos = core->pos;
  /* for the next tick, which asks for it before anything else moves the tee */
  character_here_refresh(collision(w), chr);
  character_core_events(w, chr);
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
    world_core_remove(w, character_id(chr));
    world_remove_entity(w, DDNET_ENTTYPE_CHARACTER, character_id(chr));

    if (chr->core.hooked_player != -1) { /* Keeping hook would allow cheats */
      character_reset_hook(w, chr);
      world_release_hooked(w, character_id(chr));
    }
    chr->paused_tick = server_tick(w);
  } else {
    chr->core.vel = v2(0, 0);
    world_core_add(w, character_id(chr));
    world_insert_entity(w, DDNET_ENTTYPE_CHARACTER, character_id(chr));
    if (chr->core.freeze_start > 0 && chr->paused_tick >= 0)
      chr->core.freeze_start += server_tick(w) - chr->paused_tick;
    /* The tee missed the ticks after the one it paused on (in CPlayer::Tick after the tick of the world) up
     * to this one (before the tick that it is back for), on which DDNet counts the pain sound timer down. */
    if (chr->paused_tick >= 0)
      chr->pain_sound_tick += server_tick(w) - chr->paused_tick;
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
    for (int i = 0; i < w->num_clients; i++) {
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
  PROF_COUNT(9, "spawns");
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

  chr->latest_prev_input = chr->latest_input = chr->saved_input = chr->input;

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
  chr->tune_zone = tune_zone;

  player->has_character = true;
  world_insert_entity(w, DDNET_ENTTYPE_CHARACTER, client_id);
  chr->alive = true;
  world_core_add(w, client_id);

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
  EMIT_SOUND(w, chr->pos, DDNET_SOUND_PLAYER_DIE, character_id(chr));
  EMIT_PARTICLE(w, chr->pos, DDNET_PARTICLE_PLAYER_DEATH, character_id(chr));

  /* this is to rate limit respawning to 3 secs */
  player->previous_die_tick = player->die_tick;
  player->die_tick = server_tick(w);

  chr->alive = false;
  world_core_remove(w, character_id(chr));
  character_set_solo(w, chr, false);

  world_remove_entity(w, DDNET_ENTTYPE_CHARACTER, character_id(chr));
  teams_on_character_death(w, character_id(chr), weapon);
}
