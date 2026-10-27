// Test oracle for the Unique backends of ddnet_physics, compiled into a
// patched Unique Race server (see patch_unique.py).
//
// When DDNET_ORACLE_SCRIPT and DDNET_ORACLE_DUMP are set, the server stops
// following the wall clock: it connects the scripted players, feeds them the
// scripted inputs tick by tick, writes the full physics state after every tick
// and exits when the script ends. See record.h for the formats.
//
// This file is included at the end of src/engine/server/server.cpp, which is
// compiled with -fno-access-control so that private state can be dumped.
#include "oracle_record.h"

#include <game/server/entities/character.h>
#include <game/server/entities/door.h>
#include <game/server/entities/pickup.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>
#include <game/server/teams.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// What CWorldCore::RandomOr0 is replaced with, see patch_unique.py.
int g_OracleTeleOut = 0;
// Written by CGameTeams::OnFinish, see patch_unique.py.
int g_aOracleFinishTick[MAX_CLIENTS];
float g_aOracleFinishTime[MAX_CLIENTS];

namespace {

struct COracle
{
	bool m_Checked = false;
	bool m_Active = false;
	int m_NumPlayers = 0;
	int m_NumTicks = 0;
	int m_TicksDone = 0;
	std::vector<unique_step_t> m_vSteps;
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
		if(!pFile || fread(aHeader, sizeof(aHeader), 1, pFile) != 1 || aHeader[0] != UNIQUE_SCRIPT_MAGIC || aHeader[1] != UNIQUE_VERSION)
		{
			fprintf(stderr, "oracle: cannot read script '%s'\n", pScript);
			exit(2);
		}
		m_NumPlayers = aHeader[2];
		m_NumTicks = aHeader[3];
		m_vSteps.resize((size_t)m_NumPlayers * m_NumTicks);
		if(fread(m_vSteps.data(), sizeof(unique_step_t), m_vSteps.size(), pFile) != m_vSteps.size())
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
		aHeader[0] = UNIQUE_DUMP_MAGIC;
		fwrite(aHeader, sizeof(aHeader), 1, m_pDump);
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			g_aOracleFinishTick[i] = -1;
			g_aOracleFinishTime[i] = 0.0f;
		}
		m_Active = true;
	}

	const unique_step_t &Step(int Player) const
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
		int32_t aRec[UREC_NUM] = {};
		CPlayer *pPlayer = pGame->m_apPlayers[ClientId];
		CCharacter *pChr = pPlayer ? pPlayer->GetCharacter() : nullptr;
		int Flags = 0;
		if(pChr)
		{
			const CCharacterCore &Core = pChr->m_Core;
			aRec[UREC_ALIVE] = 1;
			aRec[UREC_POS_X] = Bits(Core.m_Pos.x);
			aRec[UREC_POS_Y] = Bits(Core.m_Pos.y);
			aRec[UREC_VEL_X] = Bits(Core.m_Vel.x);
			aRec[UREC_VEL_Y] = Bits(Core.m_Vel.y);
			aRec[UREC_PREV_POS_X] = Bits(pChr->m_PrevPos.x);
			aRec[UREC_PREV_POS_Y] = Bits(pChr->m_PrevPos.y);
			aRec[UREC_HOOK_POS_X] = Bits(Core.m_HookPos.x);
			aRec[UREC_HOOK_POS_Y] = Bits(Core.m_HookPos.y);
			aRec[UREC_HOOK_DIR_X] = Bits(Core.m_HookDir.x);
			aRec[UREC_HOOK_DIR_Y] = Bits(Core.m_HookDir.y);
			aRec[UREC_HOOK_TELE_BASE_X] = Bits(Core.m_HookTeleBase.x);
			aRec[UREC_HOOK_TELE_BASE_Y] = Bits(Core.m_HookTeleBase.y);
			aRec[UREC_HOOK_TICK] = Core.m_HookTick;
			aRec[UREC_HOOK_STATE] = Core.m_HookState;
			aRec[UREC_HOOKED_PLAYER] = Core.m_HookedPlayer;
			aRec[UREC_NEW_HOOK] = Core.m_NewHook;
			aRec[UREC_ACTIVE_WEAPON] = Core.m_ActiveWeapon;
			aRec[UREC_JUMPED] = Core.m_Jumped;
			aRec[UREC_JUMPED_TOTAL] = Core.m_JumpedTotal;
			aRec[UREC_JUMPS] = Core.m_Jumps;
			aRec[UREC_DIRECTION] = Core.m_Direction;
			aRec[UREC_ANGLE] = Core.m_Angle;
			aRec[UREC_COLLIDING] = Core.m_Colliding;
			aRec[UREC_LEFT_WALL] = Core.m_LeftWall;
			if(Core.m_Solo)
				Flags |= UREC_FLAG_SOLO;
			if(Core.m_Jetpack)
				Flags |= UREC_FLAG_JETPACK;
			if(Core.m_CollisionDisabled)
				Flags |= UREC_FLAG_COLLISION_DISABLED;
			if(Core.m_EndlessHook)
				Flags |= UREC_FLAG_ENDLESS_HOOK;
			if(Core.m_EndlessJump)
				Flags |= UREC_FLAG_ENDLESS_JUMP;
			if(Core.m_HammerHitDisabled)
				Flags |= UREC_FLAG_HAMMER_HIT_DISABLED;
			if(Core.m_GrenadeHitDisabled)
				Flags |= UREC_FLAG_GRENADE_HIT_DISABLED;
			if(Core.m_LaserHitDisabled)
				Flags |= UREC_FLAG_LASER_HIT_DISABLED;
			if(Core.m_ShotgunHitDisabled)
				Flags |= UREC_FLAG_SHOTGUN_HIT_DISABLED;
			if(Core.m_HookHitDisabled)
				Flags |= UREC_FLAG_HOOK_HIT_DISABLED;
			if(Core.m_HasTelegunGun)
				Flags |= UREC_FLAG_TELEGUN_GUN;
			if(Core.m_HasTelegunGrenade)
				Flags |= UREC_FLAG_TELEGUN_GRENADE;
			if(Core.m_HasTelegunLaser)
				Flags |= UREC_FLAG_TELEGUN_LASER;
			if(Core.m_IsInFreeze)
				Flags |= UREC_FLAG_IN_FREEZE;
			if(Core.m_DeepFrozen)
				Flags |= UREC_FLAG_DEEP_FROZEN;
			if(Core.m_LiveFrozen)
				Flags |= UREC_FLAG_LIVE_FROZEN;
			if(pChr->m_FrozenLastTick)
				Flags |= UREC_FLAG_FROZEN_LAST_TICK;
			if(pChr->m_aGotFastcapFlag[TEAM_RED])
				Flags |= UREC_FLAG_GOT_RED_FLAG;
			if(pChr->m_aGotFastcapFlag[TEAM_BLUE])
				Flags |= UREC_FLAG_GOT_BLUE_FLAG;
			aRec[UREC_FREEZE_START] = Core.m_FreezeStart;
			aRec[UREC_FREEZE_TIME] = pChr->m_FreezeTime;
			aRec[UREC_RELOAD_TIMER] = pChr->m_ReloadTimer;
			for(int i = 0; i < NUM_WEAPONS; i++)
				if(Core.m_aWeapons[i].m_Got)
					aRec[UREC_WEAPONS_GOT] |= 1 << i;
			aRec[UREC_AMMO_HAMMER] = Core.m_aWeapons[WEAPON_HAMMER].m_Ammo;
			aRec[UREC_AMMO_GUN] = Core.m_aWeapons[WEAPON_GUN].m_Ammo;
			aRec[UREC_AMMO_GRENADE] = Core.m_aWeapons[WEAPON_GRENADE].m_Ammo;
			aRec[UREC_LAST_WEAPON] = pChr->m_LastWeapon;
			aRec[UREC_QUEUED_WEAPON] = pChr->m_QueuedWeapon;
			aRec[UREC_ATTACK_TICK] = pChr->m_AttackTick;
			aRec[UREC_MOVE_RESTRICTIONS] = pChr->m_MoveRestrictions;
			aRec[UREC_ARMOR] = pChr->m_Armor;
			aRec[UREC_HEALTH] = pChr->m_Health;
			aRec[UREC_RACE_STATE] = (int)pChr->m_DDRaceState;
			aRec[UREC_START_TIME] = pChr->m_StartTime;
			aRec[UREC_START_TIME_OFFSET] = Bits(pChr->m_StartTimeOffset);
			aRec[UREC_TIME] = Bits(pChr->m_Time);
			aRec[UREC_TELE_CHECKPOINT] = pChr->m_TeleCheckpoint;
			aRec[UREC_TUNE_ZONE] = pChr->m_TuneZone;
		}
		aRec[UREC_FLAGS] = Flags;
		if(pPlayer)
		{
			aRec[UREC_SPAWNING] = pPlayer->m_Spawning;
			aRec[UREC_DIE_TICK] = pPlayer->m_DieTick;
			aRec[UREC_PREVIOUS_DIE_TICK] = pPlayer->m_PreviousDieTick;
			aRec[UREC_LAST_KILL] = pPlayer->m_LastKill;
			aRec[UREC_FASTCAP_SIDE] = pPlayer->m_FastcapSpawnAt;
		}
		aRec[UREC_FINISH_TICK] = g_aOracleFinishTick[ClientId];
		aRec[UREC_FINISH_TIME] = Bits(g_aOracleFinishTime[ClientId]);
		fwrite(aRec, sizeof(aRec), 1, m_pDump);
	}

	void DumpEntities(CGameContext *pGame, int ClientId) const
	{
		// The bullets of the gun only fly: they hit nothing a solo tee could
		// feel, so the Unique backends do not have them.
		int Count = 0;
		for(CEntity *pEnt = pGame->m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
		{
			CProjectile *pProj = static_cast<CProjectile *>(pEnt);
			if(pProj->m_Owner == ClientId && pProj->m_Type != WEAPON_GUN)
				Count++;
		}
		Put(Count);
		for(CEntity *pEnt = pGame->m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
		{
			CProjectile *pProj = static_cast<CProjectile *>(pEnt);
			if(pProj->m_Owner != ClientId || pProj->m_Type == WEAPON_GUN)
				continue;
			int32_t aRec[UPROJ_NUM];
			aRec[UPROJ_POS_X] = Bits(pProj->m_Pos.x);
			aRec[UPROJ_POS_Y] = Bits(pProj->m_Pos.y);
			aRec[UPROJ_DIR_X] = Bits(pProj->m_Direction.x);
			aRec[UPROJ_DIR_Y] = Bits(pProj->m_Direction.y);
			aRec[UPROJ_START_TICK] = pProj->m_StartTick;
			aRec[UPROJ_LIFE_SPAN] = pProj->m_LifeSpan;
			aRec[UPROJ_TYPE] = pProj->m_Type;
			fwrite(aRec, sizeof(aRec), 1, m_pDump);
		}

		Count = 0;
		for(CEntity *pEnt = pGame->m_World.FindFirst(CGameWorld::ENTTYPE_PICKUP); pEnt; pEnt = pEnt->TypeNext())
			Count++;
		Put(Count);
		for(CEntity *pEnt = pGame->m_World.FindFirst(CGameWorld::ENTTYPE_PICKUP); pEnt; pEnt = pEnt->TypeNext())
		{
			CPickup *pPickup = static_cast<CPickup *>(pEnt);
			int32_t aRec[UPICKUP_NUM];
			aRec[UPICKUP_POS_X] = Bits(pPickup->m_Pos.x);
			aRec[UPICKUP_POS_Y] = Bits(pPickup->m_Pos.y);
			aRec[UPICKUP_TYPE] = pPickup->m_Type;
			aRec[UPICKUP_SUBTYPE] = pPickup->m_Subtype;
			aRec[UPICKUP_SPAWN_TICK] = pPickup->m_aSpawnTick[ClientId];
			fwrite(aRec, sizeof(aRec), 1, m_pDump);
		}

		// Doors are entities in DDNet but part of the collision in
		// ddnet_physics; anything else in the laser list would be a laser.
		for(CEntity *pEnt = pGame->m_World.FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->TypeNext())
		{
			if(!dynamic_cast<CDoor *>(pEnt))
			{
				fprintf(stderr, "oracle: unexpected laser entity at tick %d\n", pGame->Server()->Tick());
				exit(3);
			}
		}

		const int Team = pGame->m_pController->Teams().m_Core.Team(ClientId);
		const std::vector<SSwitchers> &vSwitchers = pGame->Switchers();
		Put((int)vSwitchers.size());
		for(const SSwitchers &Switcher : vSwitchers)
		{
			Put(Switcher.m_aStatus[Team]);
			Put(Switcher.m_aType[Team]);
			Put(Switcher.m_aEndTick[Team]);
		}
	}

	void Dump(CGameContext *pGame, int Tick) const
	{
		Put(Tick);
		for(int i = 0; i < m_NumPlayers; i++)
		{
			DumpCharacter(pGame, i);
			DumpEntities(pGame, i);
		}
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
		const unique_step_t &Step = gs_Oracle.Step(i);
		CPlayer *pPlayer = pGame->m_apPlayers[i];
		if(Step.action == UNIQUE_ACTION_KILL)
		{
			// CGameContext::OnKillNetMessage
			CNetMsg_Cl_Kill Msg;
			pGame->OnKillNetMessage(&Msg, i);
		}
		else if(Step.action == UNIQUE_ACTION_SIDE)
		{
			// The /red and /blue chat commands.
			pGame->Console()->ExecuteLine(Step.arg == 1 ? "red" : "blue", i);
		}
		else if(Step.action == UNIQUE_ACTION_TELEPORT)
		{
			if(pPlayer->GetCharacter())
				pPlayer->GetCharacter()->SetPosition(vec2((Step.arg & 0xffff) * 32.0f + 16.0f, (Step.arg >> 16) * 32.0f + 16.0f));
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
