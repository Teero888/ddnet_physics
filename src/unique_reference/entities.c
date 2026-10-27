/* Port of the entities in src/game/server/entities/ of Unique Race other
 * than the character, as far as its maps have them: the grenade (CProjectile)
 * and the pickups (CPickup).
 *
 * Creating an entity can move world->entities, so every function here that
 * creates one re-reads its entity pointer afterwards. */
#include "internal.h"

static tuning_t *get_tuning(world_t *w, int zone) { return &tuning_list(w)[zone]; }

/* ------------------------------------------------------------ projectile */

/* CProjectile::CProjectile, for the grenade of the tee */
void projectile_create(world_t *w, vec2 pos, vec2 dir, int span) {
  int index = world_new_entity(w, DDNET_ENTITY_PROJECTILE, pos);
  if (index < 0)
    return;
  entity_t *ent = entity(w, index);
  ddnet_projectile_t *proj = &ent->u.projectile;
  proj->direction = dir;
  proj->life_span = span;
  proj->start_tick = server_tick(w);

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
  const tuning_t *tuning = get_tuning(w, proj->tune_zone);
  float curvature = ddnet_tune(tuning->grenade_curvature);
  float speed = ddnet_tune(tuning->grenade_speed);
  return calc_pos(ent->pos, proj->direction, curvature, speed, time);
}

ddnet_vec2_t ddnet_projectile_get_pos(const ddnet_world_t *world, const ddnet_entity_t *projectile, float time) {
  return projectile_get_pos((world_t *)world, projectile, time);
}

/* CProjectile::Tick. A grenade only hits its own tee, which the server
 * leaves out of what a projectile can fly into (CGameWorld::IntersectCharacter
 * with NotThis), so it explodes where it hits the map, leaves it, or runs out
 * of time. (The weapon teleporters at the end are left out, see
 * ddnet_map_unsupported().) */
static void projectile_tick(world_t *w, int index) {
  const collision_t *col = collision(w);
  entity_t *ent = entity(w, index);
  ddnet_projectile_t *proj = &ent->u.projectile;

  float pt = (server_tick(w) - proj->start_tick - 1) / (float)SERVER_TICK_SPEED;
  float ct = (server_tick(w) - proj->start_tick) / (float)SERVER_TICK_SPEED;
  vec2 prev_pos = projectile_get_pos(w, ent, pt);
  vec2 cur_pos = projectile_get_pos(w, ent, ct);
  /* the trail a client draws behind a grenade */
  create_particle(w, prev_pos, DDNET_PARTICLE_SMOKE);
  vec2 col_pos;
  vec2 new_pos;
  int collide = collision_intersect_line(col, prev_pos, cur_pos, &col_pos, &new_pos);
  character_t *owner_char = get_player_char(w);

  if (proj->life_span > -1)
    proj->life_span--;

  if (owner_char && owner_char->alive) {
    /* the projectile stays visible to the team of its owner */
  } else if (w->config.sv_kill_grenades) {
    /* (sv_destroy_bullets_on_death, which Unique Race sets to sv_kill_grenades) */
    ent->marked_for_destroy = true;
    return;
  }

  if (collide || game_layer_clipped(w, cur_pos)) {
    create_explosion(w, col_pos, WEAPON_GRENADE);
    create_sound(w, col_pos, DDNET_SOUND_GRENADE_EXPLODE);

    ent->marked_for_destroy = true;
    return;
  }
  if (proj->life_span == -1) {
    create_explosion(w, col_pos, WEAPON_GRENADE);
    create_sound(w, col_pos, DDNET_SOUND_GRENADE_EXPLODE);
    ent->marked_for_destroy = true;
    return;
  }
}

/* ---------------------------------------------------------------- pickup */

/* CPickup::CPickup. Returns false if memory ran out. */
bool pickup_create(world_t *w, vec2 pos, int type, int subtype) {
  int index = world_new_entity(w, DDNET_ENTITY_PICKUP, v2(0, 0));
  if (index < 0)
    return false;
  entity_t *ent = entity(w, index);
  ent->u.pickup.type = type;
  ent->u.pickup.subtype = subtype;
  ent->u.pickup.spawn_tick = -1;
  world_insert_entity(w, DDNET_ENTTYPE_PICKUP, index);
  ent->pos = pos;
  return true;
}

