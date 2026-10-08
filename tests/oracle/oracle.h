// Test oracle for ddnet_physics, compiled into a patched DDNet server.
//
// When DDNET_ORACLE_SCRIPT and DDNET_ORACLE_DUMP are set, the server stops
// following the wall clock: it connects the scripted players, feeds them the
// scripted inputs tick by tick, writes the full physics state after every tick
// and exits when the script ends. See tests/oracle/record.h for the formats.
//
// This file is included at the end of src/engine/server/server.cpp, which is
// compiled with -fno-access-control so that private state can be dumped.
#include "oracle_record.h"

#include <game/server/entities/character.h>
#include <game/server/entities/door.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>
#include <game/server/teams.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// What CWorldCore::RandomOr0 is replaced with, see patch_ddnet.py.
int g_OracleTeleOut = 0;

namespace {

struct COracle
{
	bool m_Checked = false;
	bool m_Active = false;
	int m_NumPlayers = 0;
	int m_NumTicks = 0;
	int m_TicksDone = 0;
	std::vector<oracle_step_t> m_vSteps;
	FILE *m_pDump = nullptr;

	void Init()
	{
		m_Checked = true;
		const char *pScript = getenv("DDNET_ORACLE_SCRIPT");
		const char *pDump = getenv("DDNET_ORACLE_DUMP");
		if(!pScript || !pDump)
			return;

		FILE *pFile = fopen(pScript, "rb");
		int32_t aHeader[4];
		if(!pFile || fread(aHeader, sizeof(aHeader), 1, pFile) != 1 || aHeader[0] != ORACLE_SCRIPT_MAGIC || aHeader[1] != ORACLE_VERSION)
		{
			fprintf(stderr, "oracle: cannot read script '%s'\n", pScript);
			exit(2);
		}
		m_NumPlayers = aHeader[2];
		m_NumTicks = aHeader[3];
		m_vSteps.resize((size_t)m_NumPlayers * m_NumTicks);
		if(fread(m_vSteps.data(), sizeof(oracle_step_t), m_vSteps.size(), pFile) != m_vSteps.size())
		{
			fprintf(stderr, "oracle: truncated script '%s'\n", pScript);
			exit(2);
		}
		fclose(pFile);

		m_pDump = fopen(pDump, "wb");
		if(!m_pDump)
		{
			fprintf(stderr, "oracle: cannot write dump '%s'\n", pDump);
			exit(2);
		}
		aHeader[0] = ORACLE_DUMP_MAGIC;
		fwrite(aHeader, sizeof(aHeader), 1, m_pDump);
		m_Active = true;
	}

	const oracle_step_t &Step(int Player) const
	{
		return m_vSteps[(size_t)m_TicksDone * m_NumPlayers + Player];
	}

	void Put(int32_t Value) const { fwrite(&Value, sizeof(Value), 1, m_pDump); }
	static int32_t Bits(float Value)
	{
		int32_t Result;
		memcpy(&Result, &Value, sizeof(Result));
		return Result;
	}

