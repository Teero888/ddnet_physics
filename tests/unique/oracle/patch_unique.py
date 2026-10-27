#!/usr/bin/env python3
"""Patch a Unique Race checkout so that its server can act as the test oracle
of the Unique backends.

Usage: patch_unique.py <unique-race checkout>

Every edit is an exact, asserted text replacement so that an upstream change
that moves the hook points fails loudly instead of silently producing a
different oracle.
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])


def replace(path, old, new, count=1):
    file = root / path
    text = file.read_text()
    assert text.count(old) == count, f"{path}: expected {count}x {old!r}, found {text.count(old)}"
    file.write_text(text.replace(old, new))


# The oracle itself is copied in by build.sh, so that it can be changed
# without patching again.

# The debug dummies, which the oracle connects as its players, only exist in
# debug builds of this version of the server.
replace("src/engine/server/server.h", "#ifdef CONF_DEBUG", "#if 1")
replace("src/engine/server/server.cpp", "#ifdef CONF_DEBUG", "#if 1", count=5)
replace("src/engine/shared/config_variables.h",
        "#ifdef CONF_DEBUG\nMACRO_CONFIG_INT(DbgDummies,",
        "#if 1\nMACRO_CONFIG_INT(DbgDummies,")

# Hooks in the server main loop.
replace("src/engine/server/server.h",
        "\tvoid UpdateDebugDummies(bool ForceDisconnect);",
        "\tvoid UpdateDebugDummies(bool ForceDisconnect);\n"
        "\tbool OracleActive();\n\tvoid OraclePreTick();\n\tvoid OraclePostTick();")
replace("src/engine/server/server.cpp",
        "\t\t\twhile(LastTime > TickStartTime(m_CurrentGameTick + 1))\n"
        "\t\t\t{\n"
        "\t\t\t\tGameServer()->OnPreTickTeehistorian();\n"
        "\n"
        "#if 1\n"
        "\t\t\t\tUpdateDebugDummies(false);\n"
        "#endif\n",
        "\t\t\twhile(OracleActive() || LastTime > TickStartTime(m_CurrentGameTick + 1))\n"
        "\t\t\t{\n"
        "\t\t\t\tGameServer()->OnPreTickTeehistorian();\n"
        "\n"
        "\t\t\t\tUpdateDebugDummies(false);\n"
        "\t\t\t\tOraclePreTick();\n")
replace("src/engine/server/server.cpp",
        "\t\t\t\tGameServer()->OnTick();\n"
        "\t\t\t\tif(ErrorShutdown())\n",
        "\t\t\t\tGameServer()->OnTick();\n"
        "\t\t\t\tOraclePostTick();\n"
        "\t\t\t\tif(ErrorShutdown())\n")
# Scripted players get the client ids 0..n-1 instead of counting down from the top.
replace("src/engine/server/server.cpp",
        "\t\tconst int ClientId = MaxClients() - DummyIndex - 1;",
        "\t\tconst int ClientId = OracleActive() ? DummyIndex : MaxClients() - DummyIndex - 1;")
with (root / "src/engine/server/server.cpp").open("a") as f:
    f.write("\n#include <oracle/oracle.h>\n")

# No randomness: the exit of a teleporter with several exits is chosen by the
# script instead of the PRNG, the same way ddnet_physics lets its caller choose.
replace("src/game/gamecore.h",
        "\t\tif(BelowThis <= 1 || !m_pPrng)\n\t\t{\n\t\t\treturn 0;\n\t\t}\n",
        "\t\tif(BelowThis <= 1 || !m_pPrng)\n\t\t{\n\t\t\treturn 0;\n\t\t}\n"
        "\t\textern int g_OracleTeleOut;\n"
        "\t\treturn (int)((unsigned)g_OracleTeleOut % (unsigned)BelowThis);\n")

# The finish time, which the server only prints and sends to the database.
replace("src/game/server/teams.cpp",
        "\t\tTime = (FractionOfTick - GetStartTimeOffset(pPlayer) + TimeTicks) / (float)Server()->TickSpeed();\n",
        "\t\tTime = (FractionOfTick - GetStartTimeOffset(pPlayer) + TimeTicks) / (float)Server()->TickSpeed();\n"
        "\t{\n"
        "\t\textern int g_aOracleFinishTick[];\n"
        "\t\textern float g_aOracleFinishTime[];\n"
        "\t\tg_aOracleFinishTick[pPlayer->GetCid()] = Server()->Tick();\n"
        "\t\tg_aOracleFinishTime[pPlayer->GetCid()] = Time;\n"
        "\t}\n")

# The oracle dumps private state.
with (root / "CMakeLists.txt").open("a") as f:
    f.write("\nset_source_files_properties(src/engine/server/server.cpp PROPERTIES COMPILE_OPTIONS -fno-access-control)\n")

print("patched", root)
