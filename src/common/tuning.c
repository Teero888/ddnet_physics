#include <ddnet_physics/tuning.h>

#include <limits.h>
#include <stddef.h>
#include <strings.h>

static const char *const TUNING_NAMES[] = {
#define DDNET_TUNING_PARAM(name, default_value) #name,
#include <ddnet_physics/tuning_params.h>
#undef DDNET_TUNING_PARAM
};

const char *ddnet_tuning_name(int index) {
  if (index < 0 || index >= DDNET_NUM_TUNING_PARAMS)
    return NULL;
  return TUNING_NAMES[index];
}

/* CTuneParam::operator=(float) */
static ddnet_tune_param_t tune_param_from_float(float v) {
  const float fixed = v * 100.0f;
  if (fixed >= (float)INT_MIN && fixed < (float)INT_MAX)
    return (int)fixed;
  return INT_MIN;
}

bool ddnet_tuning_set(ddnet_tuning_t *tuning, int index, float value) {
  if (index < 0 || index >= DDNET_NUM_TUNING_PARAMS)
    return false;
  ((ddnet_tune_param_t *)tuning)[index] = tune_param_from_float(value);
  return true;
}

bool ddnet_tuning_get(const ddnet_tuning_t *tuning, int index, float *value) {
  if (index < 0 || index >= DDNET_NUM_TUNING_PARAMS)
    return false;
  *value = ddnet_tune(((const ddnet_tune_param_t *)tuning)[index]);
  return true;
}

bool ddnet_tuning_set_by_name(ddnet_tuning_t *tuning, const char *name, float value) {
  for (int i = 0; i < DDNET_NUM_TUNING_PARAMS; i++)
    if (strcasecmp(name, TUNING_NAMES[i]) == 0)
      return ddnet_tuning_set(tuning, i, value);
  return false;
}

bool ddnet_tuning_get_by_name(const ddnet_tuning_t *tuning, const char *name, float *value) {
  for (int i = 0; i < DDNET_NUM_TUNING_PARAMS; i++)
    if (strcasecmp(name, TUNING_NAMES[i]) == 0)
      return ddnet_tuning_get(tuning, i, value);
  return false;
}

void ddnet_tuning_init_default(ddnet_tuning_t *tuning) {
#define DDNET_TUNING_PARAM(name, default_value) tuning->name = tune_param_from_float(default_value);
#include <ddnet_physics/tuning_params.h>
#undef DDNET_TUNING_PARAM
}

/* CGameContext::ResetTuning() */
void ddnet_tuning_init_ddrace(ddnet_tuning_t *tuning) {
  ddnet_tuning_init_default(tuning);
  ddnet_tuning_set_by_name(tuning, "gun_speed", 1400);
  ddnet_tuning_set_by_name(tuning, "gun_curvature", 0);
  ddnet_tuning_set_by_name(tuning, "shotgun_speed", 500);
  ddnet_tuning_set_by_name(tuning, "shotgun_speeddiff", 0);
  ddnet_tuning_set_by_name(tuning, "shotgun_curvature", 0);
}
