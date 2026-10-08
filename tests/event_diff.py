#!/usr/bin/env python3
"""Compare two event logs of tests/replay (REPLAY_EVENTS): the same events on every tick, in any order
within a tick (the optimized backend has some of them in another order, see events.h).

Usage: event_diff.py [--from tick] <expected> <actual>

With --from, only the ticks from that one on (for a world that the event build ticks from that one on)."""
import collections
import struct
import sys

KINDS = ("sound", "particle", "damage indicator")


def load(path):
    ticks = collections.defaultdict(list)
    with open(path, "rb") as f:
        data = f.read()
    for rec in struct.iter_unpack("<7i", data):
        ticks[rec[0]].append(rec[1:])
    return ticks


def describe(rec):
    kind, ident, client, x, y, angle = rec
    fx, fy, fa = (struct.unpack("<f", struct.pack("<i", v))[0] for v in (x, y, angle))
    extra = " angle %g" % fa if kind == 2 else ""
    return "%s %d client %d at %g %g%s" % (KINDS[kind], ident, client, fx, fy, extra)


def main():
    args = sys.argv[1:]
    start = 0
    if args[0] == "--from":
        start = int(args[1])
        args = args[2:]
    expected, actual = load(args[0]), load(args[1])
    total = sum(len(v) for t, v in expected.items() if t >= start)
    for tick in sorted(set(expected) | set(actual)):
        if tick < start:
            continue
        a, b = sorted(expected.get(tick, [])), sorted(actual.get(tick, []))
        if a != b:
            missing = collections.Counter(a) - collections.Counter(b)
            extra = collections.Counter(b) - collections.Counter(a)
            lines = ["tick %d: events differ" % tick]
            lines += ["  missing %s" % describe(r) for r in missing.elements()][:3]
            lines += ["  extra %s" % describe(r) for r in extra.elements()][:3]
            print("\n".join(lines))
            return 1
    print("identical: %d events" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
