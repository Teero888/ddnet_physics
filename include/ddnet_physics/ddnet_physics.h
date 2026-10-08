/* ddnet_physics: the raw physics of DDNet (https://ddnet.org), bit-identical
 * to the game server, as a C library.
 *
 *   map_data_t map = load_map("maps/Tutorial.map");
 *   ddnet_collision_t collision;
 *   ddnet_collision_init(&collision, &map);
 *   free_map_data(&map);
 *
 *   ddnet_config_t config = ddnet_config_default(DDNET_MODE_DDRACE);
 *   ddnet_world_t world;
 *   ddnet_world_init(&world, &collision, &config);
 *   ddnet_player_join(&world, 0);
 *
 *   for (int i = 0; i < 100; ++i) {
 *     world.players[0].input.direction = 1;
 *     ddnet_world_tick(&world);
 *   }
 *
 *   ddnet_world_free(&world);
 *   ddnet_collision_free(&collision);
 */
#ifndef DDNET_PHYSICS_H
#define DDNET_PHYSICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "backend.h"
#include "collision.h"
#include "config.h"
#include "events.h"
#include "tuning.h"
#include "types.h"
#include "vmath.h"
#include "world.h"

#ifdef __cplusplus
}
#endif

#endif
