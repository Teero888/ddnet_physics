/* Port of the entities in DDNet's src/game/server/entities/ other than the
 * character: projectiles, lasers, pickups, draggers, turrets and laser walls.
 *
 * Creating an entity can move world->entities, so every function here that
 * creates one re-reads its entity pointer afterwards. */
#include "internal.h"

#include <stdlib.h>

static tuning_t *get_tuning(world_t *w, int zone) { return &tuning_list(w)[zone]; }

/* A switched entity only acts on teams whose switch is active. */
static bool switch_inactive_for(world_t *w, const entity_t *ent, int team) {
  return ent->layer == LAYER_SWITCH && ent->number > 0 && !switchers(w)[ent->number].status[team];
}

/* Entities on conveyor tiles move every 0.15 seconds. */
static bool is_mover_tick(const world_t *w) { return server_tick(w) % (int)(SERVER_TICK_SPEED * 0.15f) == 0; }

/* ------------------------------------------------------------ projectile */

/* CProjectile::CProjectile */
void projectile_create(world_t *w, int type, int owner, vec2 pos, vec2 dir, int span, bool freeze,
                       bool explosive, int layer, int number, int bouncing) {
  int index = world_new_entity(w, DDNET_ENTITY_PROJECTILE, pos);
  if (index < 0)
    return;
  entity_t *ent = entity(w, index);
  ddnet_projectile_t *proj = &ent->u.projectile;
  proj->type = type;
  proj->direction = dir;
  proj->life_span = span;
  proj->owner = owner;
  proj->start_tick = server_tick(w);
  proj->explosive = explosive;
  ent->layer = layer;
  ent->number = number;
  proj->bouncing = bouncing;
  proj->freeze = freeze;

  proj->tune_zone = collision_is_tune(collision(w), collision_get_map_index(collision(w), ent->pos));

  world_insert_entity(w, DDNET_ENTTYPE_PROJECTILE, index);
}

/* CalcPos */
static vec2 calc_pos(vec2 pos, vec2 velocity, float curvature, float speed, float time) {
  vec2 n;
  time *= speed;
  n.x = pos.x + velocity.x * time;
  n.y = pos.y + velocity.y * time + curvature / 10000 * (time * time);
  return n;
}

/* CProjectile::GetPos */
static vec2 projectile_get_pos(world_t *w, const entity_t *ent, float time) {
  const ddnet_projectile_t *proj = &ent->u.projectile;
  float curvature = 0;
  float speed = 0;
  const tuning_t *tuning = get_tuning(w, proj->tune_zone);

  switch (proj->type) {
  case WEAPON_GRENADE:
    curvature = ddnet_tune(tuning->grenade_curvature);
    speed = ddnet_tune(tuning->grenade_speed);
    break;

  case WEAPON_SHOTGUN:
    curvature = ddnet_tune(tuning->shotgun_curvature);
    speed = ddnet_tune(tuning->shotgun_speed);
    break;

  case WEAPON_GUN:
    curvature = ddnet_tune(tuning->gun_curvature);
    speed = ddnet_tune(tuning->gun_speed);
    break;
  }

  return calc_pos(ent->pos, proj->direction, curvature, speed, time);
}

ddnet_vec2_t ddnet_projectile_get_pos(const ddnet_world_t *world, const ddnet_entity_t *projectile, float time) {
  return projectile_get_pos((world_t *)world, projectile, time);
}

/* CProjectile::Tick */
/* CProjectile::m_SoundImpact, which DDNet gives a projectile when it makes it: the explosion's sound for
 * grenades, and for the explosive bullets of the map if sv_shotgun_bullet_sound is set */
static int projectile_sound_impact(const world_t *w, const ddnet_projectile_t *proj) {
  if (proj->type == WEAPON_GRENADE)
    return DDNET_SOUND_GRENADE_EXPLODE;
  if (proj->type == WEAPON_SHOTGUN && proj->owner == -1)
    return !proj->explosive || w->config.sv_shotgun_bullet_sound ? DDNET_SOUND_GRENADE_EXPLODE : -1;
  return -1;
}

