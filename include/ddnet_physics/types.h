/* Constants and types that both implementations share. */
#ifndef DDNET_PHYSICS_TYPES_H
#define DDNET_PHYSICS_TYPES_H

#include "vmath.h"

#include <stdbool.h>
#include <stdint.h>

/* Number of player slots. This is DDNet's MAX_CLIENTS and changes the layout
 * of the structs below: the library and everything that uses it must agree on
 * it. The CMake target exports the value it was built with. */
#ifndef DDNET_MAX_CLIENTS
#define DDNET_MAX_CLIENTS 128
#endif

enum {
  DDNET_TEAM_FLOCK = 0, /* team 0: everybody who is not in a team, every tee races for itself */
  DDNET_NUM_TEAMS = DDNET_MAX_CLIENTS,
  DDNET_MAX_CHECKPOINTS = 25,
};

/* A door of the map (CDoor), to draw it: a laser from to (where the line of
 * its length from pos hits something solid or a no-laser tile) to pos, shown
 * for a team while switchers[number].status[team] is set, which is when it
 * blocks (DDNet sends the switch number with it and leaves this to the
 * client). length and direction are what it was made with. */
typedef struct ddnet_map_door_t {
  ddnet_vec2_t pos, to;
  ddnet_vec2_t direction;
  int length;
  int number;
} ddnet_map_door_t;

enum {
  DDNET_WEAPON_HAMMER = 0,
  DDNET_WEAPON_GUN,
  DDNET_WEAPON_SHOTGUN,
  DDNET_WEAPON_GRENADE,
  DDNET_WEAPON_LASER,
  DDNET_WEAPON_NINJA,
  DDNET_NUM_WEAPONS,
  /* Kill reasons. */
  DDNET_WEAPON_GAME = -3,  /* disconnecting, team switching */
  DDNET_WEAPON_SELF = -2,  /* console kill command */
  DDNET_WEAPON_WORLD = -1, /* death tiles etc. */
};

enum {
  DDNET_HOOK_RETRACTED = -1,
  DDNET_HOOK_IDLE = 0,
  DDNET_HOOK_RETRACT_START = 1,
  DDNET_HOOK_RETRACT_END = 3,
  DDNET_HOOK_FLYING = 4,
  DDNET_HOOK_GRABBED = 5,
};

/* ddnet_character_core_t::triggered_events */
enum {
  DDNET_COREEVENT_GROUND_JUMP = 0x01,
  DDNET_COREEVENT_AIR_JUMP = 0x02,
  DDNET_COREEVENT_HOOK_LAUNCH = 0x04,
  DDNET_COREEVENT_HOOK_ATTACH_PLAYER = 0x08,
  DDNET_COREEVENT_HOOK_ATTACH_GROUND = 0x10,
  DDNET_COREEVENT_HOOK_HIT_NOHOOK = 0x20,
  DDNET_COREEVENT_HOOK_RETRACT = 0x40,
};

/* ddnet_input_t::player_flags */
enum {
  DDNET_PLAYERFLAG_PLAYING = 1 << 0,
  DDNET_PLAYERFLAG_IN_MENU = 1 << 1,
  DDNET_PLAYERFLAG_CHATTING = 1 << 2,
  DDNET_PLAYERFLAG_SCOREBOARD = 1 << 3,
  DDNET_PLAYERFLAG_AIM = 1 << 4,
  DDNET_PLAYERFLAG_SPEC_CAM = 1 << 5,
};

typedef enum ddnet_race_state_t {
  DDNET_RACE_NONE = 0,
  DDNET_RACE_STARTED,
  DDNET_RACE_CHEATED,
  DDNET_RACE_FINISHED,
} ddnet_race_state_t;

typedef enum ddnet_team_state_t {
  DDNET_TEAMSTATE_EMPTY = 0,
  DDNET_TEAMSTATE_OPEN,
  DDNET_TEAMSTATE_STARTED,
  /* a tee that has not hit the start line left the team */
  DDNET_TEAMSTATE_STARTED_UNFINISHABLE,
  DDNET_TEAMSTATE_FINISHED,
} ddnet_team_state_t;

/* ddnet_player_t::paused */
enum {
  DDNET_PAUSE_NONE = 0,
  DDNET_PAUSE_PAUSED, /* spectating while the tee stays in the world */
  DDNET_PAUSE_SPEC,   /* spectating with the tee taken out of the world */
};

enum {
  DDNET_POWERUP_HEALTH = 0,
  DDNET_POWERUP_ARMOR,
  DDNET_POWERUP_WEAPON,
  DDNET_POWERUP_NINJA,
  DDNET_POWERUP_ARMOR_SHOTGUN,
  DDNET_POWERUP_ARMOR_GRENADE,
  DDNET_POWERUP_ARMOR_NINJA,
  DDNET_POWERUP_ARMOR_LASER,
  DDNET_POWERUP_FREEZE,
};

/* What a client sends every tick (CNetObj_PlayerInput).
 *
 * fire, next_weapon and prev_weapon are counters, not buttons: the lowest bit
 * of fire is "button is down", and every press or release increments it. */
typedef struct ddnet_input_t {
  int direction; /* -1 left, 0, 1 right */
  int target_x;  /* aim, relative to the tee */
  int target_y;
  int jump;
  int fire;
  int hook;
  int player_flags;
  int wanted_weapon; /* 0 = keep, otherwise weapon + 1 */
  int next_weapon;
  int prev_weapon;
} ddnet_input_t;

#endif
