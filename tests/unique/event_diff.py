#!/usr/bin/env python3
"""Compare two event logs of tests/unique/replay (REPLAY_EVENTS): the same events for every player on every
tick, in any order within a tick (the optimized backend has some of them in another order, see events.h).

Usage: event_diff.py <expected> <actual>"""
import collections
import struct
import sys

KINDS = ("sound", "particle", "damage indicator")


def load(path):
    ticks = collections.defaultdict(list)
    with open(path, "rb") as f:
        data = f.read()
    for rec in struct.iter_unpack("<7i", data):
        ticks[(rec[1], rec[0])].append(rec[2:])
    return ticks


def describe(rec):
    kind, ident, x, y, angle = rec
    fx, fy, fa = (struct.unpack("<f", struct.pack("<i", v))[0] for v in (x, y, angle))
    extra = " angle %g" % fa if kind == 2 else ""
    return "%s %d at %g %g%s" % (KINDS[kind], ident, fx, fy, extra)


def main():
    expected, actual = load(sys.argv[1]), load(sys.argv[2])
    total = sum(len(v) for v in expected.values())
    for key in sorted(set(expected) | set(actual)):
        a, b = sorted(expected.get(key, [])), sorted(actual.get(key, []))
        if a != b:
            missing = collections.Counter(a) - collections.Counter(b)
            extra = collections.Counter(b) - collections.Counter(a)
            lines = ["tick %d player %d: events differ" % key]
            lines += ["  missing %s" % describe(r) for r in missing.elements()][:3]
            lines += ["  extra %s" % describe(r) for r in extra.elements()][:3]
            print("\n".join(lines))
            return 1
    print("identical: %d events" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