	void DumpCharacter(CGameContext *pGame, int ClientId) const
	{
		int32_t aRec[REC_NUM] = {};
		CPlayer *pPlayer = pGame->m_apPlayers[ClientId];
		CCharacter *pChr = pPlayer ? pPlayer->GetCharacter() : nullptr;
		CGameTeams &Teams = pGame->m_pController->Teams();
		aRec[REC_TEAM] = Teams.m_Core.Team(ClientId);
		int Flags = 0;
		if(Teams.m_aTeeStarted[ClientId])
			Flags |= REC_FLAG_TEE_STARTED;
		if(Teams.m_aTeeFinished[ClientId])
			Flags |= REC_FLAG_TEE_FINISHED;
		if(pChr)
		{
			const CCharacterCore &Core = pChr->m_Core;
			aRec[REC_ALIVE] = 1;
			aRec[REC_POS_X] = Bits(Core.m_Pos.x);
			aRec[REC_POS_Y] = Bits(Core.m_Pos.y);
			aRec[REC_VEL_X] = Bits(Core.m_Vel.x);
			aRec[REC_VEL_Y] = Bits(Core.m_Vel.y);
			aRec[REC_HOOK_POS_X] = Bits(Core.m_HookPos.x);
			aRec[REC_HOOK_POS_Y] = Bits(Core.m_HookPos.y);
			aRec[REC_HOOK_DIR_X] = Bits(Core.m_HookDir.x);
			aRec[REC_HOOK_DIR_Y] = Bits(Core.m_HookDir.y);
			aRec[REC_HOOK_TELE_BASE_X] = Bits(Core.m_HookTeleBase.x);
			aRec[REC_HOOK_TELE_BASE_Y] = Bits(Core.m_HookTeleBase.y);
			aRec[REC_HOOK_TICK] = Core.m_HookTick;
			aRec[REC_HOOK_STATE] = Core.m_HookState;
			aRec[REC_HOOKED_PLAYER] = Core.m_HookedPlayer;
			aRec[REC_NEW_HOOK] = Core.m_NewHook;
			aRec[REC_ACTIVE_WEAPON] = Core.m_ActiveWeapon;
			aRec[REC_JUMPED] = Core.m_Jumped;
			aRec[REC_JUMPED_TOTAL] = Core.m_JumpedTotal;
			aRec[REC_JUMPS] = Core.m_Jumps;
			aRec[REC_DIRECTION] = Core.m_Direction;
			aRec[REC_ANGLE] = Core.m_Angle;
			aRec[REC_COLLIDING] = Core.m_Colliding;
			aRec[REC_LEFT_WALL] = Core.m_LeftWall;
			if(Core.m_Solo)
				Flags |= REC_FLAG_SOLO;
			if(Core.m_Jetpack)
				Flags |= REC_FLAG_JETPACK;
			if(Core.m_CollisionDisabled)
				Flags |= REC_FLAG_COLLISION_DISABLED;
			if(Core.m_EndlessHook)
				Flags |= REC_FLAG_ENDLESS_HOOK;
			if(Core.m_EndlessJump)
				Flags |= REC_FLAG_ENDLESS_JUMP;
			if(Core.m_HammerHitDisabled)
				Flags |= REC_FLAG_HAMMER_HIT_DISABLED;
			if(Core.m_GrenadeHitDisabled)
				Flags |= REC_FLAG_GRENADE_HIT_DISABLED;
			if(Core.m_LaserHitDisabled)
				Flags |= REC_FLAG_LASER_HIT_DISABLED;
			if(Core.m_ShotgunHitDisabled)
				Flags |= REC_FLAG_SHOTGUN_HIT_DISABLED;
			if(Core.m_HookHitDisabled)
				Flags |= REC_FLAG_HOOK_HIT_DISABLED;
			
			
			if(Core.m_HasTelegunGun)
				Flags |= REC_FLAG_TELEGUN_GUN;
			if(Core.m_HasTelegunGrenade)
				Flags |= REC_FLAG_TELEGUN_GRENADE;
			if(Core.m_HasTelegunLaser)
				Flags |= REC_FLAG_TELEGUN_LASER;
			if(Core.m_IsInFreeze)
				Flags |= REC_FLAG_IN_FREEZE;
			if(Core.m_DeepFrozen)
				Flags |= REC_FLAG_DEEP_FROZEN;
			if(Core.m_LiveFrozen)
				Flags |= REC_FLAG_LIVE_FROZEN;
			if(pChr->m_FrozenLastTick)
				Flags |= REC_FLAG_FROZEN_LAST_TICK;
			aRec[REC_FREEZE_START] = Core.m_FreezeStart;
			aRec[REC_FREEZE_TIME] = pChr->m_FreezeTime;
			aRec[REC_RELOAD_TIMER] = pChr->m_ReloadTimer;
			for(int i = 0; i < NUM_WEAPONS; i++)
			{
				if(Core.m_aWeapons[i].m_Got)
					aRec[REC_WEAPONS_GOT] |= 1 << i;
				aRec[REC_AMMO_0 + i] = Core.m_aWeapons[i].m_Ammo;
			}
			aRec[REC_NINJA_DIR_X] = Bits(Core.m_Ninja.m_ActivationDir.x);
			aRec[REC_NINJA_DIR_Y] = Bits(Core.m_Ninja.m_ActivationDir.y);
			aRec[REC_NINJA_ACTIVATION_TICK] = Core.m_Ninja.m_ActivationTick;
			aRec[REC_NINJA_MOVE_TIME] = Core.m_Ninja.m_CurrentMoveTime;
			aRec[REC_NINJA_OLD_VEL] = Core.m_Ninja.m_OldVelAmount;
			aRec[REC_LAST_WEAPON] = pChr->m_LastWeapon;
			aRec[REC_QUEUED_WEAPON] = pChr->m_QueuedWeapon;
			aRec[REC_ATTACK_TICK] = pChr->m_AttackTick;
			aRec[REC_MOVE_RESTRICTIONS] = pChr->m_MoveRestrictions;
			aRec[REC_ARMOR] = pChr->m_Armor;
			aRec[REC_HEALTH] = pChr->m_Health;
			aRec[REC_RACE_STATE] = (int)pChr->m_DDRaceState;
			aRec[REC_START_TIME] = pChr->m_StartTime;
			aRec[REC_TELE_CHECKPOINT] = pChr->m_TeleCheckpoint;
			aRec[REC_TUNE_ZONE] = pChr->m_TuneZone;
			aRec[REC_STRONG_WEAK_ID] = pChr->m_StrongWeakId;
		}
		aRec[REC_FLAGS] = Flags;
		fwrite(aRec, sizeof(aRec), 1, m_pDump);
	}

