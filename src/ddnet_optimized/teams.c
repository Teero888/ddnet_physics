/* Port of the parts of CGameTeams (src/game/server/teams.cpp) and CTeamsCore
 * (src/game/teamscore.cpp) that influence the physics: who collides with whom,
 * per team switch states, and the race state machine of tees and teams.
 *
 * Chat messages, score saving, team invitations, swap requests, team 0 mode
 * and practice mode are left out.
 *
 * DDNet keeps everything of every team there can be, by team number. Here a
 * team has a row in world->teams while somebody is in it (which number a team
 * has does not matter for that), and the player of a client knows the row of
 * its team. A team without a row is as nobody has ever been in it.
 *
 * When the last tee leaves a team, DDNet resets it to that, nearly: the row
 * goes then, unless something would still be different for the next tee that
 * joins it: team 0 is not reset when it empties (its switches stay as they
 * are), a dragger can still aim at a tee "for" the team, or a turret still waits
 * after firing at it. Such a row goes as soon as that is over and a team
 * changes again (teams_release_rows()). */
#include "internal.h"

/* ------------------------------------------------------------ CTeamsCore */

bool teams_can_keep_hook(const world_t *w, int client_id1, int client_id2) {
  if (client_id1 == client_id2)
    return true;
  return teams_team(w, client_id1) == teams_team(w, client_id2);
}

bool teams_get_solo(const world_t *w, int client_id) {
  /* (a slot that nobody used is not solo) */
  if (client_id < 0 || client_id >= w->num_clients)
    return false;
  return w->players[client_id].is_solo;
}

/* ------------------------------------------------------------- the rows */

int team_row_get(world_t *w, int team) {
  const int row = team_row_find(w, team);
  return row >= 0 ? row : team_row_make(w, team);
}

/* Whether the row of a team can go: nobody is in the team, and what DDNet
 * would carry over to the next tee that joins it is what a team without a
 * row gets too (see above). */
bool teams_row_unneeded(const world_t *w, int row) {
  const ddnet_team_t *team = &w->teams[row];
  if (team->number == FREE_TEAM_ROW || team->state != DDNET_TEAMSTATE_EMPTY || team->locked || team->kill_tick != -1)
    return false;
  /* (a client that is not connected keeps its team, like in DDNet) */
  for (int i = 0; i < w->num_clients; i++)
    if (w->players[i].team == team->number)
      return false;
  /* The switches: a team without a row gets them reset when somebody joins
   * it, this one too unless they were not touched since its last reset: as
   * that left them then. (last_update_tick is not looked at by anything.) */
  if (!team->switches_touched)
    for (int s = 0; s < w->num_switchers; s++) {
      const ddnet_switch_state_t *state = &w->switch_states[row * w->num_switchers + s];
      if (state->status != w->switch_initial[s] || state->end_tick != 0 || state->type != TILE_SWITCHOPEN)
        return false;
    }
  for (int t = 0; t < w->num_dragger_targets; t++)
    if (dragger_table(w, t)[1 + row] != -1)
      return false;
  /* (a turret waits SERVER_TICK_SPEED / sv_plasma_per_sec ticks after it fired, at most a second) */
  const int stride = w->table_teams + w->table_clients;
  for (int g = 0; g < w->num_guns; g++) {
    const int last = w->gun_timers[g * stride + row];
    if (last != 0 && last + SERVER_TICK_SPEED > server_tick(w))
      return false;
  }
  return true;
}



/* ------------------------------------------------------------ CGameTeams */

bool teams_is_valid_team_number(int team) { return team >= TEAM_FLOCK && team < NUM_DDRACE_TEAMS; }

bool teams_team_locked(const world_t *w, int team) {
  if (team == TEAM_FLOCK || !teams_is_valid_team_number(team))
    return false;
  const int row = team_row_find(w, team);
  return row >= 0 && w->teams[row].locked;
}