static void projectile_tick(world_t *w, int index) {
  const collision_t *col = collision(w);
  entity_t *ent = entity(w, index);
  ddnet_projectile_t *proj = &ent->u.projectile;

  float pt = (server_tick(w) - proj->start_tick - 1) / (float)SERVER_TICK_SPEED;
  float ct = (server_tick(w) - proj->start_tick) / (float)SERVER_TICK_SPEED;
  vec2 prev_pos = projectile_get_pos(w, ent, pt);
  vec2 cur_pos = projectile_get_pos(w, ent, ct);
  /* the trail a client draws behind a grenade */
  if (proj->type == WEAPON_GRENADE)
    create_particle(w, prev_pos, DDNET_PARTICLE_SMOKE, proj->owner);
  vec2 col_pos;
  vec2 new_pos;
  int collide = collision_intersect_line(col, prev_pos, cur_pos, &col_pos, &new_pos);
  character_t *owner_char = NULL;

  if (proj->owner >= 0)
    owner_char = get_player_char(w, proj->owner);

  character_t *target_chr = NULL;

  if (owner_char ? !owner_char->core.grenade_hit_disabled : w->config.sv_hit)
    target_chr = world_intersect_character(w, prev_pos, col_pos, proj->freeze ? 1.0f : 6.0f, &col_pos,
                                           owner_char, proj->owner, NULL);

  if (proj->life_span > -1)
    proj->life_span--;

  bool is_weapon_collide = false;
  if (owner_char && target_chr && owner_char->alive && target_chr->alive &&
      !character_can_collide(w, target_chr, proj->owner)) {
    is_weapon_collide = true;
  }
  if (owner_char && owner_char->alive) {
    /* the projectile stays visible to the team of its owner */
  } else if (proj->owner >= 0 && (proj->type != WEAPON_GRENADE || w->config.sv_destroy_bullets_on_death)) {
    ent->marked_for_destroy = true;
    return;
  }

  if (((target_chr && (owner_char ? !owner_char->core.grenade_hit_disabled
                                  : w->config.sv_hit || proj->owner == -1 || target_chr == owner_char)) ||
       collide || game_layer_clipped(w, cur_pos)) &&
      !is_weapon_collide) {
    if (proj->explosive &&
        (!target_chr || (target_chr && (!proj->freeze || (proj->type == WEAPON_SHOTGUN && collide))))) {
      int number = 1;
      if (w->config.bug_grenade_double_explosion && proj->life_span == -1)
        number = 2;
      for (int i = 0; i < number; i++) {
        create_explosion(w, col_pos, proj->owner, proj->type, proj->owner == -1,
                         (!target_chr ? -1 : character_team(w, target_chr)));
        create_sound(w, col_pos, projectile_sound_impact(w, proj), proj->owner);
      }
    } else if (proj->freeze) {
      int ents[MAX_CLIENTS];
      int num = world_find_characters(w, cur_pos, 1.0f, ents, MAX_CLIENTS);
      for (int i = 0; i < num; ++i) {
        character_t *chr = &w->characters[ents[i]];
        if (ent->layer != LAYER_SWITCH || (ent->layer == LAYER_SWITCH && ent->number > 0 &&
                                           switchers(w)[ent->number].status[character_team(w, chr)]))
          character_freeze(w, chr);
      }
    } else if (target_chr) {
      character_take_damage(w, target_chr, v2(0, 0), 0, proj->owner, proj->type);
    }

    if (owner_char && !game_layer_clipped(w, col_pos) &&
        ((proj->type == WEAPON_GRENADE && owner_char->core.has_telegun_grenade) ||
         (proj->type == WEAPON_GUN && owner_char->core.has_telegun_gun))) {
      int map_index = collision_get_pure_map_index(col, target_chr ? target_chr->pos : col_pos);
      int tile_f_index = collision_get_front_tile_index(col, map_index);
      bool is_switch_tele_gun = collision_get_switch_type(col, map_index) == TILE_ALLOW_TELE_GUN;
      bool is_blue_switch_tele_gun = collision_get_switch_type(col, map_index) == TILE_ALLOW_BLUE_TELE_GUN;

      if (is_switch_tele_gun || is_blue_switch_tele_gun) {
        /* Delay specifies which weapon the tile should work for.
         * Delay = 0 means all. */
        int delay = collision_get_switch_delay(col, map_index);

        if (delay == 1 && proj->type != WEAPON_GUN)
          is_switch_tele_gun = is_blue_switch_tele_gun = false;
        if (delay == 2 && proj->type != WEAPON_GRENADE)
          is_switch_tele_gun = is_blue_switch_tele_gun = false;
        if (delay == 3 && proj->type != WEAPON_LASER)
          is_switch_tele_gun = is_blue_switch_tele_gun = false;
      }

      if (tile_f_index == TILE_ALLOW_TELE_GUN || tile_f_index == TILE_ALLOW_BLUE_TELE_GUN ||
          is_switch_tele_gun || is_blue_switch_tele_gun || target_chr) {
        bool found;
        vec2 possible_pos;

        if (!collide)
          found = get_nearest_air_pos_player(w, target_chr ? target_chr->pos : col_pos, &possible_pos);
        else
          found = get_nearest_air_pos(w, new_pos, cur_pos, &possible_pos);

        if (found) {
          owner_char->tele_gun_pos = possible_pos;
          owner_char->tele_gun_teleport = true;
          owner_char->is_blue_tele_gun_teleport =
              tile_f_index == TILE_ALLOW_BLUE_TELE_GUN || is_blue_switch_tele_gun;
        }
      }
    }

    if (collide && proj->bouncing != 0) {
      proj->start_tick = server_tick(w);
      ent->pos = vadd(new_pos, vneg(vscale(proj->direction, 4)));
      if (proj->bouncing == 1)
        proj->direction.x = -proj->direction.x;
      else if (proj->bouncing == 2)
        proj->direction.y = -proj->direction.y;
      if (absolutef(proj->direction.x) < 1e-6f)
        proj->direction.x = 0;
      if (absolutef(proj->direction.y) < 1e-6f)
        proj->direction.y = 0;
      ent->pos = vadd(ent->pos, proj->direction);
    } else if (proj->type == WEAPON_GUN) {
      create_damage_ind(w, cur_pos, -atan2f(proj->direction.x, proj->direction.y), 10, proj->owner);
      ent->marked_for_destroy = true;
      return;
    } else {
      if (!proj->freeze) {
        ent->marked_for_destroy = true;
        return;
      }
    }
  }
  if (proj->life_span == -1) {
    if (proj->explosive) {
      create_explosion(w, col_pos, proj->owner, proj->type, proj->owner == -1,
                       (!owner_char ? -1 : character_team(w, owner_char)));
      create_sound(w, col_pos, projectile_sound_impact(w, proj), proj->owner);
    }
    ent->marked_for_destroy = true;
    return;
  }

  int x = collision_get_index(col, prev_pos, cur_pos);
  int z;
  if (w->config.sv_old_teleport_weapons)
    z = collision_is_teleport(col, x);
  else
    z = collision_is_teleport_weapon(col, x);
  if (z && col->tele_outs[z - 1].count != 0) {
    int tele_out = world_pick_tele_out(w, proj->owner, col->tele_outs[z - 1].count);
    ent->pos = col->tele_outs[z - 1].positions[tele_out];
    proj->start_tick = server_tick(w);
  }
}

/* ----------------------------------------------------------------- laser */

/* CInteractions::CanHit */
static bool interactions_can_hit(world_t *w, const ddnet_interactions_t *state, int client_id) {
  const player_t *player = &w->players[client_id];
  if (!player->active)
    return false;

  if (state->ddrace_team && teams_team(w, client_id) != state->ddrace_team)
    return false;
  if (state->solo && state->unique_owner_id != player->unique_id)
    return false;
  if (state->no_hit_others && state->unique_owner_id != player->unique_id)
    return false;
  if (state->no_hit_self && state->unique_owner_id == player->unique_id)
    return false;

  return true;
}

