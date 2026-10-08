#!/usr/bin/env python3
"""Generate a random input script for the oracle (see record.h).

Usage: gen_script.py <out> <players> <ticks> <seed> [--kills] [--spec] [--teams] [--teleports <file>]

The inputs are not uniformly random: keys are held for a while, the aim moves
in steps and the fire counter follows the real press/release protocol, so that
tees actually travel through the map, hook each other and use their weapons.
"""
import argparse
import random
import struct

MAGIC = 0x524F4444
VERSION = 3
ACTION_NONE, ACTION_KILL, ACTION_TELEPORT, ACTION_SET_TEAM, ACTION_LOCK_TEAM = 0, 1, 2, 3, 4


class Player:
    def __init__(self, rng):
        self.rng = rng
        self.direction = 0
        self.jump = 0
        self.hook = 0
        self.fire = 0
        self.target = (1, 0)
        self.wanted_weapon = 0
        self.next_weapon = 0
        self.prev_weapon = 0
        self.flags = 0
        self.spec = False
        self.timers = {}

    def due(self, key, low, high):
        """True once every random(low..high) ticks."""
        self.timers[key] = self.timers.get(key, 0) - 1
        if self.timers[key] > 0:
            return False
        self.timers[key] = self.rng.randint(low, high)
        return True

    def step(self):
        rng = self.rng
        if self.due("direction", 3, 80):
            self.direction = rng.choice([-1, -1, 0, 1, 1, 1])
        if self.due("jump", 1, 40):
            self.jump = 0 if self.jump else rng.choice([0, 1, 1])
        if self.due("hook", 2, 60):
            self.hook = 0 if self.hook else rng.choice([0, 1, 1])
        if self.due("aim", 1, 30):
            kind = rng.random()
            if kind < 0.1:
                self.target = (0, 0)  # aiming at the center is not allowed and gets fixed up
            elif kind < 0.5:
                self.target = (rng.randint(-400, 400), rng.randint(-400, 400))
            else:
                self.target = (rng.choice([-1, 0, 1]) * rng.randint(0, 200), rng.choice([-1, 1]) * rng.randint(1, 300))
        if self.due("fire", 1, 25):
            # every press and every release increments the counter
            self.fire = (self.fire + rng.choice([1, 1, 1, 2, 3])) & 0x3F
        if self.due("weapon", 20, 200):
            self.wanted_weapon = rng.choice([0, 0, 1, 2, 3, 4, 5, 6])
        if self.due("scroll", 30, 300):
            if rng.random() < 0.5:
                self.next_weapon = (self.next_weapon + 2) & 0x3F
            else:
                self.prev_weapon = (self.prev_weapon + 2) & 0x3F
        if self.due("flags", 100, 600):
            # mostly playing; sometimes chatting (4) or in the spectator camera (32), which block the input
            self.flags = rng.choice([0, 0, 0, 0, 1, 4, 32, 4 | 32, 8])
        return [self.direction, self.target[0], self.target[1], self.jump, self.fire, self.hook, self.flags,
                self.wanted_weapon, self.next_weapon, self.prev_weapon]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("players", type=int)
    parser.add_argument("ticks", type=int)
    parser.add_argument("seed", type=int)
    parser.add_argument("--kills", action="store_true", help="players sometimes kill themselves")
    parser.add_argument("--spec", action="store_true", help="players sometimes use /spec")
    parser.add_argument("--teams", action="store_true", help="players join, change and lock teams")
    parser.add_argument("--teleports", help="file with 'x y' tile positions (from map_points) that tees get "
                        "teleported to, to reach all parts of a map")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    points = []
    if args.teleports:
        with open(args.teleports) as f:
            points = [tuple(map(int, line.split())) for line in f if line.strip()]
    players = [Player(rng) for _ in range(args.players)]
    recent = [points[0]] if points else []
    with open(args.out, "wb") as f:
        f.write(struct.pack("<4i", MAGIC, VERSION, args.players, args.ticks))
        for tick in range(args.ticks):
            # one value per tick: what the server would have drawn from its random number generator
            tele_out = rng.getrandbits(31)
            for player in players:
                action, arg = ACTION_NONE, 0
                if args.kills and tick > 10 and rng.random() < 0.002:
                    action = ACTION_KILL
                elif args.teams and tick > 10 and rng.random() < 0.003:
                    action, arg = ACTION_SET_TEAM, rng.randint(0, 2)
                elif args.teams and tick > 10 and rng.random() < 0.002:
                    action, arg = ACTION_LOCK_TEAM, rng.randint(0, 1)
                elif points and tick > 10 and rng.random() < 0.01:
                    if rng.random() < 0.3 and len(players) > 1:
                        # close to where somebody else was sent, so that tees meet
                        x, y = rng.choice(recent)
                        x, y = max(0, x + rng.randint(-2, 2)), max(0, y + rng.randint(-2, 2))
                    else:
                        x, y = rng.choice(points)
                    recent.append((x, y))
                    del recent[:-8]
                    action, arg = ACTION_TELEPORT, x | y << 16
                if args.spec and rng.random() < 0.004:
                    player.spec = not player.spec
                f.write(struct.pack("<14i", action, arg, int(player.spec), tele_out, *player.step()))


if __name__ == "__main__":
    main()
