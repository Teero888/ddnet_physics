/* Port of the parts of CGameTeams (src/game/server/teams.cpp) and CTeamsCore
 * (src/game/teamscore.cpp) that influence the physics: who collides with whom,
 * per team switch states, and the race state machine of tees and teams.
 *
 * Chat messages, score saving, team invitations, swap requests, team 0 mode
 * and practice mode are left out. */
#include "internal.h"

/* ------------------------------------------------------------ CTeamsCore */

bool teams_can_keep_hook(const world_t *w, int client_id1, int client_id2) {
  const int *team = w->teams.team;
  if (client_id1 == client_id2)
    return true;
  return team[client_id1] == team[client_id2];
}

bool teams_can_collide(const world_t *w, int client_id1, int client_id2) {
  const int *team = w->teams.team;
  if (client_id1 == client_id2)
    return true;
  if (w->teams.is_solo[client_id1] || w->teams.is_solo[client_id2])
    return false;
  return team[client_id1] == team[client_id2];
}

bool teams_get_solo(const world_t *w, int client_id) {
  if (client_id < 0 || client_id >= MAX_CLIENTS)
    return false;
  return w->teams.is_solo[client_id];
}

/* ------------------------------------------------------------ CGameTeams */

bool teams_is_valid_team_number(int team) { return team >= TEAM_FLOCK && team < NUM_DDRACE_TEAMS; }

bool teams_team_locked(const world_t *w, int team) {
  if (team == TEAM_FLOCK || !teams_is_valid_team_number(team))
    return false;
  return w->teams.team_locked[team];
}

void teams_set_team_lock(world_t *w, int team, bool lock) {
  if (team != TEAM_FLOCK && teams_is_valid_team_number(team))
    w->teams.team_locked[team] = lock;
}

int teams_team_size(const world_t *w, int team) {
  int count = 0;
  for (int i = 0; i < MAX_CLIENTS; ++i)
    if (teams_team(w, i) == team)
      count++;

  return count;
}

void teams_reset_switchers(world_t *w, int team) {
  for (int i = 0; i < w->num_switchers; i++) {
    ddnet_switcher_t *switcher = &switchers(w)[i];
    switcher->status[team] = switcher->initial;
    switcher->end_tick[team] = 0;
    switcher->type[team] = TILE_SWITCHOPEN;
  }
}

static void teams_reset_round_state(world_t *w, int team) {
  teams_reset_switchers(w, team);
  w->teams.team_unfinishable_kill_tick[team] = -1;
}

/* CGameTeams::Reset and CTeamsCore::Reset */
void teams_reset(world_t *w) {
  teams_t *teams = &w->teams;
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (w->config.sv_team == DDNET_SV_TEAM_FORCED_SOLO)
      teams->team[i] = i;
    else
      teams->team[i] = TEAM_FLOCK;
    teams->is_solo[i] = false;
  }

  for (int i = 0; i < MAX_CLIENTS; ++i) {
    teams->tee_started[i] = false;
    teams->tee_finished[i] = false;
  }

  for (int i = 0; i < NUM_DDRACE_TEAMS; ++i) {
    teams->team_state[i] = DDNET_TEAMSTATE_EMPTY;
    teams->team_locked[i] = false;
    teams_reset_round_state(w, i);
  }
}

/* CPlayer::IsPlaying */
static bool player_is_playing(world_t *w, int client_id) {
  return w->players[client_id].has_character && w->characters[client_id].alive;
}

static ddnet_race_state_t teams_get_race_state(world_t *w, int client_id) {
  character_t *chr = get_player_char(w, client_id);
  if (chr)
    return chr->race_state;
  return DDNET_RACE_NONE;
}

static void teams_set_race_state(world_t *w, int client_id, ddnet_race_state_t state) {
  character_t *chr = get_player_char(w, client_id);
  if (chr)
    chr->race_state = state;
}

static int teams_get_start_time(world_t *w, int client_id) {
  character_t *chr = get_player_char(w, client_id);
  if (chr)
    return chr->start_time;
  return 0;
}

static void teams_set_start_time(world_t *w, int client_id, int start_time) {
  character_t *chr = get_player_char(w, client_id);
  if (chr)
    chr->start_time = start_time;
}