/* CLaser::SyncInteractState */
static void laser_sync_interact_state(world_t *w, entity_t *ent) {
  ddnet_laser_t *laser = &ent->u.laser;
  const player_t *owner_player =
      (laser->owner >= 0 && laser->owner < MAX_CLIENTS && w->players[laser->owner].active)
          ? &w->players[laser->owner]
          : NULL;

  character_t *owner_char = get_player_char(w, laser->owner);

  /* as long as the owner is connected
   * refill the state on tick
   * as soon as the owner disconnects keep that state */
  if (owner_player) {
    bool no_hit_others = w->config.sv_hit == 0;
    if (owner_char)
      no_hit_others = (laser->type == WEAPON_LASER && owner_char->core.laser_hit_disabled) ||
                      (laser->type == WEAPON_SHOTGUN && owner_char->core.shotgun_hit_disabled);

    bool no_hit_self = w->config.sv_old_laser || (laser->bounces == 0 && !laser->was_tele);

    /* CInteractions::FillOwnerConnected */
    laser->interact_state.owner_alive = owner_char && owner_char->alive;

    laser->interact_state.ddrace_team = teams_team(w, laser->owner);
    laser->interact_state.solo = owner_char && owner_char->core.solo;
    laser->interact_state.no_hit_others = no_hit_others;
    laser->interact_state.no_hit_self = no_hit_self;
  } else {
    /* CInteractions::FillOwnerDisconnected */
    laser->interact_state.owner_alive = false;
    laser->interact_state.owner_id = -1;
  }
}

/* CLaser::HitCharacter */
static bool laser_hit_character(world_t *w, entity_t *ent, vec2 from, vec2 to) {
  static const vec2 STACKED_LASER_SHOTGUN_BUG_SPEED = {-2147483648.0f, -2147483648.0f};
  ddnet_laser_t *laser = &ent->u.laser;

  laser_sync_interact_state(w, ent);

  vec2 at;
  character_t *owner_char = get_player_char(w, laser->owner);
  character_t *hit;
  bool dont_hit_self = w->config.sv_old_laser || (laser->bounces == 0 && !laser->was_tele);

  if (owner_char ? (!owner_char->core.laser_hit_disabled && laser->type == WEAPON_LASER) ||
                       (!owner_char->core.shotgun_hit_disabled && laser->type == WEAPON_SHOTGUN)
                 : w->config.sv_hit)
    hit = world_intersect_character(w, ent->pos, to, 0.f, &at, dont_hit_self ? owner_char : NULL,
                                    laser->owner, NULL);
  else
    hit = world_intersect_character(w, ent->pos, to, 0.f, &at, dont_hit_self ? owner_char : NULL,
                                    laser->owner, owner_char);

  if (!hit || !interactions_can_hit(w, &laser->interact_state, character_id(hit)))
    return false;
  laser->from = from;
  ent->pos = at;
  laser->energy = -1;

  if (laser->type == WEAPON_SHOTGUN) {
    float strength = ddnet_tune(tuning_list(w)[laser->tune_zone].shotgun_strength);
    const vec2 hit_pos = hit->core.pos;
    if (!w->config.sv_old_laser) {
      if (vne(laser->prev_pos, hit_pos))
        character_add_velocity(hit, vscale(vnormalize(vsub(laser->prev_pos, hit_pos)), strength));
      else
        character_set_raw_velocity(hit, STACKED_LASER_SHOTGUN_BUG_SPEED);
    } else if (w->config.sv_old_laser && owner_char) {
      if (vne(owner_char->core.pos, hit_pos))
        character_add_velocity(hit, vscale(vnormalize(vsub(owner_char->core.pos, hit_pos)), strength));
      else
        character_set_raw_velocity(hit, STACKED_LASER_SHOTGUN_BUG_SPEED);
    } else {
      /* Re-apply move restrictions as a part of 'shotgun bug' reproduction */
      character_apply_move_restrictions(hit);
    }
  } else if (laser->type == WEAPON_LASER) {
    character_unfreeze(w, hit);
  }
  hit->hit_num += 2;
  character_take_damage(w, hit, v2(0, 0), 0, laser->owner, laser->type);
  return true;
}

