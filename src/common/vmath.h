/* Vector and scalar math, expression for expression what DDNet's base/math.h
 * and base/vmath.h do. The physics are only bit-identical if every operation
 * happens in the same order and in the same type (float vs. double), so these
 * helpers are deliberately literal. */
#ifndef DDNET_PHYSICS_COMMON_VMATH_H
#define DDNET_PHYSICS_COMMON_VMATH_H

#include <ddnet_physics/vmath.h>

#include <math.h>
#include <stdbool.h>

typedef ddnet_vec2_t vec2;

/* pi = 3.1415926535897932384626433f */
#define PI 3.1415926535897932384626433f

static inline vec2 v2(float x, float y) { return (vec2){x, y}; }
static inline vec2 vadd(vec2 a, vec2 b) { return (vec2){a.x + b.x, a.y + b.y}; }
static inline vec2 vsub(vec2 a, vec2 b) { return (vec2){a.x - b.x, a.y - b.y}; }
static inline vec2 vneg(vec2 a) { return (vec2){-a.x, -a.y}; }
static inline vec2 vscale(vec2 a, float s) { return (vec2){a.x * s, a.y * s}; }
static inline bool veq(vec2 a, vec2 b) { return a.x == b.x && a.y == b.y; }
static inline bool vne(vec2 a, vec2 b) { return a.x != b.x || a.y != b.y; }

static inline float vdot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
static inline float vlength(vec2 a) { return sqrtf(vdot(a, a)); }
static inline float vdistance(vec2 a, vec2 b) { return vlength(vsub(a, b)); }

/* (inlined where the compiler can be told to: it is on the way of every tick, and a call costs more than it)
 */
#if defined(__GNUC__)
#define VMATH_ALWAYS_INLINE __attribute__((always_inline))
#else
#define VMATH_ALWAYS_INLINE
#endif

static inline VMATH_ALWAYS_INLINE vec2 vnormalize(vec2 v) {
  float divisor = vlength(v);
  if (divisor == 0.0f)
    return (vec2){0.0f, 0.0f};
  float l = 1.0f / divisor;
  return (vec2){v.x * l, v.y * l};
}

static inline vec2 vdirection(float angle) { return (vec2){cosf(angle), sinf(angle)}; }

/* mix(a, b, amount) = a + (b - a) * amount */
static inline vec2 vmix(vec2 a, vec2 b, float amount) { return vadd(a, vscale(vsub(b, a), amount)); }

/* f > 0 ? (int)(f + 0.5f) : (int)(f - 0.5f), written so that it compiles without a branch */
static inline int round_to_int(float f) { return (int)(f + (f > 0 ? 0.5f : -0.5f)); }

/* std::clamp, std::min and std::max with their exact comparison order, which
 * matters as soon as a NaN is involved. */
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (hi < v ? hi : v); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }
static inline int mini(int a, int b) { return b < a ? b : a; }
static inline int maxi(int a, int b) { return a < b ? b : a; }
static inline float maxf(float a, float b) { return a < b ? b : a; }
static inline float minf(float a, float b) { return b < a ? b : a; }

static inline float absolutef(float a) { return a < 0.0f ? -a : a; }
static inline int absolutei(int a) { return a < 0 ? -a : a; }

static inline bool closest_point_on_line(vec2 line_point_a, vec2 line_point_b, vec2 target_point,
                                         vec2 *out_pos) {
  vec2 ab = vsub(line_point_b, line_point_a);
  float squared_magnitude_ab = vdot(ab, ab);
  if (squared_magnitude_ab > 0) {
    vec2 ap = vsub(target_point, line_point_a);
    float ap_dot_ab = vdot(ap, ab);
    float t = ap_dot_ab / squared_magnitude_ab;
    *out_pos = vadd(line_point_a, vscale(ab, clampf(t, 0.0f, 1.0f)));
    return true;
  }
  return false;
}

static inline float saturated_add(float min, float max, float current, float modifier) {
  if (modifier < 0) {
    if (current < min)
      return current;
    current += modifier;
    if (current < min)
      current = min;
    return current;
  } else {
    if (current > max)
      return current;
    current += modifier;
    if (current > max)
      current = max;
    return current;
  }
}

#endif
