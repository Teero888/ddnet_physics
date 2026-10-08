#!/usr/bin/env python3
"""Patch a DDNet checkout so that DDNet-Server can act as the test oracle.

Usage: patch_ddnet.py <ddnet checkout>

Every edit is an exact, asserted text replacement so that an upstream change
that moves the hook points fails loudly instead of silently producing a
different oracle.
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
here = pathlib.Path(__file__).resolve().parent


def replace(path, old, new, count=1):
    file = root / path
    text = file.read_text()
    assert text.count(old) == count, f"{path}: expected {count}x {old!r}, found {text.count(old)}"
    file.write_text(text.replace(old, new))


# The oracle itself is copied in by build.sh, so that it can be changed
# without patching again.

# Hooks in the server main loop.
replace("src/engine/server/server.h",
        "\tvoid UpdateDebugDummies(bool ForceDisconnect);",
        "\tvoid UpdateDebugDummies(bool ForceDisconnect);\n"
        "\tbool OracleActive();\n\tvoid OraclePreTick();\n\tvoid OraclePostTick();")
replace("src/engine/server/server.cpp",
        "\t\t\twhile(LastTime > TickStartTime(m_CurrentGameTick + 1))\n"
        "\t\t\t{\n"
        "\t\t\t\tGameServer()->OnPreTickTeehistorian();\n"
        "\t\t\t\tUpdateDebugDummies(false);\n",
        "\t\t\twhile(OracleActive() || LastTime > TickStartTime(m_CurrentGameTick + 1))\n"
        "\t\t\t{\n"
        "\t\t\t\tGameServer()->OnPreTickTeehistorian();\n"
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

# The oracle dumps private state.
with (root / "CMakeLists.txt").open("a") as f:
    f.write("\nset_source_files_properties(src/engine/server/server.cpp PROPERTIES COMPILE_OPTIONS -fno-access-control)\n")

print("patched", root)