/* CLaser::DoBounce */
static void laser_do_bounce(world_t *w, entity_t *ent) {
  const collision_t *col = collision(w);
  ddnet_laser_t *laser = &ent->u.laser;

  laser->eval_tick = server_tick(w);

  if (laser->energy < 0) {
    ent->marked_for_destroy = true;
    return;
  }
  laser->prev_pos = ent->pos;
  vec2 coltile;

  int res;
  int z = 0;

  if (laser->was_tele) {
    laser->prev_pos = laser->tele_pos;
    ent->pos = laser->tele_pos;
    laser->tele_pos = v2(0, 0);
  }

  vec2 to = vadd(ent->pos, vscale(laser->dir, laser->energy));

  res = collision_intersect_line_tele_weapon(col, w->config.sv_old_teleport_weapons, ent->pos, to, &coltile,
                                             &to, &z);

  if (res) {
    if (!laser_hit_character(w, ent, ent->pos, to)) {
      /* intersected */
      laser->from = ent->pos;
      ent->pos = to;

      vec2 temp_pos = ent->pos;
      vec2 temp_dir = vscale(laser->dir, 4.0f);

      collision_move_point(col, &temp_pos, &temp_dir, 1.0f, NULL);

      ent->pos = temp_pos;
      laser->dir = vnormalize(temp_dir);

      const float distance = vdistance(laser->from, ent->pos);
      /* Prevent infinite bounces */
      if (distance == 0.0f && laser->zero_energy_bounce_in_last_tick)
        laser->energy = -1;
      else
        laser->energy -= distance + ddnet_tune(tuning_list(w)[laser->tune_zone].laser_bounce_cost);
      laser->zero_energy_bounce_in_last_tick = distance == 0.0f;

      if (res == TILE_TELEINWEAPON && col->tele_outs[z - 1].count != 0) {
        int tele_out = world_pick_tele_out(w, laser->owner, col->tele_outs[z - 1].count);
        laser->tele_pos = col->tele_outs[z - 1].positions[tele_out];
        laser->was_tele = true;
      } else {
        laser->bounces++;
        laser->was_tele = false;
      }

      int bounce_num = (int)ddnet_tune(tuning_list(w)[laser->tune_zone].laser_bounce_num);

      if (laser->bounces > bounce_num)
        laser->energy = -1;

      create_sound(w, ent->pos, DDNET_SOUND_LASER_BOUNCE, laser->owner);
    }
  } else {
    if (!laser_hit_character(w, ent, ent->pos, to)) {
      laser->from = ent->pos;
      ent->pos = to;
      laser->energy = -1;
    }
  }

  character_t *owner_char = get_player_char(w, laser->owner);
  if (laser->owner >= 0 && laser->energy <= 0 && !laser->teleport_cancelled && owner_char &&
      owner_char->alive && owner_char->core.has_telegun_laser && laser->type == WEAPON_LASER) {
    vec2 possible_pos;
    bool found = false;

    /* Check if the laser hits a player. */
    bool dont_hit_self = w->config.sv_old_laser || (laser->bounces == 0 && !laser->was_tele);
    vec2 at;
    character_t *hit;
    if (owner_char ? (!owner_char->core.laser_hit_disabled && laser->type == WEAPON_LASER) : w->config.sv_hit)
      hit = world_intersect_character(w, ent->pos, to, 0.f, &at, dont_hit_self ? owner_char : NULL,
                                      laser->owner, NULL);
    else
      hit = world_intersect_character(w, ent->pos, to, 0.f, &at, dont_hit_self ? owner_char : NULL,
                                      laser->owner, owner_char);

    if (hit)
      found = get_nearest_air_pos_player(w, hit->pos, &possible_pos);
    else
      found = get_nearest_air_pos(w, ent->pos, laser->from, &possible_pos);

    if (found) {
      owner_char->tele_gun_pos = possible_pos;
      owner_char->tele_gun_teleport = true;
      owner_char->is_blue_tele_gun_teleport = laser->is_blue_teleport;
    }
  } else if (laser->owner >= 0) {
    int map_index = collision_get_pure_map_index(col, coltile);
    int tile_f_index = collision_get_front_tile_index(col, map_index);
    bool is_switch_tele_gun = collision_get_switch_type(col, map_index) == TILE_ALLOW_TELE_GUN;
    bool is_blue_switch_tele_gun = collision_get_switch_type(col, map_index) == TILE_ALLOW_BLUE_TELE_GUN;
    int is_tele_in_weapon = collision_is_teleport_weapon(col, map_index);

    if (!is_tele_in_weapon) {
      if (is_switch_tele_gun || is_blue_switch_tele_gun) {
        /* Delay specifies which weapon the tile should work for.
         * Delay = 0 means all. */
        const int delay = collision_get_switch_delay(col, map_index);
        if ((delay != 3 && delay != 0) && laser->type == WEAPON_LASER)
          is_switch_tele_gun = is_blue_switch_tele_gun = false;
      }

      laser->is_blue_teleport = tile_f_index == TILE_ALLOW_BLUE_TELE_GUN || is_blue_switch_tele_gun;

      /* Teleport is canceled if the last bounce tile is not a TILE_ALLOW_TELE_GUN.
       * Teleport also works if laser didn't bounce. */
      laser->teleport_cancelled =
          laser->type == WEAPON_LASER &&
          (tile_f_index != TILE_ALLOW_TELE_GUN && tile_f_index != TILE_ALLOW_BLUE_TELE_GUN &&
           !is_switch_tele_gun && !is_blue_switch_tele_gun);
    }
  }
}

/* CLaser::CLaser. Both the laser rifle and the shotgun shoot this. */
void laser_create(world_t *w, vec2 pos, vec2 direction, float start_energy, int owner, int type) {
  int index = world_new_entity(w, DDNET_ENTITY_LASER, pos);
  if (index < 0)
    return;
  entity_t *ent = entity(w, index);
  ddnet_laser_t *laser = &ent->u.laser;
  ent->number = 0;
  ent->layer = LAYER_GAME;
  laser->owner = owner;
  laser->energy = start_energy;
  laser->dir = direction;
  laser->bounces = 0;
  laser->eval_tick = 0;
  laser->tele_pos = v2(0, 0);
  laser->was_tele = false;
  laser->type = type;
  laser->teleport_cancelled = false;
  laser->is_blue_teleport = false;
  laser->zero_energy_bounce_in_last_tick = false;
  laser->tune_zone = collision_is_tune(collision(w), collision_get_map_index(collision(w), ent->pos));

  const bool owner_connected = owner >= 0 && owner < MAX_CLIENTS && w->players[owner].active;
  laser->interact_state.owner_id = owner;
  laser->interact_state.unique_owner_id = owner_connected ? w->players[owner].unique_id : 0;
  laser_sync_interact_state(w, ent);

  world_insert_entity(w, DDNET_ENTTYPE_LASER, index);
  laser_do_bounce(w, ent);
}

/* CLaser::Tick */
static void laser_tick(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_laser_t *laser = &ent->u.laser;

  laser_sync_interact_state(w, ent);

  if (w->config.sv_destroy_lasers_on_death && laser->owner >= 0) {
    character_t *owner_char = get_player_char(w, laser->owner);
    if (!(owner_char && owner_char->alive))
      ent->marked_for_destroy = true;
  }

  float delay = ddnet_tune(tuning_list(w)[laser->tune_zone].laser_bounce_delay);

  if ((server_tick(w) - laser->eval_tick) > (SERVER_TICK_SPEED * delay / 1000.0f))
    laser_do_bounce(w, ent);
}

/* ---------------------------------------------------------------- pickup */

/* CPickup::CPickup */
void pickup_create(world_t *w, vec2 pos, int type, int subtype, int layer, int number) {
  int index = world_new_entity(w, DDNET_ENTITY_PICKUP, v2(0, 0));
  if (index < 0)
    return;
  entity_t *ent = entity(w, index);
  ent->u.pickup.core = v2(0.0f, 0.0f);
  ent->u.pickup.type = type;
  ent->u.pickup.subtype = subtype;
  ent->layer = layer;
  ent->number = number;
  world_insert_entity(w, DDNET_ENTTYPE_PICKUP, index);
  ent->pos = pos;
}