void teams_set_team_lock(world_t *w, int team, bool lock) {
  if (team == TEAM_FLOCK || !teams_is_valid_team_number(team))
    return;
  /* (a team without a row is not locked) */
  const int row = lock ? team_row_get(w, team) : team_row_find(w, team);
  if (row >= 0)
    w->teams[row].locked = lock;
}

/* How many client slots that nobody used yet are in a team (DDNet counts
 * every slot): they are where teams_reset() put them. */
static int teams_unused_slots_in(const world_t *w, int team) {
  if (w->slots_forced_solo)
    return team >= w->num_clients && team < MAX_CLIENTS;
  return team == TEAM_FLOCK ? MAX_CLIENTS - w->num_clients : 0;
}

int teams_team_size(const world_t *w, int team) {
  int count = teams_unused_slots_in(w, team);
  for (int i = 0; i < w->num_clients; ++i)
    if (teams_team(w, i) == team)
      count++;
  return count;
}

/* The switches of a team that has its row (out of line: a team is reset on
 * every spawn, and this is seldom more than the check before it). */
DDNET_NOINLINE static void teams_reset_switch_row(world_t *w, int row) {
  ddnet_switch_state_t *states = &w->switch_states[row * w->num_switchers];
  for (int i = 0; i < w->num_switchers; i++) {
    /* (only the ones that are not like that already are written to, and noted for ddnet_world_copy()) */
    if (states[i].status == w->switch_initial[i] && states[i].end_tick == 0 && states[i].type == TILE_SWITCHOPEN)
      continue;
    world_touch(w, TOUCH_SWITCHER, row * w->num_switchers + i);
    states[i].status = w->switch_initial[i];
    states[i].end_tick = 0;
    states[i].type = TILE_SWITCHOPEN;
  }
}

/* CGameTeams::ResetSwitchers of the team of a row */
void teams_reset_switchers(world_t *w, int row) {
  /* (they are like that already: nothing was written to them since the last time) */
  if (!w->teams[row].switches_touched)
    return;
  w->teams[row].switches_touched = false;
  teams_reset_switch_row(w, row);
}

static void teams_reset_round_state(world_t *w, int row) {
  teams_reset_switchers(w, row);
  w->teams[row].kill_tick = -1;
}

