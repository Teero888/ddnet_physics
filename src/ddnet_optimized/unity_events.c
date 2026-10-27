/* The event build of the optimized backend: the same code with DDNET_PHYSICS_EVENTS, which calls the
 * callbacks of events.h, and with names of its own for every function (event_names.h: the functions of the
 * API become ddnet_ev_*), so that it is in the library next to the build without events. Both use the same
 * structures: a world can be ticked by either. */
#define DDNET_PHYSICS_EVENTS 1
#include "event_names.h"

#include "unity.c"