/* CPickup::Tick */
static void pickup_tick(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_pickup_t *pickup = &ent->u.pickup;

  /* CPickup::Move */
  if (is_mover_tick(w)) {
    collision_mover_speed(collision(w), ent->pos.x, ent->pos.y, &pickup->core);
    ent->pos = vadd(ent->pos, pickup->core);
  }

  /* Check if a player intersected us */
  enum { COLLISION_EXTRA_SIZE = 6 };
  int ents[MAX_CLIENTS];
  int num =
      world_find_characters(w, ent->pos, PICKUP_PROXIMITY_RADIUS + COLLISION_EXTRA_SIZE, ents, MAX_CLIENTS);
  for (int i = 0; i < num; ++i) {
    character_t *chr = &w->characters[ents[i]];

    if (chr->alive) {
      if (switch_inactive_for(w, ent, character_team(w, chr)))
        continue;
      bool sound = false;
      /* player picked us up, is someone was hooking us, let them go */
      switch (pickup->type) {
      case DDNET_POWERUP_FREEZE:
        if (character_freeze(w, chr))
          create_sound(w, ent->pos, DDNET_SOUND_PICKUP_HEALTH, character_id(chr));
        break;

      case DDNET_POWERUP_ARMOR:
        for (int j = WEAPON_SHOTGUN; j < NUM_WEAPONS; j++) {
          if (chr->core.weapons[j].got) {
            chr->core.weapons[j].got = false;
            chr->core.weapons[j].ammo = 0;
            sound = true;
          }
        }
        chr->core.ninja.activation_dir = v2(0, 0);
        chr->core.ninja.activation_tick = -500;
        chr->core.ninja.current_move_time = 0;
        if (sound) {
          chr->last_weapon = WEAPON_GUN;
          create_sound(w, ent->pos, DDNET_SOUND_PICKUP_ARMOR, character_id(chr));
        }
        if (chr->core.active_weapon >= WEAPON_SHOTGUN)
          chr->core.active_weapon = WEAPON_HAMMER;
        break;

      case DDNET_POWERUP_ARMOR_SHOTGUN:
        if (chr->core.weapons[WEAPON_SHOTGUN].got) {
          chr->core.weapons[WEAPON_SHOTGUN].got = false;
          chr->core.weapons[WEAPON_SHOTGUN].ammo = 0;
          chr->last_weapon = WEAPON_GUN;
          create_sound(w, ent->pos, DDNET_SOUND_PICKUP_ARMOR, character_id(chr));
        }
        if (chr->core.active_weapon == WEAPON_SHOTGUN)
          chr->core.active_weapon = WEAPON_HAMMER;
        break;

      case DDNET_POWERUP_ARMOR_GRENADE:
        if (chr->core.weapons[WEAPON_GRENADE].got) {
          chr->core.weapons[WEAPON_GRENADE].got = false;
          chr->core.weapons[WEAPON_GRENADE].ammo = 0;
          chr->last_weapon = WEAPON_GUN;
          create_sound(w, ent->pos, DDNET_SOUND_PICKUP_ARMOR, character_id(chr));
        }
        if (chr->core.active_weapon == WEAPON_GRENADE)
          chr->core.active_weapon = WEAPON_HAMMER;
        break;

      case DDNET_POWERUP_ARMOR_NINJA:
        chr->core.ninja.activation_dir = v2(0, 0);
        chr->core.ninja.activation_tick = -500;
        chr->core.ninja.current_move_time = 0;
        break;

      case DDNET_POWERUP_ARMOR_LASER:
        if (chr->core.weapons[WEAPON_LASER].got) {
          chr->core.weapons[WEAPON_LASER].got = false;
          chr->core.weapons[WEAPON_LASER].ammo = 0;
          chr->last_weapon = WEAPON_GUN;
          create_sound(w, ent->pos, DDNET_SOUND_PICKUP_ARMOR, character_id(chr));
        }
        if (chr->core.active_weapon == WEAPON_LASER)
          chr->core.active_weapon = WEAPON_HAMMER;
        break;

      case DDNET_POWERUP_WEAPON:
        if (pickup->subtype >= 0 && pickup->subtype < NUM_WEAPONS &&
            (!chr->core.weapons[pickup->subtype].got || chr->core.weapons[pickup->subtype].ammo != -1)) {
          character_give_weapon(w, chr, pickup->subtype, false);

          if (pickup->subtype == WEAPON_GRENADE)
            create_sound(w, ent->pos, DDNET_SOUND_PICKUP_GRENADE, character_id(chr));
          else if (pickup->subtype == WEAPON_SHOTGUN || pickup->subtype == WEAPON_LASER)
            create_sound(w, ent->pos, DDNET_SOUND_PICKUP_SHOTGUN, character_id(chr));
        }
        break;

      case DDNET_POWERUP_NINJA:
        /* activate ninja on target player */
        character_give_ninja(w, chr);
        break;

      default:
        break;
      }
    }
  }
}

/* --------------------------------------------------------------- dragger */

/* CDragger::CDragger */
bool dragger_create(world_t *w, vec2 pos, float strength, bool ignore_walls, int layer, int number) {
  ddnet_dragger_targets_t *targets =
      realloc(w->dragger_targets, (size_t)(w->num_draggers + 1) * sizeof(*w->dragger_targets));
  if (!targets)
    return false;
  w->dragger_targets = targets;

  int index = world_new_entity(w, DDNET_ENTITY_DRAGGER, pos);
  if (index < 0)
    return false;
  entity_t *ent = entity(w, index);
  ddnet_dragger_t *dragger = &ent->u.dragger;
  dragger->core = v2(0.0f, 0.0f);
  dragger->strength = strength;
  dragger->ignore_walls = ignore_walls;
  ent->layer = layer;
  ent->number = number;
  dragger->eval_tick = server_tick(w);

  dragger->targets = w->num_draggers++;

  for (int i = 0; i < MAX_CLIENTS; i++) {

    w->dragger_targets[dragger->targets].target_id_in_team[i] = -1;
    w->dragger_targets[dragger->targets].beam[i] = -1;
  }
  world_insert_entity(w, DDNET_ENTTYPE_LASER, index);
  return true;
}

/* CDraggerBeam::Reset */
static void dragger_beam_reset(world_t *w, entity_t *ent) {
  ddnet_dragger_beam_t *beam = &ent->u.dragger_beam;
  ent->marked_for_destroy = true;
  beam->active = false;

  /* CDragger::RemoveDraggerBeam */
  ddnet_dragger_targets_t *targets = &w->dragger_targets[entity(w, beam->dragger)->u.dragger.targets];
  targets->beam[beam->for_client_id] = -1;
}

