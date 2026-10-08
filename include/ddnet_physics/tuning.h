#ifndef DDNET_PHYSICS_TUNING_H
#define DDNET_PHYSICS_TUNING_H

#include <stdbool.h>

#define DDNET_TICK_SPEED 50

/* A tuning value is stored like DDNet stores and sends it: as an integer in
 * hundredths. Physics code reads it through ddnet_tune(), every time, exactly
 * like the implicit float conversion of DDNet's CTuneParam. */
typedef int ddnet_tune_param_t;

static inline float ddnet_tune(ddnet_tune_param_t value) { return value / 100.0f; }

typedef struct ddnet_tuning_t {
#define DDNET_TUNING_PARAM(name, default_value) ddnet_tune_param_t name;
#include "tuning_params.h"
#undef DDNET_TUNING_PARAM
} ddnet_tuning_t;

enum { DDNET_NUM_TUNING_PARAMS = sizeof(ddnet_tuning_t) / sizeof(ddnet_tune_param_t) };

/* DDNet's built-in defaults (CTuningParams::DEFAULT). Note that a DDNet server
 * does not run with these: see ddnet_tuning_init_ddrace(). */
void ddnet_tuning_init_default(ddnet_tuning_t *tuning);
/* The defaults a DDNet server starts every map with. */
void ddnet_tuning_init_ddrace(ddnet_tuning_t *tuning);

const char *ddnet_tuning_name(int index);
bool ddnet_tuning_set(ddnet_tuning_t *tuning, int index, float value);
bool ddnet_tuning_get(const ddnet_tuning_t *tuning, int index, float *value);
/* By name, case insensitive, like the "tune" console command. */
bool ddnet_tuning_set_by_name(ddnet_tuning_t *tuning, const char *name, float value);
bool ddnet_tuning_get_by_name(const ddnet_tuning_t *tuning, const char *name, float *value);

#endif