/* CGameTeams::OnCharacterStart: a tee touched a start tile. */
void teams_on_character_start(world_t *w, int client_id) {
  teams_t *teams = &w->teams;
  const ddnet_config_t *config = &w->config;
  int tick = server_tick(w);
  character_t *starting_char = get_player_char(w, client_id);
  if (!starting_char)
    return;
  if (config->sv_team == DDNET_SV_TEAM_FORCED_SOLO && starting_char->race_state == DDNET_RACE_STARTED)
    return;
  if ((config->sv_team == DDNET_SV_TEAM_FORCED_SOLO || teams_team(w, client_id) != TEAM_FLOCK) &&
      starting_char->race_state == DDNET_RACE_FINISHED)
    return;
  if (config->sv_team != DDNET_SV_TEAM_FORCED_SOLO && teams_team(w, client_id) == TEAM_FLOCK) {
    /* team 0: every tee races for itself and can restart at any time */
    teams->tee_started[client_id] = true;
    starting_char->race_state = DDNET_RACE_STARTED;
    starting_char->start_time = tick;
    return;
  }
  bool waiting = false;
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (teams_team(w, client_id) != teams_team(w, i))
      continue;
    if (!w->players[i].active || !player_is_playing(w, i))
      continue;
    if (teams_get_race_state(w, i) != DDNET_RACE_FINISHED)
      continue;

    /* Somebody of the team finished and did not go through start yet. */
    waiting = true;
    starting_char->race_state = DDNET_RACE_NONE;
  }

  if (!waiting)
    teams->tee_started[client_id] = true;

  if (teams->team_state[teams_team(w, client_id)] < DDNET_TEAMSTATE_STARTED && !waiting) {
    teams->team_state[teams_team(w, client_id)] = DDNET_TEAMSTATE_STARTED;
    teams->team_unfinishable_kill_tick[teams_team(w, client_id)] = -1;

    /* the whole team starts together */
    for (int i = 0; i < MAX_CLIENTS; ++i) {
      if (teams_team(w, client_id) == teams_team(w, i)) {
        if (w->players[i].active &&
            (player_is_playing(w, i) || teams_team_locked(w, teams_team(w, client_id)))) {
          teams_set_race_state(w, i, DDNET_RACE_STARTED);
          teams_set_start_time(w, i, tick);
        }
      }
    }
  }
}

/* CGameTeams::OnFinish */
static void teams_on_finish(world_t *w, int client_id, int time_ticks) {
  if (!w->players[client_id].active || !player_is_playing(w, client_id))
    return;

  character_t *chr = get_player_char(w, client_id);
  chr->last_time_cp = -1;

  w->players[client_id].finish_tick = server_tick(w);
  w->players[client_id].finish_time_ticks = time_ticks;

  teams_set_race_state(w, client_id, DDNET_RACE_FINISHED);

  /* Confetti */
  create_particle(w, chr->pos, DDNET_PARTICLE_CONFETTI, client_id);
}

static bool teams_team_finished(const world_t *w, int team) {
  if (w->teams.team_state[team] != DDNET_TEAMSTATE_STARTED)
    return false;

  for (int i = 0; i < MAX_CLIENTS; ++i)
    if (teams_team(w, i) == team && !w->teams.tee_finished[i])
      return false;
  return true;
}

/* CGameTeams::CheckTeamFinished: a team finishes when all of its tees did. */
void teams_check_team_finished(world_t *w, int team) {
  teams_t *teams = &w->teams;
  if (teams_team_finished(w, team)) {
    int team_players[MAX_CLIENTS];
    unsigned int players_count = 0;

    for (int i = 0; i < MAX_CLIENTS; ++i) {
      if (team == teams_team(w, i)) {
        if (w->players[i].active && player_is_playing(w, i)) {
          teams->tee_started[i] = false;
          teams->tee_finished[i] = false;

          team_players[players_count++] = i;
        }
      }
    }

    if (players_count > 0) {
      int time_ticks = server_tick(w) - teams_get_start_time(w, team_players[0]);
      if (time_ticks <= 0)
        return;

      for (unsigned int i = 0; i < players_count; ++i)
        teams_on_finish(w, team_players[i], time_ticks);
      teams->team_state[team] = DDNET_TEAMSTATE_FINISHED; /* TODO: Make it better */

      /* CGameTeams::OnTeamFinish: unlocked teams go back to team 0 */
      for (unsigned int i = 0; i < players_count; i++) {
        const int client_id = team_players[i];
        if (w->config.sv_rejoin_team_0 && w->config.sv_team != DDNET_SV_TEAM_FORCED_SOLO &&
            (!teams_is_valid_team_number(teams_team(w, client_id)) ||
             !teams->team_locked[teams_team(w, client_id)]))
          teams_set_force_character_team(w, client_id, TEAM_FLOCK);
      }
    }
  }
}

/* CGameTeams::OnCharacterFinish: a tee touched a finish tile. */
void teams_on_character_finish(world_t *w, int client_id) {
  teams_t *teams = &w->teams;
  if (teams_team(w, client_id) == TEAM_FLOCK && w->config.sv_team != DDNET_SV_TEAM_FORCED_SOLO) {
    if (w->players[client_id].active && player_is_playing(w, client_id)) {
      int time_ticks = server_tick(w) - teams_get_start_time(w, client_id);
      if (time_ticks <= 0)
        return;
      teams_on_finish(w, client_id, time_ticks);
    }
  } else {
    if (teams->tee_started[client_id])
      teams->tee_finished[client_id] = true;
    teams_check_team_finished(w, teams_team(w, client_id));
  }
}