/* CDraggerBeam::Tick */
static void dragger_beam_tick(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_dragger_beam_t *beam = &ent->u.dragger_beam;

  if (!beam->active)
    return;

  /* Drag only if the player is reachable and alive */
  character_t *target = get_player_char(w, beam->for_client_id);
  if (!target) {
    dragger_beam_reset(w, ent);
    return;
  }

  /* The following checks are necessary, because the checks in CDragger::LookForPlayersToDrag only take place
   * after CDraggerBeam::Tick and only every 150ms
   * When the dragger is disabled for the target player's team, the dragger beam dissolves. The check if a
   * dragger is disabled is only executed every 150ms, so the beam can stay activated up to 6 extra ticks */
  if (is_mover_tick(w)) {
    if (switch_inactive_for(w, ent, character_team(w, target))) {
      dragger_beam_reset(w, ent);
      return;
    }
  }

  /* When the dragger can no longer reach the target player, the dragger beam dissolves */
  if (vdistance(target->pos, ent->pos) >= w->config.sv_dragger_range || !target->alive ||
      (beam->ignore_walls
           ? collision_intersect_no_laser_no_walls(collision(w), ent->pos, target->pos, NULL, NULL)
           : collision_intersect_no_laser(collision(w), ent->pos, target->pos, NULL, NULL))) {
    dragger_beam_reset(w, ent);
    return;
  }
  /* In the center of the dragger a tee does not experience speed-up */
  else if (vdistance(target->pos, ent->pos) > 28) {
    character_add_velocity(target, vscale(vnormalize(vsub(ent->pos, target->pos)), beam->strength));
  }
}

/* CDraggerBeam::CDraggerBeam */
static int dragger_beam_create(world_t *w, int dragger_index, vec2 pos, float strength, bool ignore_walls,
                               int for_client_id, int layer, int number) {
  int index = world_new_entity(w, DDNET_ENTITY_DRAGGER_BEAM, pos);
  if (index < 0)
    return -1;
  entity_t *ent = entity(w, index);
  ddnet_dragger_beam_t *beam = &ent->u.dragger_beam;
  beam->dragger = dragger_index;
  beam->strength = strength;
  beam->ignore_walls = ignore_walls;
  beam->for_client_id = for_client_id;
  beam->active = true;
  ent->layer = layer;
  ent->number = number;
  beam->eval_tick = server_tick(w);
  world_insert_entity(w, DDNET_ENTTYPE_LASER, index);
  return index;
}

/* CDragger::LookForPlayersToDrag */
static void dragger_look_for_players_to_drag(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_dragger_t *dragger = &ent->u.dragger;
  /* this table never moves during a tick */
  ddnet_dragger_targets_t *targets = &w->dragger_targets[dragger->targets];

  /* Create a list of players who are in the range of the dragger */
  int players_in_range[MAX_CLIENTS];
  int num_players_in_range = world_find_characters(w, ent->pos, w->config.sv_dragger_range - PHYSICAL_SIZE,
                                                   players_in_range, MAX_CLIENTS);

  /* The closest player (within range) in a team is selected as the target */
  int closest_target_id_in_team[MAX_CLIENTS];
  bool can_still_be_team_target[MAX_CLIENTS];
  bool is_target[MAX_CLIENTS];
  int min_dist_in_team[MAX_CLIENTS];
  for (int i = 0; i < MAX_CLIENTS; i++) {
    can_still_be_team_target[i] = false;
    min_dist_in_team[i] = 0;
    is_target[i] = false;
    closest_target_id_in_team[i] = -1;
  }

  for (int i = 0; i < num_players_in_range; i++) {
    character_t *target = &w->characters[players_in_range[i]];
    const int target_team = character_team(w, target);
    /* If the dragger is disabled for the target's team, no dragger beam will be generated */
    if (switch_inactive_for(w, ent, target_team))
      continue;

    /* Dragger beams can be created only for reachable, alive players */
    int is_reachable =
        dragger->ignore_walls
            ? !collision_intersect_no_laser_no_walls(collision(w), ent->pos, target->pos, NULL, NULL)
            : !collision_intersect_no_laser(collision(w), ent->pos, target->pos, NULL, NULL);
    if (is_reachable && target->alive) {
      const int target_client_id = character_id(target);
      /* Solo players are dragged independently from the others */
      if (teams_get_solo(w, target_client_id)) {
        is_target[target_client_id] = true;
      } else {
        int distance = (int)vdistance(target->pos, ent->pos);
        if (min_dist_in_team[target_team] == 0 || min_dist_in_team[target_team] > distance) {
          min_dist_in_team[target_team] = distance;
          closest_target_id_in_team[target_team] = target_client_id;
        }
        can_still_be_team_target[target_client_id] = true;
      }
    }
  }

  /* Set the closest player for each team as a target if the team does not have a target player yet */
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if ((targets->target_id_in_team[i] != -1 && !can_still_be_team_target[targets->target_id_in_team[i]]) ||
        targets->target_id_in_team[i] == -1) {
      targets->target_id_in_team[i] = closest_target_id_in_team[i];
    }
    if (targets->target_id_in_team[i] != -1)
      is_target[targets->target_id_in_team[i]] = true;
  }

  for (int i = 0; i < MAX_CLIENTS; i++) {
    /* Create Dragger Beams which have not been created yet */
    if (is_target[i] && targets->beam[i] == -1) {
      ent = entity(w, index);
      dragger = &ent->u.dragger;
      targets->beam[i] = dragger_beam_create(w, index, ent->pos, dragger->strength, dragger->ignore_walls, i,
                                             ent->layer, ent->number);
      /* The generated dragger beam is placed in the first position in the tick sequence and would therefore
       * no longer be executed automatically in this tick. To execute the dragger beam nevertheless already
       * this tick we call it manually (we do this to keep the old game logic) */
      if (targets->beam[i] != -1)
        dragger_beam_tick(w, targets->beam[i]);
    }
    /* Remove dragger beams that have not yet been deleted */
    else if (!is_target[i] && targets->beam[i] != -1) {
      dragger_beam_reset(w, entity(w, targets->beam[i]));
    }
  }
}

/* CDragger::Tick */
static void dragger_tick(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_dragger_t *dragger = &ent->u.dragger;

  if (is_mover_tick(w)) {
    dragger->eval_tick = server_tick(w);
    collision_mover_speed(collision(w), ent->pos.x, ent->pos.y, &dragger->core);
    ent->pos = vadd(ent->pos, dragger->core);

    /* Adopt the new position for all outgoing laser beams */
    const ddnet_dragger_targets_t *targets = &w->dragger_targets[dragger->targets];
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (targets->beam[i] != -1)
        entity(w, targets->beam[i])->pos = ent->pos;
    }

    dragger_look_for_players_to_drag(w, index);
  }
}