/* CGameTeams::Reset and CTeamsCore::Reset, when the world is made */
void teams_reset(world_t *w) {
  w->slots_forced_solo = w->config.sv_team == DDNET_SV_TEAM_FORCED_SOLO;
  for (int i = 0; i < w->client_capacity; ++i) {
    player_t *slot = &w->players[i];
    slot->team = w->slots_forced_solo ? i : TEAM_FLOCK;
    slot->team_row = NO_TEAM_ROW;
    slot->is_solo = false;
    slot->tee_started = false;
    slot->tee_finished = false;
  }
  for (int row = 0; row < w->num_team_rows; ++row) {
    w->teams[row].state = DDNET_TEAMSTATE_EMPTY;
    w->teams[row].locked = false;
    teams_reset_round_state(w, row);
  }
  /* the switches of all the teams without a row at once */
  if (w->rowless_switches_touched) {
    w->rowless_switches_touched = false;
    w->rowless_switches_reset = true;
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
    w->players[client_id].tee_started = true;
    starting_char->race_state = DDNET_RACE_STARTED;
    starting_char->start_time = tick;
    return;
  }
  bool waiting = false;
  for (int i = 0; i < w->num_clients; ++i) {
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
    w->players[client_id].tee_started = true;

  ddnet_team_t *team = &w->teams[teams_team_row(w, client_id)];
  if (team->state < DDNET_TEAMSTATE_STARTED && !waiting) {
    team->state = DDNET_TEAMSTATE_STARTED;
    team->kill_tick = -1;

    /* the whole team starts together */
    for (int i = 0; i < w->num_clients; ++i) {
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
  EMIT_PARTICLE(w, chr->pos, DDNET_PARTICLE_CONFETTI, client_id);
}

/* (row: the row the team has or had, which callers know; a row that went is empty) */
static bool teams_team_finished(const world_t *w, int team, int row) {
  if (w->teams[row].state != DDNET_TEAMSTATE_STARTED || w->teams[row].number != team)
    return false;
  /* (a slot that nobody used never finished) */
  if (teams_unused_slots_in(w, team))
    return false;
  for (int i = 0; i < w->num_clients; ++i)
    if (teams_team(w, i) == team && !w->players[i].tee_finished)
      return false;
  return true;
}

/* CGameTeams::CheckTeamFinished: a team finishes when all of its tees did. */
void teams_check_team_finished(world_t *w, int team, int row) {
  if (teams_team_finished(w, team, row)) {
    int team_players[MAX_CLIENTS];
    unsigned int players_count = 0;

    for (int i = 0; i < w->num_clients; ++i) {
      if (team == teams_team(w, i)) {
        if (w->players[i].active && player_is_playing(w, i)) {
          w->players[i].tee_started = false;
          w->players[i].tee_finished = false;

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
      /* (it has a row: it started) */
      w->teams[row].state = DDNET_TEAMSTATE_FINISHED; /* TODO: Make it better */

      /* CGameTeams::OnTeamFinish: unlocked teams go back to team 0 */
      for (unsigned int i = 0; i < players_count; i++) {
        const int client_id = team_players[i];
        if (w->config.sv_rejoin_team_0 && w->config.sv_team != DDNET_SV_TEAM_FORCED_SOLO &&
            (!teams_is_valid_team_number(teams_team(w, client_id)) ||
             !teams_team_locked(w, teams_team(w, client_id))))
          teams_set_force_character_team(w, client_id, TEAM_FLOCK);
      }
    }
  }
}

/* CGameTeams::OnCharacterFinish: a tee touched a finish tile. */
void teams_on_character_finish(world_t *w, int client_id) {
  if (teams_team(w, client_id) == TEAM_FLOCK && w->config.sv_team != DDNET_SV_TEAM_FORCED_SOLO) {
    if (w->players[client_id].active && player_is_playing(w, client_id)) {
      int time_ticks = server_tick(w) - teams_get_start_time(w, client_id);
      if (time_ticks <= 0)
        return;
      teams_on_finish(w, client_id, time_ticks);
    }
  } else {
    if (w->players[client_id].tee_started)
      w->players[client_id].tee_finished = true;
    teams_check_team_finished(w, teams_team(w, client_id), teams_team_row(w, client_id));
  }
}

/* CGameTeams::SetForceCharacterTeam */
void teams_set_force_character_team(world_t *w, int client_id, int team) {
  /* The rows of both teams (no memory for one of them: the player stays where
   * it is). The one it is in first: a new row can free rows of teams nobody
   * is in, and the new team has nobody in it yet. */
  const int old_team = teams_team(w, client_id);
  /* (a connected player knows its row; respawning puts it into the team it is in) */
  const int known = w->players[client_id].team_row;
  const int old_row = known != NO_TEAM_ROW && w->teams[known].number == old_team ? known : team_row_get(w, old_team);
  if (old_row < 0)
    return;
  const int row = team == old_team ? old_row : team_row_get(w, team);
  if (row < 0)
    return;
  player_t *player = &w->players[client_id];
  player->tee_started = false;
  player->tee_finished = false;

  if (team != old_team && (old_team != TEAM_FLOCK || w->config.sv_team == DDNET_SV_TEAM_FORCED_SOLO) &&
      w->teams[old_row].state != DDNET_TEAMSTATE_EMPTY) {
    bool no_else_in_old_team = teams_team_size(w, old_team) <= 1;
    if (no_else_in_old_team) {
      w->teams[old_row].state = DDNET_TEAMSTATE_EMPTY;

      /* unlock team when last player leaves */
      teams_set_team_lock(w, old_team, false);
      teams_reset_round_state(w, old_row);
    }
  }

  player->team = team;
  player->team_row = row;

  if (old_team != team)
    world_remove_entities_from_player(w, client_id);
  /* (the team it left may have nobody in it now) */
  if (old_team != team && teams_row_unneeded(w, old_row))
    w->teams[old_row].number = FREE_TEAM_ROW;

  if (w->teams[row].state == DDNET_TEAMSTATE_EMPTY || w->teams[row].locked) {
    if (!w->teams[row].locked)
      w->teams[row].state = DDNET_TEAMSTATE_OPEN;

    teams_reset_switchers(w, row);
  }
}

/* CGameTeams::KillTeam */
static void teams_kill_team(world_t *w, int team, int new_strong_id, int except_id) {
  for (int i = 0; i < w->num_clients; i++) {
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
  w->players[client_id].is_solo = false;
  int team = teams_team(w, client_id);
  const int row = teams_team_row(w, client_id);

  if (!teams_is_valid_team_number(team) || !teams_team_locked(w, team)) {
    if (w->config.sv_team != DDNET_SV_TEAM_FORCED_SOLO)
      teams_set_force_character_team(w, client_id, TEAM_FLOCK);
    else
      teams_set_force_character_team(w, client_id, client_id); /* initialize team */
    teams_check_team_finished(w, team, row);
  }
}

/* CGameTeams::OnCharacterDeath */
void teams_on_character_death(world_t *w, int client_id, int weapon) {
  const ddnet_config_t *config = &w->config;
  w->players[client_id].is_solo = false;

  int team = teams_team(w, client_id);
  const int row = teams_team_row(w, client_id);
  bool locked = teams_team_locked(w, team) && weapon != WEAPON_GAME;

  if (config->sv_team == DDNET_SV_TEAM_FORCED_SOLO) {
    w->teams[row].state = DDNET_TEAMSTATE_OPEN;
    teams_reset_round_state(w, row);
  } else if (locked) {
    teams_set_force_character_team(w, client_id, team);

    if (w->teams[row].state != DDNET_TEAMSTATE_OPEN) {
      w->teams[row].state = DDNET_TEAMSTATE_OPEN;

      if (!config->sv_pauseable) {
        for (int client_id1 = 0; client_id1 < w->num_clients; client_id1++) {
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
    if (w->teams[row].state == DDNET_TEAMSTATE_STARTED && !w->players[client_id].tee_started) {
      /* The team cannot finish anymore because a tee left before hitting the
       * start. On a server it would have 60 seconds to enter practice mode. */
      w->teams[row].kill_tick = server_tick(w) + 60 * SERVER_TICK_SPEED;
      w->unfinishable_teams = true;
      w->teams[row].state = DDNET_TEAMSTATE_STARTED_UNFINISHABLE;
    }
    teams_set_force_character_team(w, client_id, TEAM_FLOCK);
    teams_check_team_finished(w, team, row);
  }
}

/* CGameTeams::Tick */
void teams_tick(world_t *w) {
  int now = server_tick(w);

  /* The teams with a kill tick, in the order of their numbers like DDNet's
   * loop (killing a team can add rows). */
  int numbers[DDNET_NUM_TEAMS];
  int count = 0;
  for (int row = 0; row < w->num_team_rows; row++) {
    if (w->teams[row].kill_tick == -1)
      continue;
    int at = count++;
    while (at > 0 && numbers[at - 1] > w->teams[row].number) {
      numbers[at] = numbers[at - 1];
      at--;
    }
    numbers[at] = w->teams[row].number;
  }
  for (int n = 0; n < count; n++) {
    const ddnet_team_t *team = &w->teams[team_row_find(w, numbers[n])];
    if (team->state != DDNET_TEAMSTATE_STARTED_UNFINISHABLE)
      continue;
    if (now >= team->kill_tick) {
      /* The team is killed because it cannot finish anymore. */
      teams_kill_team(w, numbers[n], -1, -1);
    }
  }
  w->unfinishable_teams = count > 0;
}
