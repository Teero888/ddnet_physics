/* Port of the entities in DDNet's src/game/server/entities/ other than the
 * character: projectiles, lasers, pickups, draggers, turrets and laser walls.
 *
 * Creating an entity can move world->entities, so every function here that
 * creates one re-reads its entity pointer afterwards. */
#include "internal.h"

#include <limits.h>
#include <stdlib.h>

/* A switched entity only acts on teams whose switch is active. */
static bool switch_inactive_for(world_t *w, const entity_t *ent, int team) {
  return ent->layer == LAYER_SWITCH && ent->number > 0 && !switch_status(w, ent->number, team);
}

/* Entities on conveyor tiles move every 0.15 seconds. */
/* how many ticks the way of a projectile is looked at in advance */
enum { PROJECTILE_LOOKAHEAD = 60 };

/* ascending, for a few ids */
static void sort_ids(int *ids, int num) {
  for (int i = 1; i < num; i++) {
    const int id = ids[i];
    int k = i;
    for (; k > 0 && ids[k - 1] > id; k--)
      ids[k] = ids[k - 1];
    ids[k] = id;
  }
}

static bool is_mover_tick(const world_t *w) { return server_tick(w) % (int)(SERVER_TICK_SPEED * 0.15f) == 0; }

/* ------------------------------------------------------------ projectile */

/* Puts a projectile into the set of the awake or of the parked ones, whichever it is. */
static void projectile_set_insert(world_t *w, int index) {
  ddnet_projectile_t *proj = &entity(w, index)->u.projectile;
  /* (the lazy ones are in neither) */
  if (w->projectile_sets_failed || proj->lazy)
    return;
  if (!proj->parked) {
    if (w->num_projectile_awake == w->projectile_awake_capacity) {
      const int capacity = w->projectile_awake_capacity ? w->projectile_awake_capacity * 2 : 64;
      int *awake = realloc(w->projectile_awake, (size_t)capacity * sizeof(*awake));
      if (!awake) {
        w->projectile_sets_failed = true;
        return;
      }
      w->projectile_awake = awake;
      w->projectile_awake_capacity = capacity;
    }
    proj->set_slot = w->num_projectile_awake;
    w->projectile_awake[w->num_projectile_awake++] = index;
  } else {
    if (w->num_projectile_parked == w->projectile_parked_capacity) {
      const int capacity = w->projectile_parked_capacity ? w->projectile_parked_capacity * 2 : 64;
      ddnet_parked_projectile_t *parked = realloc(w->projectile_parked, (size_t)capacity * sizeof(*parked));
      if (!parked) {
        w->projectile_sets_failed = true;
        return;
      }
      w->projectile_parked = parked;
      w->projectile_parked_capacity = capacity;
    }
    world_touch(w, TOUCH_PARKED, 0);
    proj->set_slot = w->num_projectile_parked;
    ddnet_parked_projectile_t *parked = &w->projectile_parked[w->num_projectile_parked++];
    parked->min = proj->orbit_min;
    parked->max = proj->orbit_max;
    parked->index = index;
  }
}

/* Takes a projectile out of the set it is in. */
void projectile_set_erase(world_t *w, int index) {
  ddnet_projectile_t *proj = &entity(w, index)->u.projectile;
  if (w->projectile_sets_failed || proj->lazy)
    return;
  if (!proj->parked) {
    const int last = w->projectile_awake[--w->num_projectile_awake];
    w->projectile_awake[proj->set_slot] = last;
    entity(w, last)->u.projectile.set_slot = proj->set_slot;
  } else {
    world_touch(w, TOUCH_PARKED, 0);
    const ddnet_parked_projectile_t last = w->projectile_parked[--w->num_projectile_parked];
    w->projectile_parked[proj->set_slot] = last;
    entity(w, last.index)->u.projectile.set_slot = proj->set_slot;
  }
}

static bool projectile_can_be_lazy(const world_t *w, const ddnet_projectile_t *proj);
static bool projectile_lazy_now(world_t *w, const ddnet_projectile_t *proj);
static void projectile_schedule(world_t *w, int index, int now);

/* The memo for lines of sight of the world, NULL without memory. */
static ddnet_sight_memo_t *sight_memo(world_t *w) {
  if (!w->sight_memo)
    w->sight_memo = calloc(1, sizeof(*w->sight_memo));
  /* A world that was copied to from a world of another map has kept what it
   * knew about its old one (ddnet_world_copy() does not copy this). */
  if (w->sight_memo && w->sight_memo->universe != w->memo_universe) {
    memset(w->sight_memo, 0, sizeof(*w->sight_memo));
    w->sight_memo->universe = w->memo_universe;
  }
  return w->sight_memo;
}

/* CProjectile::CProjectile */