/* ------------------------------------------------- turret (gun) & plasma */

/* CGun::CGun */
bool gun_create(world_t *w, vec2 pos, bool freeze, bool explosive, int layer, int number) {
  ddnet_gun_timers_t *timers = realloc(w->gun_timers, (size_t)(w->num_guns + 1) * sizeof(*w->gun_timers));
  if (!timers)
    return false;
  w->gun_timers = timers;

  int index = world_new_entity(w, DDNET_ENTITY_GUN, pos);
  if (index < 0)
    return false;
  entity_t *ent = entity(w, index);
  ddnet_gun_t *gun = &ent->u.gun;
  gun->core = v2(0.0f, 0.0f);
  gun->freeze = freeze;
  gun->explosive = explosive;
  ent->layer = layer;
  ent->number = number;
  gun->eval_tick = server_tick(w);

  gun->timers = w->num_guns++;

  for (int i = 0; i < MAX_CLIENTS; i++) {

    w->gun_timers[gun->timers].last_fire_team[i] = 0;
    w->gun_timers[gun->timers].last_fire_solo[i] = 0;
  }
  world_insert_entity(w, DDNET_ENTTYPE_LASER, index);
  return true;
}

/* CPlasma::CPlasma */
static void plasma_create(world_t *w, vec2 pos, vec2 dir, bool freeze, bool explosive, int for_client_id) {
  int index = world_new_entity(w, DDNET_ENTITY_PLASMA, pos);
  if (index < 0)
    return;
  entity_t *ent = entity(w, index);
  ddnet_plasma_t *plasma = &ent->u.plasma;
  ent->number = 0;
  ent->layer = LAYER_GAME;
  plasma->core = dir;
  plasma->freeze = freeze;
  plasma->explosive = explosive;
  plasma->for_client_id = for_client_id;
  plasma->eval_tick = server_tick(w);
  plasma->life_time = (int)(SERVER_TICK_SPEED * 1.5f);
  world_insert_entity(w, DDNET_ENTTYPE_LASER, index);
}

/* CGun::Fire */
static void gun_fire(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_gun_t *gun = &ent->u.gun;
  /* this table never moves during a tick */
  ddnet_gun_timers_t *timers = &w->gun_timers[gun->timers];

  /* Create a list of players who are in the range of the turret */
  int players_in_range[MAX_CLIENTS];
  int num_players_in_range =
      world_find_characters(w, ent->pos, w->config.sv_plasma_range, players_in_range, MAX_CLIENTS);

  /* The closest player (within range) in a team is selected as the target */
  int target_id_in_team[MAX_CLIENTS];
  bool is_target[MAX_CLIENTS];
  int min_dist_in_team[MAX_CLIENTS];
  for (int i = 0; i < MAX_CLIENTS; i++) {
    min_dist_in_team[i] = 0;
    is_target[i] = false;
    target_id_in_team[i] = -1;
  }

  for (int i = 0; i < num_players_in_range; i++) {
    character_t *target = &w->characters[players_in_range[i]];
    const int target_team = character_team(w, target);
    /* If the turret is disabled for the target's team, the turret will not fire */
    if (switch_inactive_for(w, ent, target_team))
      continue;

    /* Turrets can only shoot at a speed of sv_plasma_per_sec */
    const int target_client_id = character_id(target);
    const bool target_is_solo = teams_get_solo(w, target_client_id);
    if ((target_is_solo &&
         timers->last_fire_solo[target_client_id] + SERVER_TICK_SPEED / w->config.sv_plasma_per_sec >
             server_tick(w)) ||
        (!target_is_solo &&
         timers->last_fire_team[target_team] + SERVER_TICK_SPEED / w->config.sv_plasma_per_sec >
             server_tick(w))) {
      continue;
    }

    /* Turrets can shoot only at reachable, alive players */
    int is_reachable = !collision_intersect_line(collision(w), ent->pos, target->pos, NULL, NULL);
    if (is_reachable && target->alive) {
      /* Turrets fire on solo players regardless of the others */
      if (target_is_solo) {
        is_target[target_client_id] = true;
        timers->last_fire_solo[target_client_id] = server_tick(w);
      } else {
        int distance = (int)vdistance(target->pos, ent->pos);
        if (min_dist_in_team[target_team] == 0 || min_dist_in_team[target_team] > distance) {
          min_dist_in_team[target_team] = distance;
          target_id_in_team[target_team] = target_client_id;
        }
      }
    }
  }

  /* Set the closest player for each team as a target */
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (target_id_in_team[i] != -1) {
      is_target[target_id_in_team[i]] = true;
      timers->last_fire_team[i] = server_tick(w);
    }
  }

  for (int i = 0; i < MAX_CLIENTS; i++) {
    /* Fire at each target */
    if (is_target[i]) {
      character_t *target = get_player_char(w, i);
      ent = entity(w, index);
      gun = &ent->u.gun;
      plasma_create(w, ent->pos, vnormalize(vsub(target->pos, ent->pos)), gun->freeze, gun->explosive, i);
    }
  }
}

/* CGun::Tick */
static void gun_tick(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_gun_t *gun = &ent->u.gun;

  if (is_mover_tick(w)) {
    gun->eval_tick = server_tick(w);
    collision_mover_speed(collision(w), ent->pos.x, ent->pos.y, &gun->core);
    ent->pos = vadd(ent->pos, gun->core);
  }
  if (w->config.sv_plasma_per_sec > 0)
    gun_fire(w, index);
}

#define PLASMA_ACCEL 1.1f

/* CPlasma::HitCharacter */
static bool plasma_hit_character(world_t *w, entity_t *ent, character_t *target) {
  ddnet_plasma_t *plasma = &ent->u.plasma;
  vec2 intersect_pos;
  character_t *hit_player = world_intersect_character(w, ent->pos, vadd(ent->pos, plasma->core), 0.0f,
                                                      &intersect_pos, NULL, plasma->for_client_id, NULL);
  if (!hit_player)
    return false;

  plasma->freeze ? character_freeze(w, hit_player) : character_unfreeze(w, hit_player);
  if (plasma->explosive) {
    /* Plasma Turrets are very precise weapons only one tee gets speed from it,
     * other tees near the explosion remain unaffected */
    create_explosion(w, ent->pos, plasma->for_client_id, WEAPON_GRENADE, true, character_team(w, target));
  }
  ent->marked_for_destroy = true;
  return true;
}

