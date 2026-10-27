#!/usr/bin/env python3
"""Generate a random input script for the Unique oracle (see record.h).

Usage: gen_script.py <out> <players> <ticks> <seed> [--kills] [--sides] [--teleports <file>]

The inputs are not uniformly random: keys are held for a while, the aim moves
in steps (and often at the ground, for grenade jumps) and the fire counter
follows the real press/release protocol, so that tees actually travel through
the map, pick up grenades and fly with them.
"""
import argparse
import random
import struct

MAGIC = 0x524F5255
VERSION = 1
ACTION_NONE, ACTION_KILL, ACTION_TELEPORT, ACTION_SIDE = 0, 1, 2, 3


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
            if kind < 0.05:
                self.target = (0, 0)  # aiming at the center is not allowed and gets fixed up
            elif kind < 0.45:
                # at the ground, for grenade jumps
                self.target = (rng.randint(-120, 120), rng.randint(40, 300))
            elif kind < 0.75:
                self.target = (rng.randint(-400, 400), rng.randint(-400, 400))
            else:
                self.target = (rng.choice([-1, 0, 1]) * rng.randint(0, 200), rng.choice([-1, 1]) * rng.randint(1, 300))
        if self.due("fire", 1, 20):
            # every press and every release increments the counter
            self.fire = (self.fire + rng.choice([1, 1, 1, 2, 3])) & 0x3F
        if self.due("weapon", 20, 300):
            # mostly the grenade, sometimes the hammer and the gun, sometimes nothing there
            self.wanted_weapon = rng.choice([0, 0, 1, 2, 4, 4, 4, 3, 6])
        if self.due("scroll", 40, 400):
            if rng.random() < 0.5:
                self.next_weapon = (self.next_weapon + 2) & 0x3F
            else:
                self.prev_weapon = (self.prev_weapon + 2) & 0x3F
        if self.due("flags", 100, 800):
            # mostly playing; sometimes chatting (4) or in the spectator camera (32), which block the input
            self.flags = rng.choice([1, 1, 1, 1, 1, 0, 4, 32, 4 | 32, 8])
        return [self.direction, self.target[0], self.target[1], self.jump, self.fire, self.hook, self.flags,
                self.wanted_weapon, self.next_weapon, self.prev_weapon]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("players", type=int)
    parser.add_argument("ticks", type=int)
    parser.add_argument("seed", type=int)
    parser.add_argument("--kills", action="store_true", help="players sometimes use the kill bind")
    parser.add_argument("--sides", action="store_true", help="players sometimes use /red and /blue (fastcap)")
    parser.add_argument("--teleports", help="file with 'x y' tile positions (from map_points) that tees get "
                        "teleported to, to reach all parts of a map")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    points = []
    if args.teleports:
        with open(args.teleports) as f:
            points = [tuple(map(int, line.split())) for line in f if line.strip()]
    players = [Player(rng) for _ in range(args.players)]
    with open(args.out, "wb") as f:
        f.write(struct.pack("<4i", MAGIC, VERSION, args.players, args.ticks))
        for tick in range(args.ticks):
            # one value per tick: what the server would have drawn from its random number generator
            tele_out = rng.getrandbits(31)
            for player in players:
                action, arg = ACTION_NONE, 0
                if args.kills and tick > 10 and rng.random() < 0.004:
                    action = ACTION_KILL
                elif args.sides and tick > 10 and rng.random() < 0.002:
                    action, arg = ACTION_SIDE, rng.choice([1, 2])
                elif points and tick > 10 and rng.random() < 0.008:
                    x, y = rng.choice(points)
                    action, arg = ACTION_TELEPORT, x | y << 16
                f.write(struct.pack("<13i", action, arg, tele_out, *player.step()))


if __name__ == "__main__":
    main()