/* CGameTeams::SetForceCharacterTeam */
void teams_set_force_character_team(world_t *w, int client_id, int team) {
  teams_t *teams = &w->teams;
  teams->tee_started[client_id] = false;
  teams->tee_finished[client_id] = false;
  int old_team = teams_team(w, client_id);

  if (team != old_team && (old_team != TEAM_FLOCK || w->config.sv_team == DDNET_SV_TEAM_FORCED_SOLO) &&
      teams->team_state[old_team] != DDNET_TEAMSTATE_EMPTY) {
    bool no_else_in_old_team = teams_team_size(w, old_team) <= 1;
    if (no_else_in_old_team) {
      teams->team_state[old_team] = DDNET_TEAMSTATE_EMPTY;

      /* unlock team when last player leaves */
      teams_set_team_lock(w, old_team, false);
      teams_reset_round_state(w, old_team);
    }
  }

  teams->team[client_id] = team;

  if (old_team != team)
    world_remove_entities_from_player(w, client_id);

  if (teams->team_state[team] == DDNET_TEAMSTATE_EMPTY || teams->team_locked[team]) {
    if (!teams->team_locked[team])
      teams->team_state[team] = DDNET_TEAMSTATE_OPEN;

    teams_reset_switchers(w, team);
  }
}

/* CGameTeams::KillTeam */
static void teams_kill_team(world_t *w, int team, int new_strong_id, int except_id) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (teams_team(w, i) == team && w->players[i].active) {
      if (i != except_id) {
        player_kill_character(w, i, WEAPON_SELF);
        if (new_strong_id != -1 && i != new_strong_id)
          player_respawn(w, i, true); /* spawn the rest of team with weak hook on the killer */
      }
    }
  }
}

/* CGameTeams::OnCharacterSpawn */
void teams_on_character_spawn(world_t *w, int client_id) {
  teams_t *teams = &w->teams;
  teams->is_solo[client_id] = false;
  int team = teams_team(w, client_id);

  if (!teams_is_valid_team_number(team) || !teams->team_locked[team]) {
    if (w->config.sv_team != DDNET_SV_TEAM_FORCED_SOLO)
      teams_set_force_character_team(w, client_id, TEAM_FLOCK);
    else
      teams_set_force_character_team(w, client_id, client_id); /* initialize team */
    teams_check_team_finished(w, team);
  }
}

/* CGameTeams::OnCharacterDeath */
void teams_on_character_death(world_t *w, int client_id, int weapon) {
  teams_t *teams = &w->teams;
  const ddnet_config_t *config = &w->config;
  teams->is_solo[client_id] = false;

  int team = teams_team(w, client_id);
  bool locked = teams_team_locked(w, team) && weapon != WEAPON_GAME;

  if (config->sv_team == DDNET_SV_TEAM_FORCED_SOLO) {
    teams->team_state[team] = DDNET_TEAMSTATE_OPEN;
    teams_reset_round_state(w, team);
  } else if (locked) {
    teams_set_force_character_team(w, client_id, team);

    if (teams->team_state[team] != DDNET_TEAMSTATE_OPEN) {
      teams->team_state[team] = DDNET_TEAMSTATE_OPEN;

      if (!config->sv_pauseable) {
        for (int client_id1 = 0; client_id1 < MAX_CLIENTS; client_id1++) {
          if (teams_team(w, client_id1) == team) {
            if (w->players[client_id1].active && w->players[client_id1].paused == DDNET_PAUSE_SPEC)
              player_pause(w, client_id1, DDNET_PAUSE_PAUSED, true);
          }
        }
      }

      if (teams_team_size(w, team) > 1) {
        /* Disband team if the team has more players than allowed. */
        if (teams_team_size(w, team) > config->sv_max_team_size) {
          teams_set_team_lock(w, team, false);
          teams_kill_team(w, team, weapon == WEAPON_SELF ? client_id : -1, client_id);
          return;
        }

        /* Everyone in a locked team is killed when one of them dies. */
        teams_kill_team(w, team, weapon == WEAPON_SELF ? client_id : -1, client_id);
      }
    }
  } else {
    if (teams->team_state[teams_team(w, client_id)] == DDNET_TEAMSTATE_STARTED &&
        !teams->tee_started[client_id]) {
      /* The team cannot finish anymore because a tee left before hitting the
       * start. On a server it would have 60 seconds to enter practice mode. */
      teams->team_unfinishable_kill_tick[team] = server_tick(w) + 60 * SERVER_TICK_SPEED;
      teams->team_state[team] = DDNET_TEAMSTATE_STARTED_UNFINISHABLE;
    }
    teams_set_force_character_team(w, client_id, TEAM_FLOCK);
    teams_check_team_finished(w, team);
  }
}

/* CGameTeams::Tick */
void teams_tick(world_t *w) {
  teams_t *teams = &w->teams;
  int now = server_tick(w);

  for (int i = 0; i < NUM_DDRACE_TEAMS; i++) {
    if (teams->team_unfinishable_kill_tick[i] == -1 ||
        teams->team_state[i] != DDNET_TEAMSTATE_STARTED_UNFINISHABLE) {
      continue;
    }
    if (now >= teams->team_unfinishable_kill_tick[i]) {
      /* The team is killed because it cannot finish anymore. */
      teams_kill_team(w, i, -1, -1);
    }
  }
}
