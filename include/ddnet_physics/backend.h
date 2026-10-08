/* Which implementation the library was built as.
 *
 * There are two implementations of the same physics with the same functions:
 * the reference implementation, a readable port of DDNet, and the optimized
 * implementation. They do not share their structures, so the choice is made
 * when the library is compiled (-DDDNET_PHYSICS_BACKEND=reference|optimized
 * for CMake) and everything that includes these headers has to be compiled
 * for the same one. The CMake target takes care of that by exporting one of:
 *
 *   DDNET_PHYSICS_BACKEND_REFERENCE
 *   DDNET_PHYSICS_BACKEND_OPTIMIZED */
#ifndef DDNET_PHYSICS_BACKEND_H
#define DDNET_PHYSICS_BACKEND_H

#if defined(DDNET_PHYSICS_BACKEND_REFERENCE) && defined(DDNET_PHYSICS_BACKEND_OPTIMIZED)
#error "ddnet_physics: both backends are selected"
#elif !defined(DDNET_PHYSICS_BACKEND_REFERENCE) && !defined(DDNET_PHYSICS_BACKEND_OPTIMIZED)
#error                                                                                                       \
    "ddnet_physics: define DDNET_PHYSICS_BACKEND_REFERENCE or DDNET_PHYSICS_BACKEND_OPTIMIZED (the CMake target does)"
#endif

#endif
