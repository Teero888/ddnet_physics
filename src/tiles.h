#ifndef TILES_H
#define TILES_H

#include <ddnet_physics/gamecore.h>

#define MAX_TILES 256
typedef void (*tile_handler_f)(SCharacterCore *pCore);
typedef void (*switch_handler_f)(SCharacterCore *pCore, unsigned char ucNumber, unsigned char ucDelay);
extern tile_handler_f g_apGameTileHandlers[MAX_TILES];
extern switch_handler_f g_apSwitchTileHandlers[MAX_TILES];

void cc_init_tile_handlers(void);

#endif // TILES_H
