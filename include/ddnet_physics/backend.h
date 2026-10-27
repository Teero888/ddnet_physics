/* Which implementation the library was built as.
 *
 * The library simulates one of two games:
 *
 *   ddnet   DDNet (https://ddnet.org), with everything a DDNet server has.
 *   unique  The Unique Race mod of DDNet (https://github.com/unique-clan/unique-race):
 *           race and fastcap, solo, with the grenade as the only weapon that
 *           does anything. A world holds one player, and only what the
 *           mapping rules of Unique Race allow exists in it.
 *
 * Each has two implementations: a reference implementation, a readable port
 * of the game server, and an optimized implementation with the same results.
 * They do not share their structures, so the choice is made when the library
 * is compiled (-DDDNET_PHYSICS_BACKEND=ddnet_reference|ddnet_optimized|
 * unique_reference|unique_optimized for CMake) and everything that includes
 * these headers has to be compiled for the same one. The CMake target takes
 * care of that by exporting one of:
 *
 *   DDNET_PHYSICS_BACKEND_DDNET_REFERENCE
 *   DDNET_PHYSICS_BACKEND_DDNET_OPTIMIZED
 *   DDNET_PHYSICS_BACKEND_UNIQUE_REFERENCE
 *   DDNET_PHYSICS_BACKEND_UNIQUE_OPTIMIZED
 *
 * From that, this header defines DDNET_PHYSICS_GAME_DDNET or
 * DDNET_PHYSICS_GAME_UNIQUE for the game. */
#ifndef DDNET_PHYSICS_BACKEND_H
#define DDNET_PHYSICS_BACKEND_H

#if (defined(DDNET_PHYSICS_BACKEND_DDNET_REFERENCE) + defined(DDNET_PHYSICS_BACKEND_DDNET_OPTIMIZED) +           \
     defined(DDNET_PHYSICS_BACKEND_UNIQUE_REFERENCE) + defined(DDNET_PHYSICS_BACKEND_UNIQUE_OPTIMIZED)) > 1
#error "ddnet_physics: more than one backend is selected"
#endif

#if defined(DDNET_PHYSICS_BACKEND_DDNET_REFERENCE) || defined(DDNET_PHYSICS_BACKEND_DDNET_OPTIMIZED)
#define DDNET_PHYSICS_GAME_DDNET
#elif defined(DDNET_PHYSICS_BACKEND_UNIQUE_REFERENCE) || defined(DDNET_PHYSICS_BACKEND_UNIQUE_OPTIMIZED)
#define DDNET_PHYSICS_GAME_UNIQUE
#else
#error "ddnet_physics: define one of DDNET_PHYSICS_BACKEND_{DDNET,UNIQUE}_{REFERENCE,OPTIMIZED} (the CMake target does)"
#endif

#endif