/* CPickup::Tick */
static void pickup_tick(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_pickup_t *pickup = &ent->u.pickup;

  /* (the maps have no tiles that move pickups) */

  /* wait for */
  if (w->config.sv_health_and_ammo) {
    if (!get_player_char(w)) {
      pickup->spawn_tick = -1;
    } else if (pickup->spawn_tick > 0 && server_tick(w) > pickup->spawn_tick) {
      /* respawn */
      pickup->spawn_tick = -1;

      if (pickup->type == DDNET_UNIQUE_POWERUP_WEAPON)
        create_sound(w, ent->pos, DDNET_SOUND_WEAPON_SPAWN);
    }
  }

  /* Check if a player intersected us */
  enum { COLLISION_EXTRA_SIZE = 6 };
  if (!world_find_character(w, ent->pos, PICKUP_PROXIMITY_RADIUS + COLLISION_EXTRA_SIZE))
    return;
  character_t *chr = &w->character;

  if (chr->alive) {
    if (w->config.sv_health_and_ammo && pickup->spawn_tick != -1)
      return;

    int respawn_time = -1;
    bool sound = false;
    /* player picked us up, is someone was hooking us, let them go */
    switch (pickup->type) {
    case DDNET_UNIQUE_POWERUP_HEALTH:
      if (w->config.sv_health_and_ammo) {
        if (character_increase_health(chr, 1)) {
          create_sound(w, ent->pos, DDNET_SOUND_PICKUP_HEALTH);
          respawn_time = PICKUP_RESPAWN_TIME;
        }
      } else {
        if (character_freeze(w, chr))
          create_sound(w, ent->pos, DDNET_SOUND_PICKUP_HEALTH);
      }
      break;

    case DDNET_UNIQUE_POWERUP_ARMOR:
      if (w->config.sv_health_and_ammo) {
        if (character_increase_armor(chr, 1)) {
          create_sound(w, ent->pos, DDNET_SOUND_PICKUP_ARMOR);
          respawn_time = PICKUP_RESPAWN_TIME;
        }
      } else {
        for (int j = WEAPON_SHOTGUN; j < NUM_WEAPONS; j++) {
          if (chr->core.weapons[j].got) {
            chr->core.weapons[j].got = false;
            chr->core.weapons[j].ammo = 0;
            sound = true;
          }
        }
        /* (and the ninja of the tee is reset, which it never has) */
        if (sound) {
          chr->last_weapon = WEAPON_GUN;
          create_sound(w, ent->pos, DDNET_SOUND_PICKUP_ARMOR);
        }
        if (chr->core.active_weapon >= WEAPON_SHOTGUN)
          chr->core.active_weapon = WEAPON_HAMMER;
      }
      break;

    case DDNET_UNIQUE_POWERUP_WEAPON:
      if (pickup->subtype >= 0 && pickup->subtype < NUM_WEAPONS &&
          (!chr->core.weapons[pickup->subtype].got || chr->core.weapons[pickup->subtype].ammo != -1) &&
          (!w->config.sv_health_and_ammo || chr->core.weapons[pickup->subtype].ammo != GRENADE_MAX_AMMO)) {
        character_give_weapon(w, chr, pickup->subtype);

        if (w->config.sv_health_and_ammo)
          respawn_time = PICKUP_RESPAWN_TIME;

        create_sound(w, ent->pos, DDNET_SOUND_PICKUP_GRENADE);
      }
      break;

    default:
      break;
    }

    if (respawn_time >= 0 && w->config.sv_health_and_ammo)
      pickup->spawn_tick = server_tick(w) + SERVER_TICK_SPEED * respawn_time;
  }
}

/* --------------------------------------------------------------- dispatch */

void entity_tick(world_t *w, int index) {
  switch (entity(w, index)->kind) {
  case DDNET_ENTITY_PROJECTILE:
    projectile_tick(w, index);
    break;
  case DDNET_ENTITY_PICKUP:
    pickup_tick(w, index);
    break;
  default:
    break;
  }
}