DDNET_HOT void projectile_create(world_t *w, int type, int owner, vec2 pos, vec2 dir, int span, bool freeze,
                                 bool explosive, int layer, int number, int bouncing) {
  int index = world_new_entity(w, DDNET_ENTITY_PROJECTILE, pos);
  if (index < 0)
    return;
  entity_t *ent = entity(w, index);
  ddnet_projectile_t *proj = &ent->u.projectile;
  proj->type = type;
  proj->direction = dir;
  /* m_LifeSpan is decremented on every tick while it is above -1, and the
   * projectile ends when it is -1 after that */
  proj->expire_tick = span >= 0 ? server_tick(w) + 1 + span : (span == -1 ? server_tick(w) + 1 : INT_MAX);
  proj->next_tick = 0;
  proj->due_tick = -1;
  proj->seq = w->projectile_seq++;
  proj->owner = owner;
  proj->start_tick = server_tick(w);
  proj->explosive = explosive;
  ent->layer = layer;
  ent->number = number;
  proj->bouncing = bouncing;
  proj->freeze = freeze;

  proj->tune_zone = collision_is_tune(collision(w), collision_get_map_index(collision(w), ent->pos));

  if (projectile_can_be_lazy(w, proj) && projectile_lazy_now(w, proj)) {
    /* Nobody to hit: it is lazy from the start, and nothing has to look at
     * the projectiles because of it. Its first tick would be the next one. */
    proj->lazy = true;
    proj->lazy_tick = proj->start_tick;
    w->num_lazy_projectiles++;
    proj->next_tick = mini(proj->expire_tick, proj->start_tick + DDNET_PROJECTILE_DUE_SIZE - 1);
    proj->next_is_event = false;
    projectile_schedule(w, index, proj->start_tick);
  } else {
    w->projectile_wakeup = 0;
  }
  projectile_set_insert(w, index);
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

ddnet_vec2_t ddnet_projectile_get_pos(const ddnet_world_t *world, const ddnet_entity_t *projectile,
                                      float time) {
  return projectile_get_pos((world_t *)world, projectile, time);
}

/* CProjectile::m_SoundImpact, which DDNet gives a projectile when it makes it: the explosion's sound for
 * grenades, and for the explosive bullets of the map if sv_shotgun_bullet_sound is set */
static inline int projectile_sound_impact(const world_t *w, const ddnet_projectile_t *proj) {
  if (proj->type == WEAPON_GRENADE)
    return DDNET_SOUND_GRENADE_EXPLODE;
  if (proj->type == WEAPON_SHOTGUN && proj->owner == -1)
    return !proj->explosive || w->config.sv_shotgun_bullet_sound ? DDNET_SOUND_GRENADE_EXPLODE : -1;
  return -1;
}

/* CProjectile::Tick */
static void projectile_tick(world_t *w, int index) {
  PROF_COUNT(13, "projectile exact ticks");
  w->projectile_only_bounced = false;
  const collision_t *col = collision(w);
  entity_t *ent = entity(w, index);
  ddnet_projectile_t *proj = &ent->u.projectile;

  float pt = (server_tick(w) - proj->start_tick - 1) / (float)SERVER_TICK_SPEED;
  float ct = (server_tick(w) - proj->start_tick) / (float)SERVER_TICK_SPEED;
  vec2 prev_pos = projectile_get_pos(w, ent, pt);
  vec2 cur_pos = projectile_get_pos(w, ent, ct);
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

  const bool expired = server_tick(w) >= proj->expire_tick;

  bool is_weapon_collide = false;
  if (owner_char && target_chr && owner_char->alive && target_chr->alive &&
      !character_can_collide(w, target_chr, proj->owner)) {
    is_weapon_collide = true;
  }
  if (owner_char && owner_char->alive) {
    /* the projectile stays visible to the team of its owner */
  } else if (proj->owner >= 0 && (proj->type != WEAPON_GRENADE || w->config.sv_destroy_bullets_on_death)) {
    entity_mark_for_destroy(w, ent);
    return;
  }

  if (((target_chr && (owner_char ? !owner_char->core.grenade_hit_disabled
                                  : w->config.sv_hit || proj->owner == -1 || target_chr == owner_char)) ||
       collide || game_layer_clipped(w, cur_pos)) &&
      !is_weapon_collide) {
    if (proj->explosive &&
        (!target_chr || (target_chr && (!proj->freeze || (proj->type == WEAPON_SHOTGUN && collide))))) {
      int number = 1;
      if (w->config.bug_grenade_double_explosion && expired)
        number = 2;
      for (int i = 0; i < number; i++) {
        create_explosion(w, col_pos, proj->owner, proj->type, proj->owner == -1,
                         (!target_chr ? -1 : character_team(w, target_chr)));
        EMIT_SOUND(w, col_pos, projectile_sound_impact(w, proj), proj->owner);
      }
    } else if (proj->freeze) {
      int ents[MAX_CLIENTS];
      int num = world_find_characters(w, cur_pos, 1.0f, ents, MAX_CLIENTS);
      for (int i = 0; i < num; ++i) {
        character_t *chr = &w->characters[ents[i]];
        if (ent->layer != LAYER_SWITCH || (ent->layer == LAYER_SWITCH && ent->number > 0 &&
                                           switch_status(w, ent->number, character_team(w, chr))))
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
      w->projectile_only_bounced = !target_chr;
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
      EMIT_DAMAGE_IND(w, cur_pos, -atan2f(proj->direction.x, proj->direction.y), 10, proj->owner);
      entity_mark_for_destroy(w, ent);
      return;
    } else {
      if (!proj->freeze) {
        entity_mark_for_destroy(w, ent);
        return;
      }
    }
  }
  if (expired) {
    if (proj->explosive) {
      create_explosion(w, col_pos, proj->owner, proj->type, proj->owner == -1,
                       (!owner_char ? -1 : character_team(w, owner_char)));
      EMIT_SOUND(w, col_pos, projectile_sound_impact(w, proj), proj->owner);
    }
    entity_mark_for_destroy(w, ent);
    return;
  }

  /* only teleporters are of interest, and they are rare */
  int x = collision_rect_is_free_of_teleporters(col, prev_pos, cur_pos)
              ? -1
              : collision_get_index(col, prev_pos, cur_pos);
  int z;
  if (w->config.sv_old_teleport_weapons)
    z = collision_is_teleport(col, x);
  else
    z = collision_is_teleport_weapon(col, x);
  if (z && col->tele_outs[z - 1].count != 0) {
    int tele_out = world_pick_tele_out(w, proj->owner, col->tele_outs[z - 1].count);
    ent->pos = col->tele_outs[z - 1].positions[tele_out];
    proj->start_tick = server_tick(w);
    w->projectile_only_bounced = false;
  }
}

/* The entry of the memo for a projectile that started `age` ticks ago, and
 * whether it is known. question: 0 = what the map does next, 1 = how it
 * bounces. NULL if there is no memo. */
static ddnet_projectile_memo_entry_t *projectile_memo(world_t *w, const entity_t *ent, int age, int question,
                                                      bool *known) {
  const ddnet_projectile_t *proj = &ent->u.projectile;
  *known = false;
  if (!w->projectile_memo)
    w->projectile_memo = calloc(1, sizeof(*w->projectile_memo));
  if (!w->projectile_memo)
    return NULL;
  if (w->projectile_memo->universe != w->memo_universe) {
    memset(w->projectile_memo, 0, sizeof(*w->projectile_memo));
    w->projectile_memo->universe = w->memo_universe;
  }
  /* everything a projectile is besides where it started */
  const int kind =
      proj->type | proj->bouncing << 8 | proj->freeze << 16 | proj->explosive << 17 | question << 18;
  uint32_t key[4];
  memcpy(key, &ent->pos, sizeof(ent->pos));
  memcpy(key + 2, &proj->direction, sizeof(proj->direction));
  const uint32_t hash = key[0] * 0x9E3779B1u ^ key[1] * 0x85EBCA77u ^ key[2] * 0xC2B2AE3Du ^
                        key[3] * 0x27D4EB2Fu ^ (uint32_t)age * 0x165667B1u ^ (uint32_t)question * 0x7F4A7C15u;
  ddnet_projectile_memo_entry_t *memo =
      &w->projectile_memo->entries[(hash >> 16) % DDNET_PROJECTILE_MEMO_SIZE];
  *known = memo->ticks && memcmp(&memo->pos, &ent->pos, sizeof(ent->pos)) == 0 &&
           memcmp(&memo->direction, &proj->direction, sizeof(proj->direction)) == 0 && memo->kind == kind &&
           memo->tune_zone == proj->tune_zone && memo->age == age;
  if (!*known) {
    /* the caller fills in the answer */
    memo->pos = ent->pos;
    memo->direction = proj->direction;
    memo->kind = kind;
    memo->tune_zone = proj->tune_zone;
    memo->age = age;
    memo->ticks = 0;
  }
  return memo;
}

/* The first tick after `now` on which the map could do something to a
 * projectile: it runs into something solid, leaves the map, passes a
 * teleporter or its lifetime ends. A projectile flies on a fixed path, so this
 * is known in advance, and until then only tees can interfere with it. The
 * answer may be too early, never too late: on that tick the projectile ticks
 * like in DDNet, and if nothing happens this is asked again. Also sets
 * next_is_event and the rectangle the projectile stays in until then. */
static int projectile_next_static_tick(world_t *w, entity_t *ent, int now) {
  PROF_COUNT(15, "projectile lookaheads");
  const collision_t *col = collision(w);
  ddnet_projectile_t *proj = &ent->u.projectile;
  const float map_width = col->width * 32.0f, map_height = col->height * 32.0f;
  /* not further than this, to keep the rectangle small and this cheap */
  const int last_tick = mini(proj->expire_tick, now + PROJECTILE_LOOKAHEAD);

  /* A projectile of the map whose lifetime does not end in that time: the
   * answer only depends on where it started and when, and may be known. */
  ddnet_projectile_memo_entry_t *memo = NULL;
  if (proj->owner < 0 && proj->expire_tick > now + PROJECTILE_LOOKAHEAD) {
    bool known;
    memo = projectile_memo(w, ent, now - proj->start_tick, 0, &known);
    if (known) {
      PROF_COUNT(16, "projectile memo hits");
      proj->reach_min = memo->reach_min;
      proj->reach_max = memo->reach_max;
      proj->next_is_event = !(memo->ticks & DDNET_PROJECTILE_MEMO_NO_EVENT);
      return now + (memo->ticks & ~DDNET_PROJECTILE_MEMO_NO_EVENT);
    }
  }

  /* all positions that are looked at, which are those of every tick up to the result */
  vec2 reach_min = v2(INFINITY, INFINITY), reach_max = v2(-INFINITY, -INFINITY);
#define REACH(p)                                                                                             \
  (reach_min.x = minf(reach_min.x, (p).x), reach_min.y = minf(reach_min.y, (p).y),                           \
   reach_max.x = maxf(reach_max.x, (p).x), reach_max.y = maxf(reach_max.y, (p).y))

  /* CProjectile::GetPos with the tuning looked up once */
  float curvature = 0, speed = 0;
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
#define POS_AFTER(t)                                                                                         \
  calc_pos(ent->pos, proj->direction, curvature, speed, ((t) - proj->start_tick) / (float)SERVER_TICK_SPEED)

  int tick = now + 1;
  /* where the projectile is before this tick */
  vec2 base = POS_AFTER(tick - 1);
  while (tick < last_tick) {
    REACH(base);
    if (!(base.x >= 0 && base.x < map_width && base.y >= 0 && base.y < map_height)) {
      /* the tick that has to be looked at goes up to here */
      REACH(POS_AFTER(tick));
      break;
    }
    const int dist = col->dist_projectile[(int)base.y / 32 * col->width + (int)base.x / 32];
    if (dist < 2) {
      /* Close to something. First the tiles of several ticks at once: the
       * way of a tick is the line between two of these positions, so all of
       * it is in the rectangle around them. */
      enum { SEVERAL = 8 };
      if (tick + SEVERAL <= last_tick) {
        vec2 several_min = base, several_max = base, last = base;
        for (int i = 0; i < SEVERAL; i++) {
          last = POS_AFTER(tick + i);
          several_min = v2(minf(several_min.x, last.x), minf(several_min.y, last.y));
          several_max = v2(maxf(several_max.x, last.x), maxf(several_max.y, last.y));
        }
        /* (false if one of them is not a number, which leaves the rectangle as it is) */
        if (several_min.x >= 0 && several_max.x < map_width && several_min.y >= 0 &&
            several_max.y < map_height && last.x == last.x && last.y == last.y &&
            collision_rect_is_free_for_projectiles(col, several_min, several_max)) {
          REACH(several_min);
          REACH(several_max);
          tick += SEVERAL;
          base = last;
          continue;
        }
      }
      /* then the tiles of this one tick */
      const vec2 pos = POS_AFTER(tick);
      REACH(pos);
      /* leaving the map is something the tick has to see */
      if (!(pos.x >= 0 && pos.x < map_width && pos.y >= 0 && pos.y < map_height))
        break;
      /* Something is on these tiles. What the tick itself looks at: the
       * line between the two positions and the teleporters around it. */
      if (!collision_rect_is_free_for_projectiles(col, base, pos) &&
          (!collision_rect_is_free_of_teleporters(col, base, pos) ||
           collision_intersect_line(col, base, pos, NULL, NULL)))
        break;
      tick++;
      base = pos;
      continue;
    }
    /* everything within this many units of the base is free; the rest is for rounding */
    const float free_range = (dist - 1) * 32.0f - 2.0f;

    /* how many ticks it stays in there */
    int free_ticks = 0;
    vec2 last_free = base;
    for (;;) {
      const vec2 pos = POS_AFTER(tick + free_ticks);
      REACH(pos);
      if (!(fabsf(pos.x - base.x) <= free_range && fabsf(pos.y - base.y) <= free_range) ||
          !(pos.x >= 0 && pos.x < map_width && pos.y >= 0 && pos.y < map_height))
        break;
      free_ticks++;
      last_free = pos;
      if (tick + free_ticks >= last_tick)
        break;
    }
    if (free_ticks == 0)
      break;
    tick += free_ticks;
    base = last_free;
  }
#undef POS_AFTER
  if (tick > last_tick)
    tick = last_tick;
  /* stopped by something, or by the end of the lifetime */
  proj->next_is_event = tick < last_tick || tick >= proj->expire_tick;
#undef REACH

  /* A tee is hit if its center is this close to the way of a tick. An
   * explosion where the projectile hits the map reaches further. */
  const float margin = PHYSICAL_SIZE + 6.0f + 4.0f + (proj->explosive ? 135.0f + PHYSICAL_SIZE : 0.0f);
  proj->reach_min = v2(reach_min.x - margin, reach_min.y - margin);
  proj->reach_max = v2(reach_max.x + margin, reach_max.y + margin);
  if (memo && tick > now) {
    PROF_COUNT(17, "projectile memo misses");
    memo->ticks = (tick - now) | (proj->next_is_event ? 0 : DDNET_PROJECTILE_MEMO_NO_EVENT);
    memo->reach_min = proj->reach_min;
    memo->reach_max = proj->reach_max;
  }
  return tick;
}

/* True if a tee that the projectile can hit is where the projectile can get
 * to before its next tick, or if its owner is gone. */
static bool projectile_tees_matter(world_t *w, const ddnet_projectile_t *proj) {
  if (proj->owner >= 0 && !get_player_char(w, proj->owner))
    return true;
  for (int n = 0; n < w->num_cores; n++) {
    const int id = w->core_ids[n];
    /* a projectile does not hit its owner */
    if (id == proj->owner)
      continue;
    const vec2 pos = w->characters[id].pos;
    /* also true for positions that are not numbers */
    if (!(pos.x < proj->reach_min.x || pos.x > proj->reach_max.x || pos.y < proj->reach_min.y ||
          pos.y > proj->reach_max.y))
      return true;
  }
  return false;
}

/* All projectiles. They are only looked at on the ticks on which one of them
 * can be affected by something. */
/* How far a position is from a rectangle along x or y, whichever is more. 0 inside, or if that is not a
 * number. */
static inline float distance_to_rect(vec2 pos, vec2 min, vec2 max) {
  const float dx = maxf(min.x - pos.x, pos.x - max.x), dy = maxf(min.y - pos.y, pos.y - max.y);
  const float d = maxf(dx, dy);
  return d > 0 ? d : 0;
}

/* Lowers the slack to how far the tees that the projectile can hit are from where it can get to. */
static float projectile_slack(const world_t *w, const ddnet_projectile_t *proj, float slack,
                              bool from_check_pos) {
  for (int n = 0; n < w->num_cores; n++) {
    const int id = w->core_ids[n];
    if (id == proj->owner)
      continue;
    const character_t *chr = &w->characters[id];
    slack = minf(slack, distance_to_rect(from_check_pos ? chr->projectile_check_pos : chr->pos,
                                         proj->reach_min, proj->reach_max));
  }
  return slack;
}

/* What the map does to a projectile on the tick `age` ticks after it started
 * on its way, if no tee is near: the part of CProjectile::Tick that is left
 * then. A bounce or a teleporter changes the projectile, and the caller sets
 * its start tick. uses_choice is set if a teleporter with several exits was
 * taken: which one depends on ddnet_world_t::tele_out. */
enum { MAP_EVENT_NOTHING, MAP_EVENT_CHANGED, MAP_EVENT_OTHER };

static int projectile_map_event(world_t *w, entity_t *ent, int age, bool *uses_choice) {
  const collision_t *col = collision(w);
  ddnet_projectile_t *proj = &ent->u.projectile;
  float pt = (age - 1) / (float)SERVER_TICK_SPEED;
  float ct = age / (float)SERVER_TICK_SPEED;
  vec2 prev_pos = projectile_get_pos(w, ent, pt);
  vec2 cur_pos = projectile_get_pos(w, ent, ct);
  vec2 col_pos;
  vec2 new_pos;
  int collide = collision_intersect_line(col, prev_pos, cur_pos, &col_pos, &new_pos);
  int what = MAP_EVENT_NOTHING;

  if (collide || game_layer_clipped(w, cur_pos)) {
    /* an explosion, or freezing tees: there are none */
    if (collide && proj->bouncing != 0) {
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
      what = MAP_EVENT_CHANGED;
    } else if (proj->type == WEAPON_GUN || !proj->freeze) {
      /* it is destroyed */
      return MAP_EVENT_OTHER;
    }
  }

  /* only teleporters are of interest, and they are rare */
  int x = collision_rect_is_free_of_teleporters(col, prev_pos, cur_pos)
              ? -1
              : collision_get_index(col, prev_pos, cur_pos);
  int z;
  if (w->config.sv_old_teleport_weapons)
    z = collision_is_teleport(col, x);
  else
    z = collision_is_teleport_weapon(col, x);
  if (z && col->tele_outs[z - 1].count != 0) {
    int tele_out = world_pick_tele_out(w, proj->owner, col->tele_outs[z - 1].count);
    ent->pos = col->tele_outs[z - 1].positions[tele_out];
    if (col->tele_outs[z - 1].count > 1)
      *uses_choice = true;
    what = MAP_EVENT_CHANGED;
  }
  return what;
}

static int projectile_next_static_tick(world_t *w, entity_t *ent, int now);

/* Lets the map do to a projectile what it does up to and including a tick, with
 * no tee near. *cur is the last tick that was done. Sets next_tick. Returns
 * false if it stopped before something that is not a bounce. */
static bool projectile_advance(world_t *w, entity_t *ent, int *cur, int until) {
  ddnet_projectile_t *proj = &ent->u.projectile;
  for (;;) {
    const int event = projectile_next_static_tick(w, ent, *cur);
    proj->next_tick = event;
    if (event > until)
      return true;
    if (!proj->next_is_event) {
      /* only as far as was looked: the tick itself is still to be looked at */
      *cur = event - 1;
      continue;
    }
    bool uses_choice = false;
    const int what = projectile_map_event(w, ent, event - proj->start_tick, &uses_choice);
    if (what == MAP_EVENT_OTHER)
      return false;
    if (what == MAP_EVENT_CHANGED)
      proj->start_tick = event;
    *cur = event;
  }
}

/* Looks for the orbit of a projectile of the map: follows its way until it
 * bounces into a state it was in before. Sets orbit_period, or when to try
 * again. */
DDNET_COLD static void projectile_find_orbit(world_t *w, entity_t *ent, int now) {
  enum { MAX_EVENTS = 512, MAX_TICKS = 16384, RETRY = 32768 };
  ddnet_projectile_t *proj = &ent->u.projectile;
  entity_t scratch = *ent;
  ddnet_projectile_t *ahead = &scratch.u.projectile;
  vec2 orbit_min = v2(INFINITY, INFINITY), orbit_max = v2(-INFINITY, -INFINITY);
  /* Brent's way to find a cycle: the state after a bounce is compared with one
   * that was kept, and a newer one is kept after 1, 2, 4, ... bounces, so that
   * it ends up being one the projectile comes back to. */
  vec2 anchor_pos = v2(0, 0), anchor_direction = v2(0, 0);
  int anchor_tick = 0;
  int bounces = 0, keep_after = 1;
  bool uses_choice = false;
  int cur = now;
  /* longer and longer until the next try */
  proj->orbit_retry_tick = now + (RETRY << mini(proj->orbit_tries, 12));
  if (proj->orbit_tries < 255)
    proj->orbit_tries++;
  PROF_COUNT(19, "orbit searches");
  for (int i = 0; i < MAX_EVENTS && cur - now < MAX_TICKS; i++) {
    const int event = projectile_next_static_tick(w, &scratch, cur);
    orbit_min = v2(minf(orbit_min.x, ahead->reach_min.x), minf(orbit_min.y, ahead->reach_min.y));
    orbit_max = v2(maxf(orbit_max.x, ahead->reach_max.x), maxf(orbit_max.y, ahead->reach_max.y));
    if (!ahead->next_is_event) {
      /* only as far as was looked: the tick itself is still to be looked at */
      cur = event - 1;
      continue;
    }
    cur = event;
    const int what = projectile_map_event(w, &scratch, event - ahead->start_tick, &uses_choice);
    if (what == MAP_EVENT_OTHER) {
      PROF_COUNT(21, "orbit search: other event");
      return;
    }
    if (what != MAP_EVENT_CHANGED)
      continue;
    ahead->start_tick = event;
    if (bounces > 0 && memcmp(&scratch.pos, &anchor_pos, sizeof(anchor_pos)) == 0 &&
        memcmp(&ahead->direction, &anchor_direction, sizeof(anchor_direction)) == 0) {
      PROF_COUNT(20, "orbits found");
      proj->orbit_period = event - anchor_tick;
      proj->orbit_anchor_tick = anchor_tick;
      proj->orbit_uses_choice = uses_choice;
      proj->orbit_min = orbit_min;
      proj->orbit_max = orbit_max;
      return;
    }
    if (++bounces == keep_after) {
      keep_after *= 2;
      anchor_pos = scratch.pos;
      anchor_direction = ahead->direction;
      anchor_tick = event;
    }
  }
}

/* Brings a parked projectile to where it is after a tick. */
static void projectile_catch_up(world_t *w, entity_t *ent, int until) {
  ddnet_projectile_t *proj = &ent->u.projectile;
  if (until <= proj->parked_tick)
    return;
  /* up to where its orbit begins */
  if (proj->parked_tick < proj->orbit_anchor_tick) {
    const int first = mini(until, proj->orbit_anchor_tick);
    projectile_advance(w, ent, &proj->parked_tick, first);
    proj->parked_tick = first;
  }
  if (proj->parked_tick >= proj->orbit_anchor_tick) {
    /* whole rounds change nothing but the time */
    const int rounds = (until - proj->parked_tick) / proj->orbit_period;
    proj->start_tick += rounds * proj->orbit_period;
    proj->parked_tick += rounds * proj->orbit_period;
    projectile_advance(w, ent, &proj->parked_tick, until);
    proj->parked_tick = until;
  }
}

/* True if a projectile can be left alone as long as its owner is the only tee
 * in the world: a gun bullet, which only ends when it hits something, on a map
 * without teleporters for it (which exit one takes is somebody's choice at
 * that time). */
static bool projectile_can_be_lazy(const world_t *w, const ddnet_projectile_t *proj) {
  /* (not in the event build: where it hits something it makes an event) */
  return !EVENT_BUILD && proj->type == WEAPON_GUN && proj->owner >= 0 && !proj->explosive && !proj->freeze &&
         proj->bouncing == 0 && collision(w)->num_projectile_teleporters == 0;
}

/* ... and if it is so: nobody else is there, and its owner could not be taken anywhere by it. */
static bool projectile_lazy_now(world_t *w, const ddnet_projectile_t *proj) {
  if (!(w->num_cores == 1 && w->core_ids[0] == proj->owner))
    return false;
  const character_t *owner = get_player_char(w, proj->owner);
  return owner && owner->alive && !owner->core.has_telegun_gun;
}

/* What happened to a lazy projectile up to and including a tick: false if it is gone. */
static bool projectile_lazy_catch_up(world_t *w, entity_t *ent, int until) {
  ddnet_projectile_t *proj = &ent->u.projectile;
  int cur = proj->lazy_tick;
  /* its lifetime ends on expire_tick, if nothing else ended it */
  if (!projectile_advance(w, ent, &cur, mini(until, proj->expire_tick - 1)))
    return false;
  if (proj->expire_tick <= until)
    return false;
  proj->lazy_tick = until;
  return true;
}

/* Takes a projectile out of the world right away. */
static void projectile_remove_now(world_t *w, int index) {
  world_remove_entity(w, DDNET_ENTTYPE_PROJECTILE, index);
  world_free_entity(w, index);
}

/* Brings parked projectiles to where they are after a tick: all of them, which
 * stay parked, or (forget 1) the ones whose orbit depends on the choice of
 * teleporter exits or (forget 2) all, which are unparked with their orbits
 * forgotten. */
DDNET_COLD static void projectiles_catch_up(world_t *w, int until, int forget) {
  /* through teleporters like when they were parked */
  const int tele_out = w->tele_out, old_teleport_weapons = w->config.sv_old_teleport_weapons;
  w->tele_out = w->orbit_tele_out;
  w->config.sv_old_teleport_weapons = w->orbit_old_teleport_weapons;
  for (int index = w->first_entity[DDNET_ENTTYPE_PROJECTILE], next; index != -1; index = next) {
    entity_t *ent = entity(w, index);
    ddnet_projectile_t *proj = &ent->u.projectile;
    next = ent->link.next;
    if (proj->lazy && forget != 1) {
      /* Whether it is still there. It does not change on its way, so there
       * is nothing else to bring up to date. */
      entity_t scratch = *ent;
      if (!projectile_lazy_catch_up(w, &scratch, until)) {
        projectile_remove_now(w, index);
      } else if (forget) {
        /* it is looked at on the next tick */
        proj->lazy = false;
        w->num_lazy_projectiles--;
        projectile_set_insert(w, index);
        proj->next_tick = 0;
      }
      continue;
    }
    if (forget == 1 && !(proj->orbit_period > 0 && proj->orbit_uses_choice))
      continue;
    if (proj->parked) {
      projectile_catch_up(w, ent, until);
      /* with the rectangle of its orbit, which it is still in */
      proj->reach_min = proj->orbit_min;
      proj->reach_max = proj->orbit_max;
    }
    if (!forget)
      continue;
    if (proj->parked) {
      /* everything is looked at on the next tick: it finds its way from there */
      projectile_set_erase(w, index);
      proj->parked = false;
      projectile_set_insert(w, index);
      proj->next_tick = 0;
    }
    proj->orbit_period = 0;
    if (forget == 2) {
      proj->orbit_retry_tick = 0;
      proj->orbit_tries = 0;
    }
  }
  w->tele_out = tele_out;
  w->config.sv_old_teleport_weapons = old_teleport_weapons;
  if (forget)
    w->projectile_wakeup = 0;
}

/* ddnet_world_sync() for the projectiles. */
void projectiles_sync(world_t *w) { projectiles_catch_up(w, server_tick(w), 0); }

/* Something the orbits depend on changes after a tick. */
void projectiles_forget_orbits(world_t *w, int until, bool all) {
  projectiles_catch_up(w, until, all ? 2 : 1);
}

/* Takes a projectile out of the list of the tick it was to be looked at on. */
void projectile_unschedule(world_t *w, int index) {
  ddnet_projectile_t *proj = &entity(w, index)->u.projectile;
  if (proj->due_tick < 0)
    return;
  if (proj->due_prev != -1)
    entity(w, proj->due_prev)->u.projectile.due_next = proj->due_next;
  else
    w->projectile_due[proj->due_tick % DDNET_PROJECTILE_DUE_SIZE] = proj->due_next;
  if (proj->due_next != -1)
    entity(w, proj->due_next)->u.projectile.due_prev = proj->due_prev;
  proj->due_tick = -1;
}

/* Files a projectile under its next_tick. */
static void projectile_schedule(world_t *w, int index, int now) {
  ddnet_projectile_t *proj = &entity(w, index)->u.projectile;
  /* not further ahead than there are lists for */
  if (proj->next_tick - now >= DDNET_PROJECTILE_DUE_SIZE) {
    proj->next_tick = now + DDNET_PROJECTILE_DUE_SIZE - 1;
    proj->next_is_event = false;
  }
  if (proj->due_tick == proj->next_tick)
    return;
  projectile_unschedule(w, index);
  int *first = &w->projectile_due[proj->next_tick % DDNET_PROJECTILE_DUE_SIZE];
  proj->due_tick = proj->next_tick;
  proj->due_prev = -1;
  proj->due_next = *first;
  if (*first != -1)
    entity(w, *first)->u.projectile.due_prev = index;
  *first = index;
}

/* One projectile on a tick on which the map could do something to it, or on
 * which all of them are compared with the tees (all). Returns false if all
 * of them have to be looked at again on the next tick. */
static bool projectile_pass(world_t *w, int index, int now, bool all, float *slack) {
  entity_t *ent = entity(w, index);
  ddnet_projectile_t *proj = &ent->u.projectile;
  /* only a projectile that is up to date with the tick before can become lazy: nothing was due, or it is new
   */
  if (proj->lazy || proj->next_tick >= now || (proj->next_tick == 0 && proj->start_tick == now - 1)) {
    if (projectile_can_be_lazy(w, proj) && projectile_lazy_now(w, proj)) {
      if (!proj->lazy) {
        projectile_set_erase(w, index);
        proj->lazy = true;
        proj->lazy_tick = now - 1;
        w->num_lazy_projectiles++;
      }
      /* by the end of its lifetime it is gone, whatever happened to it */
      if (now >= proj->expire_tick) {
        entity_mark_for_destroy(w, ent);
        projectile_unschedule(w, index);
        return true;
      }
      /* it only has to be looked at again to see that */
      if (now >= proj->next_tick || proj->due_tick < 0) {
        proj->next_tick = mini(proj->expire_tick, now + DDNET_PROJECTILE_DUE_SIZE - 1);
        proj->next_is_event = false;
        projectile_schedule(w, index, now);
      }
      return true;
    }
    if (proj->lazy) {
      /* not anymore: what happened to it in the meantime */
      proj->lazy = false;
      w->num_lazy_projectiles--;
      projectile_set_insert(w, index);
      if (!projectile_lazy_catch_up(w, ent, now - 1)) {
        entity_mark_for_destroy(w, ent);
        projectile_unschedule(w, index);
        return true;
      }
    }
  }
  if (proj->parked) {
    /* its rectangle is the one of its whole orbit: nothing to do while no tee is in there */
    if (!projectile_tees_matter(w, proj)) {
      *slack = projectile_slack(w, proj, *slack, !all);
      return true;
    }
    PROF_COUNT(22, "unparked");
    projectile_catch_up(w, ent, now - 1);
    projectile_set_erase(w, index);
    proj->parked = false;
    projectile_set_insert(w, index);
  }
  const bool map_matters = now >= proj->next_tick;
  if (!map_matters && !all)
    return true;

  /* tees that the projectile could hit some time, or an owner that is gone */
  const bool watch_tees = w->num_cores >= 2 || (w->num_cores == 1 && w->core_ids[0] != proj->owner) ||
                          (proj->owner >= 0 && !get_player_char(w, proj->owner));
  bool tees_matter = watch_tees && projectile_tees_matter(w, proj);
  bool done = false;

  /* No tee around (and its owner still there): what the map does to it may
   * not need its tick. */
  if (map_matters && !tees_matter && proj->next_tick == now && now < proj->expire_tick) {
    if (!proj->next_is_event) {
      /* Nothing was known to happen now, it only was not looked at yet,
       * and neither was whether a tee is where this tick gets to. */
      const int next_tick = projectile_next_static_tick(w, ent, now - 1);
      tees_matter = watch_tees && projectile_tees_matter(w, proj);
      if (next_tick > now && !tees_matter) {
        proj->next_tick = next_tick;
        done = true;
      }
    }
    if (!done && !tees_matter && proj->owner < 0 && !(EVENT_BUILD && proj->explosive)) {
      /* a projectile of the map: how it bounced the last time it was here like this (not in the event build
       * if it explodes where it bounces, which is an event) */
      bool known;
      const ddnet_projectile_memo_entry_t *memo = projectile_memo(w, ent, now - proj->start_tick, 1, &known);
      if (known) {
        PROF_COUNT(18, "projectile memo bounces");
        ent->pos = memo->reach_min;
        proj->direction = memo->reach_max;
        proj->start_tick = now;
        proj->next_tick = projectile_next_static_tick(w, ent, now);
        done = true;
      }
    }
  }

  if (!done && (map_matters || tees_matter)) {
    /* the way it is on changes when it bounces or goes through a teleporter */
    const ddnet_entity_t before = *ent;
    projectile_tick(w, index);
    if (!ent->marked_for_destroy) {
      if (w->projectile_only_bounced && !tees_matter && proj->owner < 0 &&
          before.u.projectile.next_tick == now && now < proj->expire_tick) {
        /* remember it, under what the projectile was before */
        const vec2 pos = ent->pos, direction = proj->direction;
        bool known;
        ddnet_projectile_memo_entry_t *memo =
            projectile_memo(w, &before, now - before.u.projectile.start_tick, 1, &known);
        if (memo) {
          memo->ticks = 1;
          memo->reach_min = pos;
          memo->reach_max = direction;
        }
      }
      if (map_matters || proj->start_tick != before.u.projectile.start_tick || vne(ent->pos, before.pos) ||
          vne(proj->direction, before.u.projectile.direction))
        proj->next_tick = projectile_next_static_tick(w, ent, now);
    }
  }
  if (ent->marked_for_destroy) {
    projectile_unschedule(w, index);
    return true;
  }
  /* A projectile of the map that lives forever: once it is known how it goes
   * round, it is left alone until a tee comes to where it goes. */
  /* (not in the event build, which is shown and makes events: where a parked one is is only worked out when
   * it matters, and an explosive one explodes on its way, which nobody feels but which is an event) */
  if (proj->owner < 0 && proj->expire_tick == INT_MAX && !EVENT_BUILD) {
    if (proj->orbit_period == 0 && now >= proj->orbit_retry_tick)
      projectile_find_orbit(w, ent, now);
    if (proj->orbit_period > 0) {
      const vec2 reach_min = proj->reach_min, reach_max = proj->reach_max;
      proj->reach_min = proj->orbit_min;
      proj->reach_max = proj->orbit_max;
      if (!projectile_tees_matter(w, proj)) {
        projectile_unschedule(w, index);
        projectile_set_erase(w, index);
        proj->parked = true;
        proj->parked_tick = now;
        projectile_set_insert(w, index);
        if (w->projectile_sets_failed) {
          *slack = projectile_slack(w, proj, *slack, !all);
        } else {
          /* the parked ones have a slack of their own, from where the tees were when they were looked at */
          for (int n = 0; n < w->num_cores; n++)
            w->projectile_parked_slack =
                minf(w->projectile_parked_slack,
                     distance_to_rect(w->characters[w->core_ids[n]].projectile_parked_check_pos,
                                      proj->orbit_min, proj->orbit_max));
        }
        return true;
      }
      proj->reach_min = reach_min;
      proj->reach_max = reach_max;
    }
  }
  projectile_schedule(w, index, now);
  if (watch_tees) {
    /* the tees are compared with where it can get to now */
    *slack = projectile_slack(w, proj, *slack, !all);
    /* one without its owner goes on its next tick */
    if (proj->owner >= 0 && !get_player_char(w, proj->owner))
      return false;
  }
  return true;
}

/* All projectiles. They are only looked at on the ticks on which something
 * can happen to them. */
static void projectiles_tick_some(world_t *w, int now);

DDNET_HOT void projectiles_tick(world_t *w) {
  if (w->first_entity[DDNET_ENTTYPE_PROJECTILE] == -1)
    return;
  const int now = server_tick(w);
#ifdef DDNET_PHYSICS_EVENTS
  /* A world from the other build can have lazy or parked bullets, which this build does not make: they are
   * looked at again (what they did on the ticks of the other build made no events). */
  if (w->num_lazy_projectiles > 0)
    w->lazy_wake = true;
  for (int index = w->first_entity[DDNET_ENTTYPE_PROJECTILE]; index != -1;
       index = entity_peek(w, index)->link.next)
    if (entity_peek(w, index)->u.projectile.parked) {
      projectiles_forget_orbits(w, now - 1, true);
      break;
    }
  /* The trail a client draws behind a grenade, from where it was a tick ago, for every grenade on every
   * tick (which the projectiles below are not all looked at on). (Before the others of the tick, which DDNet
   * has in between.) */
  if (w->particle)
    for (int index = w->first_entity[DDNET_ENTTYPE_PROJECTILE]; index != -1;
         index = entity_peek(w, index)->link.next) {
      const entity_t *ent = entity_peek(w, index);
      if (ent->u.projectile.type == WEAPON_GRENADE)
        emit_particle(
            w,
            projectile_get_pos(w, ent, (now - ent->u.projectile.start_tick - 1) / (float)SERVER_TICK_SPEED),
            DDNET_PARTICLE_SMOKE, ent->u.projectile.owner);
    }
#endif
  /* Only lazy ones, with nothing that would make them matter and none to be looked at now. */
  if (!EVENT_BUILD && (w->num_projectile_awake | w->num_projectile_parked | w->projectile_sets_failed) == 0 &&
      w->projectile_wakeup > 0 && w->num_cores == 1 && !w->characters[w->core_ids[0]].core.has_telegun_gun) {
    const int due = w->projectile_due[now % DDNET_PROJECTILE_DUE_SIZE];
    if (due == -1)
      return;
    /* One of them is due, which is nearly always for the end of its
     * lifetime: what projectile_pass() does with a lazy one, without the
     * rest of projectiles_tick_some(), which has nothing to do here. */
    entity_t *ent = entity(w, due);
    ddnet_projectile_t *proj = &ent->u.projectile;
    if (proj->due_next == -1 && proj->lazy && now >= proj->expire_tick && projectile_can_be_lazy(w, proj) &&
        projectile_lazy_now(w, proj)) {
      entity_mark_for_destroy(w, ent);
      projectile_unschedule(w, due);
      return;
    }
  }
  projectiles_tick_some(w, now);
}

/* The part of projectiles_tick() for ticks on which there may be something to do. */
DDNET_NOINLINE static void projectiles_tick_some(world_t *w, int now) {
  /* more than this are not worth putting in order: the whole list is gone through instead */
  enum { MAX_DUE = 12 };
  /* a teleporter exit chosen differently than the orbits were made for */
  if (w->tele_out != w->orbit_tele_out ||
      w->config.sv_old_teleport_weapons != w->orbit_old_teleport_weapons) {
    projectiles_forget_orbits(w, now - 1, w->config.sv_old_teleport_weapons != w->orbit_old_teleport_weapons);
    w->orbit_tele_out = w->tele_out;
    w->orbit_old_teleport_weapons = w->config.sv_old_teleport_weapons;
  }
  /* All projectiles are compared with all tees if something changed which
   * of them matter to each other (the wakeup is 0 then), or if a tee moved so
   * far that it could be where a projectile gets to. */
  bool all = w->projectile_wakeup <= 0;
  /* lazy projectiles, and their owner is not the only tee anymore (another one came, or it left, and
   * another one may be alone) or got something that lets them matter */
  bool wake_lazy = false;
  if (w->num_lazy_projectiles > 0) {
    const character_t *only = w->num_cores == 1 ? &w->characters[w->core_ids[0]] : NULL;
    wake_lazy = !only || w->lazy_wake || only->core.has_telegun_gun;
    all |= wake_lazy;
  }
  w->lazy_wake = false;
  for (int n = 0; n < w->num_cores && !all; n++) {
    const character_t *chr = &w->characters[w->core_ids[n]];
    const float moved = maxf(fabsf(chr->pos.x - chr->projectile_check_pos.x),
                             fabsf(chr->pos.y - chr->projectile_check_pos.y));
    all = !(moved < w->projectile_slack);
  }
  /* The parked ones: if a tee could have got into the rectangle of one, they
   * are compared with the tees, and the ones a tee is at are parked no more. */
  if (!w->projectile_sets_failed) {
    /* (the slack is below 0 after a tee entered the world) */
    bool look = false;
    for (int n = 0; n < w->num_cores && !look; n++) {
      const character_t *chr = &w->characters[w->core_ids[n]];
      const float moved = maxf(fabsf(chr->pos.x - chr->projectile_parked_check_pos.x),
                               fabsf(chr->pos.y - chr->projectile_parked_check_pos.y));
      look = !(moved < w->projectile_parked_slack);
    }
    if (look) {
      float parked_slack = INFINITY;
      for (int i = 0; i < w->num_projectile_parked;) {
        const ddnet_parked_projectile_t parked = w->projectile_parked[i];
        bool inside = false;
        for (int n = 0; n < w->num_cores; n++) {
          const vec2 pos = w->characters[w->core_ids[n]].pos;
          /* also for positions that are not numbers */
          if (!(pos.x < parked.min.x || pos.x > parked.max.x || pos.y < parked.min.y || pos.y > parked.max.y))
            inside = true;
          else
            parked_slack = minf(parked_slack, distance_to_rect(pos, parked.min, parked.max));
        }
        if (!inside) {
          i++;
          continue;
        }
        PROF_COUNT(22, "unparked");
        entity_t *ent = entity(w, parked.index);
        projectile_catch_up(w, ent, now - 1);
        projectile_set_erase(w, parked.index);
        ent->u.projectile.parked = false;
        projectile_set_insert(w, parked.index);
        /* it is looked at with the others */
        all = true;
      }
      w->projectile_parked_slack = parked_slack;
      for (int n = 0; n < w->num_cores; n++)
        w->characters[w->core_ids[n]].projectile_parked_check_pos = w->characters[w->core_ids[n]].pos;
    }
  }

  /* otherwise only the ones the map does something to */
  const int first_due = w->projectile_due[now % DDNET_PROJECTILE_DUE_SIZE];
  if (!all && first_due == -1)
    return;

  PROF_COUNT(14, "projectile passes");
  /* in the order of the entity list, which is the newest first */
  int due[MAX_DUE];
  int num_due = 0;
  if (!all) {
    for (int index = first_due; index != -1; index = entity(w, index)->u.projectile.due_next) {
      if (num_due == MAX_DUE) {
        num_due = -1;
        break;
      }
      const unsigned seq = entity(w, index)->u.projectile.seq;
      int k = num_due++;
      for (; k > 0 && entity(w, due[k - 1])->u.projectile.seq < seq; k--)
        due[k] = due[k - 1];
      due[k] = index;
    }
  }

  float slack = all ? INFINITY : w->projectile_slack;
  bool settled = true;
  enum { MAX_AWAKE = 128 };
  /* (the lazy ones are only in the entity list) */
  if (all && !wake_lazy && !w->projectile_sets_failed && w->num_projectile_awake <= MAX_AWAKE) {
    /* the ones that are not parked, in the order of the entity list */
    int awake[MAX_AWAKE];
    const int num_awake = w->num_projectile_awake;
    for (int i = 0; i < num_awake; i++) {
      const int index = w->projectile_awake[i];
      const unsigned seq = entity(w, index)->u.projectile.seq;
      int k = i;
      for (; k > 0 && entity(w, awake[k - 1])->u.projectile.seq < seq; k--)
        awake[k] = awake[k - 1];
      awake[k] = index;
    }
    for (int i = 0; i < num_awake; i++)
      settled &= projectile_pass(w, awake[i], now, true, &slack);
  } else if (all || num_due < 0) {
    for (int index = w->first_entity[DDNET_ENTTYPE_PROJECTILE]; index != -1;) {
      const int next = entity_peek(w, index)->link.next;
      /* the parked ones were looked at above */
      if (w->projectile_sets_failed || !entity(w, index)->u.projectile.parked)
        settled &= projectile_pass(w, index, now, all, &slack);
      index = next;
    }
  } else {
    for (int i = 0; i < num_due; i++)
      settled &= projectile_pass(w, due[i], now, false, &slack);
  }
  w->projectile_wakeup = settled ? 1 : 0;
  w->projectile_slack = slack;
  if (all)
    for (int n = 0; n < w->num_cores; n++)
      w->characters[w->core_ids[n]].projectile_check_pos = w->characters[w->core_ids[n]].pos;
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
      (laser->owner >= 0 && laser->owner < w->num_clients && w->players[laser->owner].active)
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
  character_take_damage(w, hit, v2(0, 0), 0, laser->owner, laser->type);
  return true;
}

/* CLaser::DoBounce */
static void laser_do_bounce(world_t *w, entity_t *ent) {
  const collision_t *col = collision(w);
  ddnet_laser_t *laser = &ent->u.laser;

  laser->eval_tick = server_tick(w);

  if (laser->energy < 0) {
    entity_mark_for_destroy(w, ent);
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

      EMIT_SOUND(w, ent->pos, DDNET_SOUND_LASER_BOUNCE, laser->owner);
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
DDNET_COLD void laser_create(world_t *w, vec2 pos, vec2 direction, float start_energy, int owner, int type) {
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

  const bool owner_connected = owner >= 0 && owner < w->num_clients && w->players[owner].active;
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
      entity_mark_for_destroy(w, ent);
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
/* What a pickup does to a tee that touches it (the switch in CPickup::Tick). */
static void pickup_apply(world_t *w, int type, int subtype, vec2 pos, character_t *chr) {
  (void)pos; /* (for the sounds of the event build) */
  bool sound = false;
  /* player picked us up, is someone was hooking us, let them go */
  switch (type) {
  case DDNET_POWERUP_FREEZE:
#ifdef DDNET_PHYSICS_EVENTS
    if (character_freeze(w, chr))
      emit_sound(w, pos, DDNET_SOUND_PICKUP_HEALTH, character_id(chr));
#else
    character_freeze(w, chr);
#endif
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
      EMIT_SOUND(w, pos, DDNET_SOUND_PICKUP_ARMOR, character_id(chr));
    }
    if (chr->core.active_weapon >= WEAPON_SHOTGUN)
      chr->core.active_weapon = WEAPON_HAMMER;
    break;

  case DDNET_POWERUP_ARMOR_SHOTGUN:
    if (chr->core.weapons[WEAPON_SHOTGUN].got) {
      chr->core.weapons[WEAPON_SHOTGUN].got = false;
      chr->core.weapons[WEAPON_SHOTGUN].ammo = 0;
      chr->last_weapon = WEAPON_GUN;
      EMIT_SOUND(w, pos, DDNET_SOUND_PICKUP_ARMOR, character_id(chr));
    }
    if (chr->core.active_weapon == WEAPON_SHOTGUN)
      chr->core.active_weapon = WEAPON_HAMMER;
    break;

  case DDNET_POWERUP_ARMOR_GRENADE:
    if (chr->core.weapons[WEAPON_GRENADE].got) {
      chr->core.weapons[WEAPON_GRENADE].got = false;
      chr->core.weapons[WEAPON_GRENADE].ammo = 0;
      chr->last_weapon = WEAPON_GUN;
      EMIT_SOUND(w, pos, DDNET_SOUND_PICKUP_ARMOR, character_id(chr));
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
      EMIT_SOUND(w, pos, DDNET_SOUND_PICKUP_ARMOR, character_id(chr));
    }
    if (chr->core.active_weapon == WEAPON_LASER)
      chr->core.active_weapon = WEAPON_HAMMER;
    break;

  case DDNET_POWERUP_WEAPON:
    if (subtype >= 0 && subtype < NUM_WEAPONS &&
        (!chr->core.weapons[subtype].got || chr->core.weapons[subtype].ammo != -1)) {
      character_give_weapon(w, chr, subtype, false);

      if (subtype == WEAPON_GRENADE)
        EMIT_SOUND(w, pos, DDNET_SOUND_PICKUP_GRENADE, character_id(chr));
      else if (subtype == WEAPON_SHOTGUN || subtype == WEAPON_LASER)
        EMIT_SOUND(w, pos, DDNET_SOUND_PICKUP_SHOTGUN, character_id(chr));
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

enum { PICKUP_COLLISION_EXTRA_SIZE = 6 };

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
  int ents[MAX_CLIENTS];
  int num = world_find_characters(w, ent->pos, PICKUP_PROXIMITY_RADIUS + PICKUP_COLLISION_EXTRA_SIZE, ents,
                                  MAX_CLIENTS);
  for (int i = 0; i < num; ++i) {
    character_t *chr = &w->characters[ents[i]];

    if (chr->alive) {
      if (switch_inactive_for(w, ent, character_team(w, chr)))
        continue;
      pickup_apply(w, pickup->type, pickup->subtype, ent->pos, chr);
    }
  }
}

/* All pickups at once, for maps where none of them is on a conveyor and they
 * therefore never move. Instead of every pickup looking for tees, every tee
 * looks at the pickups around it, in the order in which the pickups would have
 * ticked. A pickup only changes the tee that touches it, so it does not matter
 * that the tees are no longer handled pickup by pickup. */
/* A pickup on a conveyor for one tee: what is left of CPickup::Tick. */
static void pickup_mover_apply(world_t *w, int index, character_t *chr) {
  entity_t *ent = entity(w, index);
  const float radius = PICKUP_PROXIMITY_RADIUS + PICKUP_COLLISION_EXTRA_SIZE;
  if (!(vdistance(chr->pos, ent->pos) < radius + PHYSICAL_SIZE))
    return;
  if (!chr->alive || switch_inactive_for(w, ent, character_team(w, chr)))
    return;
  pickup_apply(w, ent->u.pickup.type, ent->u.pickup.subtype, ent->pos, chr);
}

/* pickups_tick_static() for a tee that has pickups of the map around it. */
DDNET_NOINLINE static void pickups_tick_near(world_t *w, character_t *chr) {
  const collision_t *col = collision(w);
  const float radius = PICKUP_PROXIMITY_RADIUS + PICKUP_COLLISION_EXTRA_SIZE;
  int mover = 0;
#define MOVERS_BEFORE(id)                                                                                    \
  while (mover < w->num_pickup_movers && w->pickup_mover_ids[mover] > (id))                                  \
  pickup_mover_apply(w, w->pickup_movers[mover++], chr)
  const int tx = clampi((int)chr->pos.x >> 5, 0, col->width - 1);
  const int ty = clampi((int)chr->pos.y >> 5, 0, col->height - 1);
  /* pickups tick newest first, and they were created row by row */
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
        const ddnet_map_pickup_t *pickup = &col->pickups[k];
        if (pickup->moves)
          continue;
        MOVERS_BEFORE(k);
        /* CGameWorld::FindEntities */
        if (!(vdistance(chr->pos, pickup->pos) < radius + PHYSICAL_SIZE))
          continue;
        if (pickup->layer == LAYER_SWITCH && pickup->number > 0 &&
            !switch_status(w, pickup->number, character_team(w, chr)))
          continue;
        pickup_apply(w, pickup->type, pickup->subtype, pickup->pos, chr);
      }
    }
  }
  MOVERS_BEFORE(-1);
#undef MOVERS_BEFORE
}

/* All pickups: the ones that stay where the map has them are found from the
 * tees, through the tiles around them. The ones on conveyors are moved and
 * looked at one by one, in between the others where they are in the order of
 * the list. */
DDNET_NOINLINE void pickups_tick_static(world_t *w) {
  const collision_t *col = collision(w);
  if (w->first_entity[DDNET_ENTTYPE_PICKUP] == -1)
    return;

  /* CPickup::Move */
  if (w->num_pickup_movers && is_mover_tick(w)) {
    for (int m = 0; m < w->num_pickup_movers; m++) {
      entity_t *ent = entity(w, w->pickup_movers[m]);
      collision_mover_speed(col, ent->pos.x, ent->pos.y, &ent->u.pickup.core);
      ent->pos = vadd(ent->pos, ent->u.pickup.core);
    }
  }

  for (int n = 0; n < w->num_cores; n++) {
    character_t *chr = &w->characters[w->core_ids[n]];
    /* the next of the pickups on conveyors, which come before the pickups of the map they were made after */
    int mover = 0;
#define MOVERS_BEFORE(id)                                                                                    \
  while (mover < w->num_pickup_movers && w->pickup_mover_ids[mover] > (id))                                  \
  pickup_mover_apply(w, w->pickup_movers[mover++], chr)

    /* no pickup of the map in reach */
    if (character_is_quiet(col, chr)) {
      MOVERS_BEFORE(-1);
      continue;
    }
    if (!(chr->here_flags & DDNET_TILEFLAG_NEAR_PICKUP)) {
      MOVERS_BEFORE(-1);
      continue;
    }
    /* pickups of the map in reach, with the ones on conveyors in between */
    pickups_tick_near(w, chr);
#undef MOVERS_BEFORE
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
  dragger->idle = true;

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
  entity_mark_for_destroy(w, ent);
  beam->active = false;

  /* CDragger::RemoveDraggerBeam */
  ddnet_dragger_targets_t *targets = &w->dragger_targets[entity(w, beam->dragger)->u.dragger.targets];
  world_touch(w, TOUCH_DRAGGER_TARGETS, entity(w, beam->dragger)->u.dragger.targets);
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
      collision_sight_blocked(collision(w), beam->ignore_walls ? 2 : 1, ent->pos, target->pos, &beam->sight,
                              sight_memo(w), index)) {
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
  /* (noted for ddnet_world_copy() further down, once something of it may change) */
  entity_t *ent = (entity_t *)entity_peek(w, index);
  ddnet_dragger_t *dragger = &ent->u.dragger;
  /* this table never moves during a tick */
  ddnet_dragger_targets_t *targets = &w->dragger_targets[dragger->targets];

  /* Create a list of players who are in the range of the dragger */
  int players_in_range[MAX_CLIENTS];
  int num_players_in_range = world_find_characters(w, ent->pos, w->config.sv_dragger_range - PHYSICAL_SIZE,
                                                   players_in_range, MAX_CLIENTS);
  /* nobody in range, and no target or beam left to remove */
  if (num_players_in_range == 0 && dragger->idle)
    return;

  /* One tee in range of a dragger without target or beam, and it cannot be
   * reached: all of what follows leaves everything as it is. */
  int reachable = -1;
  if (num_players_in_range == 1 && dragger->idle) {
    const character_t *target = &w->characters[players_in_range[0]];
    if (!target->alive || switch_inactive_for(w, ent, character_team(w, target)))
      return;
    const ddnet_sight_t sight = dragger->sight;
    reachable = !collision_sight_blocked(collision(w), dragger->ignore_walls ? 2 : 1, ent->pos, target->pos,
                                         &dragger->sight, sight_memo(w), index);
    if (memcmp(&sight, &dragger->sight, sizeof(sight)) != 0)
      world_touch(w, TOUCH_ENTITY, index);
    if (!reachable)
      return;
  }

  /* (from here on the dragger and the table of targets and beams may be written to) */
  world_touch(w, TOUCH_ENTITY, index);
  world_touch(w, TOUCH_DRAGGER_TARGETS, dragger->targets);

  /* The closest player (within range) in a team is selected as the target */
  int closest_target_id_in_team[MAX_CLIENTS];
  bool can_still_be_team_target[MAX_CLIENTS];
  bool is_target[MAX_CLIENTS];
  int min_dist_in_team[MAX_CLIENTS];
  /* DDNet goes through all clients and teams. Without a target or beam from
   * before, only the players in range and their teams can get one, so only
   * their entries are used. */
  const bool sparse = dragger->idle;
  if (sparse) {
    for (int i = 0; i < num_players_in_range; i++) {
      const character_t *target = &w->characters[players_in_range[i]];
      min_dist_in_team[character_team(w, target)] = 0;
      closest_target_id_in_team[character_team(w, target)] = -1;
      can_still_be_team_target[character_id(target)] = false;
      is_target[character_id(target)] = false;
    }
  } else {
    for (int i = 0; i < MAX_CLIENTS; i++) {
      can_still_be_team_target[i] = false;
      min_dist_in_team[i] = 0;
      is_target[i] = false;
      closest_target_id_in_team[i] = -1;
    }
  }

  for (int i = 0; i < num_players_in_range; i++) {
    character_t *target = &w->characters[players_in_range[i]];
    const int target_team = character_team(w, target);
    /* If the dragger is disabled for the target's team, no dragger beam will be generated */
    if (switch_inactive_for(w, ent, target_team))
      continue;

    /* Dragger beams can be created only for reachable, alive players */
    int is_reachable = reachable >= 0
                           ? reachable
                           : !collision_sight_blocked(collision(w), dragger->ignore_walls ? 2 : 1, ent->pos,
                                                      target->pos, &dragger->sight, sight_memo(w), index);
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
  for (int n = 0; n < (sparse ? num_players_in_range : MAX_CLIENTS); n++) {
    const int i = sparse ? character_team(w, &w->characters[players_in_range[n]]) : n;
    if ((targets->target_id_in_team[i] != -1 && !can_still_be_team_target[targets->target_id_in_team[i]]) ||
        targets->target_id_in_team[i] == -1) {
      targets->target_id_in_team[i] = closest_target_id_in_team[i];
    }
    if (targets->target_id_in_team[i] != -1)
      is_target[targets->target_id_in_team[i]] = true;
  }

  /* in the order of the client ids, which is the order the beams are created in */
  if (sparse)
    sort_ids(players_in_range, num_players_in_range);
  for (int n = 0; n < (sparse ? num_players_in_range : MAX_CLIENTS); n++) {
    const int i = sparse ? players_in_range[n] : n;
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

  bool idle = true;
  if (sparse) {
    for (int n = 0; n < num_players_in_range; n++) {
      const int id = players_in_range[n];
      idle &=
          targets->target_id_in_team[character_team(w, &w->characters[id])] == -1 && targets->beam[id] == -1;
    }
  } else {
    for (int i = 0; i < MAX_CLIENTS; i++)
      idle &= targets->target_id_in_team[i] == -1 && targets->beam[i] == -1;
  }
  /* was busy before, or it would not have got here with nobody in range */
  ddnet_dragger_t *self = &entity(w, index)->u.dragger;
  if (self->idle && !idle) {
    if (w->busy_draggers < DDNET_MAX_BUSY_DRAGGERS && !w->busy_draggers_overflow)
      w->busy_dragger_ids[w->busy_draggers] = index;
    else
      w->busy_draggers_overflow = true;
    w->busy_draggers++;
  } else if (!self->idle && idle) {
    w->busy_draggers--;
    if (!w->busy_draggers_overflow) {
      for (int i = 0; i <= w->busy_draggers; i++) {
        if (w->busy_dragger_ids[i] == index) {
          w->busy_dragger_ids[i] = w->busy_dragger_ids[w->busy_draggers];
          break;
        }
      }
    }
    if (w->busy_draggers == 0)
      w->busy_draggers_overflow = false;
  }
  self->idle = idle;
}

/* CDragger::Tick */
static void dragger_tick(world_t *w, int index) {
  /* (Most draggers stay where they are and have nobody to drag: only looked
   * at then, so that ddnet_world_copy() has nothing to copy. m_EvalTick is
   * not kept: nothing reads it.) */
  const entity_t *still = entity_peek(w, index);

  if (is_mover_tick(w)) {
    vec2 core = still->u.dragger.core;
    collision_mover_speed(collision(w), still->pos.x, still->pos.y, &core);
    const vec2 pos = vadd(still->pos, core);
    if (memcmp(&core, &still->u.dragger.core, sizeof(core)) != 0 ||
        memcmp(&pos, &still->pos, sizeof(pos)) != 0) {
      entity_t *moved = entity(w, index);
      moved->u.dragger.core = core;
      moved->pos = pos;
    }
    const entity_t *ent = still;
    const ddnet_dragger_t *dragger = &ent->u.dragger;

    /* Adopt the new position for all outgoing laser beams */
    if (!dragger->idle) {
      const ddnet_dragger_targets_t *targets = &w->dragger_targets[dragger->targets];
      for (int i = 0; i < MAX_CLIENTS; i++) {
        if (targets->beam[i] != -1)
          entity(w, targets->beam[i])->pos = ent->pos;
      }
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
  /* (noted for ddnet_world_copy() where something of it changes) */
  entity_t *ent = (entity_t *)entity_peek(w, index);
  ddnet_gun_t *gun = &ent->u.gun;
  /* this table never moves during a tick */
  ddnet_gun_timers_t *timers = &w->gun_timers[gun->timers];

  /* Create a list of players who are in the range of the turret */
  int players_in_range[MAX_CLIENTS];
  int num_players_in_range =
      world_find_characters(w, ent->pos, w->config.sv_plasma_range, players_in_range, MAX_CLIENTS);
  /* nobody to shoot at */
  if (num_players_in_range == 0)
    return;

  /* The closest player (within range) in a team is selected as the target */
  int target_id_in_team[MAX_CLIENTS];
  bool is_target[MAX_CLIENTS];
  int min_dist_in_team[MAX_CLIENTS];
  /* DDNet goes through all clients and teams. Only the players in range and
   * their teams can be anything else than not a target, so only their entries
   * are used here. */
  for (int i = 0; i < num_players_in_range; i++) {
    const character_t *target = &w->characters[players_in_range[i]];
    min_dist_in_team[character_team(w, target)] = 0;
    target_id_in_team[character_team(w, target)] = -1;
    is_target[character_id(target)] = false;
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
    const ddnet_sight_t sight = gun->sight;
    int is_reachable =
        !collision_sight_blocked(collision(w), 0, ent->pos, target->pos, &gun->sight, sight_memo(w), index);
    if (memcmp(&sight, &gun->sight, sizeof(sight)) != 0)
      world_touch(w, TOUCH_ENTITY, index);
    if (is_reachable && target->alive) {
      /* Turrets fire on solo players regardless of the others */
      if (target_is_solo) {
        is_target[target_client_id] = true;
        world_touch(w, TOUCH_GUN_TIMERS, gun->timers);
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
  for (int n = 0; n < num_players_in_range; n++) {
    const int i = character_team(w, &w->characters[players_in_range[n]]);
    if (target_id_in_team[i] != -1) {
      is_target[target_id_in_team[i]] = true;
      world_touch(w, TOUCH_GUN_TIMERS, gun->timers);
      timers->last_fire_team[i] = server_tick(w);
    }
  }

  /* in the order of the client ids, which is the order the bullets are created in */
  sort_ids(players_in_range, num_players_in_range);
  for (int n = 0; n < num_players_in_range; n++) {
    const int i = players_in_range[n];
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
  /* (like dragger_tick(): only written to if it moves) */
  const entity_t *still = entity_peek(w, index);

  if (is_mover_tick(w)) {
    vec2 core = still->u.gun.core;
    collision_mover_speed(collision(w), still->pos.x, still->pos.y, &core);
    const vec2 pos = vadd(still->pos, core);
    if (memcmp(&core, &still->u.gun.core, sizeof(core)) != 0 || memcmp(&pos, &still->pos, sizeof(pos)) != 0) {
      entity_t *moved = entity(w, index);
      moved->u.gun.core = core;
      moved->pos = pos;
    }
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
  entity_mark_for_destroy(w, ent);
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
    entity_mark_for_destroy(w, ent);
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
    entity_mark_for_destroy(w, ent);
    return;
  }
  character_t *target = get_player_char(w, plasma->for_client_id);
  /* Without a target, a plasma bullet has no reason to live */
  if (!target) {
    entity_mark_for_destroy(w, ent);
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
  light->settled = false;
  light->last_tick = server_tick(w);
  world_insert_entity(w, DDNET_ENTTYPE_LASER, index);
  light_step(w, index);
  return index;
}

/* CLight::HitCharacter with CGameWorld::IntersectedCharacters */
static void light_hit_character(world_t *w, entity_t *ent) {
  const ddnet_light_t *light = &ent->u.light;
  for (int id = w->first_entity[DDNET_ENTTYPE_CHARACTER]; id != -1; id = w->characters[id].link.next) {
    character_t *chr = &w->characters[id];
    /* the closest point is on the beam: too far from all of it is too far from that */
    const float reach = PHYSICAL_SIZE + 2.0f;
    if (chr->pos.x < minf(ent->pos.x, light->to.x) - reach ||
        chr->pos.x > maxf(ent->pos.x, light->to.x) + reach ||
        chr->pos.y < minf(ent->pos.y, light->to.y) - reach ||
        chr->pos.y > maxf(ent->pos.y, light->to.y) + reach)
      continue;
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

/* The steps of the ticks up to until that a laser wall did not tick on (it does not on the ticks no tee is
 * in its range, see lasers_tick()). */
static void light_catch_up(world_t *w, entity_t *ent, int until) {
  ddnet_light_t *light = &ent->u.light;
  if (light->settled)
    return;
  const int period = (int)(SERVER_TICK_SPEED * 0.15f);
  const int skipped = until / period - light->last_tick / period;
  if (skipped > 0) {
    for (int i = 0; i < skipped; i++)
      light_move(light);
    /* where the beam ends only depends on the last of them */
    const vec2 direction = v2(sinf(light->rotation), cosf(light->rotation));
    const vec2 next_position = vadd(ent->pos, vscale(vnormalize(direction), light->curve_length));
    collision_intersect_no_laser(collision(w), ent->pos, next_position, &light->to, NULL);
  }
}

/* ddnet_world_sync() for the laser walls. */
void lights_sync(world_t *w) {
  for (int index = w->first_static_laser; index != -1; index = entity_peek(w, index)->link.next) {
    const entity_t *ent = entity_peek(w, index);
    if (ent->kind != DDNET_ENTITY_LIGHT || ent->u.light.settled || ent->u.light.last_tick == server_tick(w))
      continue;
    entity_t *light = entity(w, index);
    light_catch_up(w, light, server_tick(w));
    light->u.light.last_tick = server_tick(w);
  }
}

/* CLight::Tick */
static void light_tick(world_t *w, int index) {
  entity_t *ent = entity(w, index);
  ddnet_light_t *light = &ent->u.light;

  light_catch_up(w, ent, server_tick(w) - 1);
  light->last_tick = server_tick(w);

  if (is_mover_tick(w)) {
    light->eval_tick = server_tick(w);
    if (!light->settled) {
      const ddnet_light_t before = *light;
      const vec2 pos = ent->pos;
      collision_mover_speed(collision(w), ent->pos.x, ent->pos.y, &light->core);
      ent->pos = vadd(ent->pos, light->core);
      light_step(w, index);
      /* Nothing changed and nothing will: the step only depends on what it
       * left unchanged and on the map. */
      light->settled = light->speed == 0 && light->angular_speed == 0 && veq(ent->pos, pos) &&
                       veq(light->core, before.core) && veq(light->to, before.to) &&
                       light->rotation == before.rotation && light->curve_length == before.curve_length &&
                       light->length_l == before.length_l;
    }
  }

  light_hit_character(w, ent);
}

/* -------------------------------------------------------------- dispatch */

void entity_tick(world_t *w, int index) {
  switch (entity_peek(w, index)->kind) {
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