/* CPlasma::HitObstacle */
static bool plasma_hit_obstacle(world_t *w, entity_t *ent, character_t *target) {
  ddnet_plasma_t *plasma = &ent->u.plasma;
  /* Check if the plasma bullet is stopped by a solid block or a laser stopper */
  int has_intersection =
      collision_intersect_no_laser(collision(w), ent->pos, vadd(ent->pos, plasma->core), NULL, NULL);
  if (has_intersection) {
    if (plasma->explosive) {
      /* Even in the case of an explosion due to a collision with obstacles, only one player is affected */
      create_explosion(w, ent->pos, plasma->for_client_id, WEAPON_GRENADE, true, character_team(w, target));
    }
    ent->marked_for_destroy = true;
    return true;
  }
  return false;
}

/* CPlasma::Tick */
static void plasma_tick(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_plasma_t *plasma = &ent->u.plasma;

  /* A plasma bullet has only a limited lifetime */
  if (plasma->life_time == 0) {
    ent->marked_for_destroy = true;
    return;
  }
  character_t *target = get_player_char(w, plasma->for_client_id);
  /* Without a target, a plasma bullet has no reason to live */
  if (!target) {
    ent->marked_for_destroy = true;
    return;
  }
  plasma->life_time--;

  /* CPlasma::Move */
  ent->pos = vadd(ent->pos, plasma->core);
  plasma->core = vscale(plasma->core, PLASMA_ACCEL);

  plasma_hit_character(w, ent, target);
  /* Plasma bullets may explode twice if they would hit both a player and an obstacle in the next move step */
  plasma_hit_obstacle(w, ent, target);
}

/* ------------------------------------------------- light (freeze laser) */

/* CLight::Move */
static void light_move(ddnet_light_t *light) {
  if (light->speed != 0) {
    if ((light->curve_length >= light->length && light->speed > 0) ||
        (light->curve_length <= 0 && light->speed < 0))
      light->speed = -light->speed;
    light->curve_length += light->speed * light->tick + light->length_l;
    light->length_l = 0;
    if (light->curve_length > light->length) {
      light->length_l = light->curve_length - light->length;
      light->curve_length = light->length;
    } else if (light->curve_length < 0) {
      light->length_l = 0 + light->curve_length;
      light->curve_length = 0;
    }
  }

  light->rotation += light->angular_speed * light->tick;
  if (light->rotation > PI * 2)
    light->rotation -= PI * 2;
  else if (light->rotation < 0)
    light->rotation += PI * 2;
}

/* CLight::Step */
void light_step(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_light_t *light = &ent->u.light;

  light_move(light);
  const vec2 direction = v2(sinf(light->rotation), cosf(light->rotation));
  const vec2 next_position = vadd(ent->pos, vscale(vnormalize(direction), light->curve_length));
  collision_intersect_no_laser(collision(w), ent->pos, next_position, &light->to, NULL);
}

/* CLight::CLight. The caller sets angular_speed, speed and curve_length afterwards. */
int light_create(world_t *w, vec2 pos, float rotation, int length, int layer, int number) {
  int index = world_new_entity(w, DDNET_ENTITY_LIGHT, pos);
  if (index < 0)
    return -1;
  entity_t *ent = entity(w, index);
  ddnet_light_t *light = &ent->u.light;
  light->to = v2(0.0f, 0.0f);
  light->core = v2(0.0f, 0.0f);
  ent->layer = layer;
  ent->number = number;
  light->tick = (int)(SERVER_TICK_SPEED * 0.15f);
  light->rotation = rotation;
  light->length = length;
  light->eval_tick = server_tick(w);
  world_insert_entity(w, DDNET_ENTTYPE_LASER, index);
  light_step(w, index);
  return index;
}

/* CLight::HitCharacter with CGameWorld::IntersectedCharacters */
static void light_hit_character(world_t *w, entity_t *ent) {
  const ddnet_light_t *light = &ent->u.light;
  for (int id = w->first_entity[DDNET_ENTTYPE_CHARACTER]; id != -1; id = w->characters[id].link.next) {
    character_t *chr = &w->characters[id];
    vec2 intersect_pos;
    if (closest_point_on_line(ent->pos, light->to, chr->pos, &intersect_pos)) {
      float len = vdistance(chr->pos, intersect_pos);
      if (len < PHYSICAL_SIZE + 0.0f) {
        if (switch_inactive_for(w, ent, character_team(w, chr)))
          continue;
        character_freeze(w, chr);
      }
    }
  }
}

/* CLight::Tick */
static void light_tick(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_light_t *light = &ent->u.light;

  if (is_mover_tick(w)) {
    light->eval_tick = server_tick(w);
    collision_mover_speed(collision(w), ent->pos.x, ent->pos.y, &light->core);
    ent->pos = vadd(ent->pos, light->core);
    light_step(w, index);
  }

  light_hit_character(w, ent);
}

/* -------------------------------------------------------------- dispatch */

void entity_tick(world_t *w, int index) {
  switch (entity(w, index)->kind) {
  case DDNET_ENTITY_PROJECTILE:
    projectile_tick(w, index);
    break;
  case DDNET_ENTITY_LASER:
    laser_tick(w, index);
    break;
  case DDNET_ENTITY_PICKUP:
    pickup_tick(w, index);
    break;
  case DDNET_ENTITY_DRAGGER:
    dragger_tick(w, index);
    break;
  case DDNET_ENTITY_DRAGGER_BEAM:
    dragger_beam_tick(w, index);
    break;
  case DDNET_ENTITY_GUN:
    gun_tick(w, index);
    break;
  case DDNET_ENTITY_PLASMA:
    plasma_tick(w, index);
    break;
  case DDNET_ENTITY_LIGHT:
    light_tick(w, index);
    break;
  case DDNET_ENTITY_FREE:
    break;
  }
}

/* CEntity::GetOwnerId */
int entity_owner_id(const entity_t *ent) {
  switch (ent->kind) {
  case DDNET_ENTITY_PROJECTILE:
    return ent->u.projectile.owner;
  case DDNET_ENTITY_LASER:
    return ent->u.laser.owner;
  default:
    return -1;
  }
}