	void Dump(CGameContext *pGame, int Tick) const
	{
		Put(Tick);
		for(int i = 0; i < m_NumPlayers; i++)
			DumpCharacter(pGame, i);

		const int aTypes[] = {CGameWorld::ENTTYPE_PROJECTILE, CGameWorld::ENTTYPE_LASER, CGameWorld::ENTTYPE_PICKUP};
		for(int Type : aTypes)
		{
			// Doors never tick or move; ddnet_physics bakes them into the collision
			// instead of keeping them as entities, so they are left out here.
			int Count = 0;
			for(CEntity *pEnt = pGame->m_World.FindFirst(Type); pEnt; pEnt = pEnt->TypeNext())
				if(!dynamic_cast<CDoor *>(pEnt))
					Count++;
			Put(Count);
			for(CEntity *pEnt = pGame->m_World.FindFirst(Type); pEnt; pEnt = pEnt->TypeNext())
			{
				if(dynamic_cast<CDoor *>(pEnt))
					continue;
				Put(Bits(pEnt->m_Pos.x));
				Put(Bits(pEnt->m_Pos.y));
			}
		}

		const std::vector<SSwitchers> &vSwitchers = pGame->Switchers();
		Put((int)vSwitchers.size());
		for(const SSwitchers &Switcher : vSwitchers)
			for(int Team = 0; Team < m_NumPlayers; Team++)
				Put(Switcher.m_aStatus[Team]);
	}
};

COracle gs_Oracle;

} // namespace

// True while the oracle wants the server to run another tick right now.
bool CServer::OracleActive()
{
	if(!gs_Oracle.m_Checked)
		gs_Oracle.Init();
	return gs_Oracle.m_Active;
}

// Called before the inputs of the next tick are applied.
void CServer::OraclePreTick()
{
	if(!gs_Oracle.m_Active)
		return;
	CGameContext *pGame = static_cast<CGameContext *>(GameServer());

	// Connect the scripted players as debug dummies with the client ids 0..n-1.
	g_OracleTeleOut = gs_Oracle.Step(0).tele_out;
	g_Config.m_DbgDummies = gs_Oracle.m_NumPlayers;
	UpdateDebugDummies(false);

	for(int i = 0; i < gs_Oracle.m_NumPlayers; i++)
	{
		const oracle_step_t &Step = gs_Oracle.Step(i);
		CPlayer *pPlayer = pGame->m_apPlayers[i];
		if(Step.action == ORACLE_ACTION_KILL)
		{
			if(pPlayer->GetCharacter() && !pPlayer->IsPaused())
			{
				pPlayer->KillCharacter(WEAPON_SELF);
				pPlayer->Respawn();
			}
		}
		else if(Step.action == ORACLE_ACTION_SET_TEAM)
		{
			pGame->m_pController->Teams().SetForceCharacterTeam(i, Step.arg);
		}
		else if(Step.action == ORACLE_ACTION_LOCK_TEAM)
		{
			CGameTeams &Teams = pGame->m_pController->Teams();
			Teams.SetTeamLock(Teams.m_Core.Team(i), Step.arg != 0);
		}
		else if(Step.action == ORACLE_ACTION_TELEPORT)
		{
			if(pPlayer->GetCharacter())
				pPlayer->GetCharacter()->SetPosition(vec2((Step.arg & 0xffff) * 32.0f + 16.0f, (Step.arg >> 16) * 32.0f + 16.0f));
		}

		// The /spec chat command (CGameContext::ConToggleSpec), whenever the wanted state differs.
		if((Step.spec != 0) != (pPlayer->IsPaused() != 0))
		{
			const int PauseType = g_Config.m_SvPauseable ? CPlayer::PAUSE_SPEC : CPlayer::PAUSE_PAUSED;
			pPlayer->Pause(pPlayer->IsPaused() ? CPlayer::PAUSE_NONE : PauseType, false);
		}

		CClient &Client = m_aClients[i];
		Client.m_aInputs[0].m_GameTick = Tick() + 1;
		mem_copy(Client.m_aInputs[0].m_aData, Step.input, sizeof(Step.input));
		Client.m_LatestInput = Client.m_aInputs[0];
		Client.m_CurrentInput = 0;
	}
}

// Called after the game ticked.
void CServer::OraclePostTick()
{
	if(!gs_Oracle.m_Active)
		return;
	gs_Oracle.Dump(static_cast<CGameContext *>(GameServer()), Tick());
	gs_Oracle.m_TicksDone++;
	if(gs_Oracle.m_TicksDone == gs_Oracle.m_NumTicks)
	{
		fclose(gs_Oracle.m_pDump);
		fprintf(stderr, "oracle: done, %d ticks\n", gs_Oracle.m_NumTicks);
		fflush(nullptr);
		_exit(0);
	}
}
