/* glibc's powf(), for VelocityRamp() (see velocity_ramp()): the way it
 * takes for a positive normal x and a y whose x^y is between 2^-126 and 2^126,
 * operation by operation, from sysdeps/ieee754/flt-32/e_powf.c (glibc 2.28 and
 * later). glibc on x86_64 has two builds of it, with the same operations: one
 * for SSE2, and one for CPUs with FMA and AVX2, in which the compiler fused
 * every a * b + c into one rounding (which is what the code it built does,
 * looked up instruction by instruction). The two give different results for a
 * few inputs, so the one to use is the one that glibc took, see
 * velocity_ramp_prepare(). The constants are glibc's __powf_log2_data and
 * __exp2f_data.
 *
 * Checked against glibc's powf() for every y from -150 to 150 with
 * x = 1.4 (the default velramp_curvature) and every y from -1000 to 1000 with
 * x = 1.1, 2, 0.7 and 3.3, for both builds: the same bits for all of them. */
#ifndef DDNET_OPTIMIZED_POWF_H
#define DDNET_OPTIMIZED_POWF_H

#include <stdint.h>
#include <string.h>

/* (for x86_64 with GCC or Clang, which build the FMA one where asked) */
#if defined(__x86_64__) && defined(__GNUC__)
#define DDNET_POWF_REPLICA 1
#else
#define DDNET_POWF_REPLICA 0
#endif

static const struct {
  double invc, logc;
} POWF_LOG2_TABLE[16] = {
    {0x1.661ec79f8f3bep+0, -0x1.efec65b963019p-2}, {0x1.571ed4aaf883dp+0, -0x1.b0b6832d4fca4p-2},
    {0x1.49539f0f010b0p+0, -0x1.7418b0a1fb77bp-2}, {0x1.3c995b0b80385p+0, -0x1.39de91a6dcf7bp-2},
    {0x1.30d190c8864a5p+0, -0x1.01d9bf3f2b631p-2}, {0x1.25e227b0b8ea0p+0, -0x1.97c1d1b3b7af0p-3},
    {0x1.1bb4a4a1a343fp+0, -0x1.2f9e393af3c9fp-3}, {0x1.12358f08ae5bap+0, -0x1.960cbbf788d5cp-4},
    {0x1.0953f419900a7p+0, -0x1.a6f9db6475fcep-5}, {0x1.0000000000000p+0, 0x0.0p+0},
    {0x1.e608cfd9a47acp-1, 0x1.338ca9f24f53dp-4},  {0x1.ca4b31f026aa0p-1, 0x1.476a9543891bap-3},
    {0x1.b2036576afce6p-1, 0x1.e840b4ac4e4d2p-3},  {0x1.9c2d163a1aa2dp-1, 0x1.40645f0c6651cp-2},
    {0x1.886e6037841edp-1, 0x1.88e9c2c1b9ff8p-2},  {0x1.767dcf5534862p-1, 0x1.ce0a44eb17bccp-2},
};
static const double POWF_LOG2_POLY[5] = {0x1.27616c9496e0bp-2, -0x1.71969a075c67ap-2, 0x1.ec70a6ca7baddp-2,
                                         -0x1.7154748bef6c8p-1, 0x1.71547652ab82bp+0};
static const double POWF_EXP2_POLY[3] = {0x1.c6af84b912394p-5, 0x1.ebfce50fac4f3p-3, 0x1.62e42ff0c52d6p-1};
static const double POWF_EXP2_SHIFT = 0x1.8p+47;
static const uint64_t POWF_EXP2_TABLE[32] = {
    0x3ff0000000000000ull, 0x3fefd9b0d3158574ull, 0x3fefb5586cf9890full, 0x3fef9301d0125b51ull,
    0x3fef72b83c7d517bull, 0x3fef54873168b9aaull, 0x3fef387a6e756238ull, 0x3fef1e9df51fdee1ull,
    0x3fef06fe0a31b715ull, 0x3feef1a7373aa9cbull, 0x3feedea64c123422ull, 0x3feece086061892dull,
    0x3feebfdad5362a27ull, 0x3feeb42b569d4f82ull, 0x3feeab07dd485429ull, 0x3feea47eb03a5585ull,
    0x3feea09e667f3bcdull, 0x3fee9f75e8ec5f74ull, 0x3feea11473eb0187ull, 0x3feea589994cce13ull,
    0x3feeace5422aa0dbull, 0x3feeb737b0cdc5e5ull, 0x3feec49182a3f090ull, 0x3feed503b23e255dull,
    0x3feee89f995ad3adull, 0x3feeff76f2fb5e47ull, 0x3fef199bdd85529cull, 0x3fef3720dcef9069ull,
    0x3fef5818dcfba487ull, 0x3fef7c97337b9b5full, 0x3fefa4afa2a490daull, 0x3fefd0765b6e4540ull,
};

/* log2_inline(): log2(x) for a positive normal x, given as its bits. (The
 * operand order of a product or a sum is not that of glibc everywhere, which
 * does not change its result.) */
#define POWF_LOG2(FMA)                                                                                       \
  const uint32_t tmp = ix - 0x3f330000;                                                                      \
  const int i = (int)((tmp >> 19) % 16);                                                                     \
  const uint32_t top = tmp & 0xff800000;                                                                     \
  const uint32_t iz = ix - top;                                                                              \
  const int k = (int32_t)top >> 23;                                                                          \
  float zf;                                                                                                  \
  memcpy(&zf, &iz, sizeof(zf));                                                                              \
  const double z = zf, invc = POWF_LOG2_TABLE[i].invc, logc = POWF_LOG2_TABLE[i].logc;                       \
  const double *A = POWF_LOG2_POLY;                                                                          \
  const double r = FMA(z, invc, -1.0);                                                                       \
  const double y0 = logc + (double)k;                                                                        \
  const double r2 = r * r;                                                                                   \
  const double y = FMA(A[0], r, A[1]);                                                                       \
  const double p = FMA(A[2], r, A[3]);                                                                       \
  const double r4 = r2 * r2;                                                                                 \
  double q = FMA(A[4], r, y0);                                                                               \
  q = FMA(p, r2, q);                                                                                         \
  return FMA(y, r4, q);

/* exp2_inline() for a positive result, rounded to float */
#define POWF_EXP2(FMA)                                                                                       \
  double kd = xd + POWF_EXP2_SHIFT;                                                                          \
  uint64_t ki;                                                                                               \
  memcpy(&ki, &kd, sizeof(ki));                                                                              \
  kd -= POWF_EXP2_SHIFT;                                                                                     \
  const double r = xd - kd;                                                                                  \
  const uint64_t t = POWF_EXP2_TABLE[ki % 32] + (ki << 47);                                                  \
  double s;                                                                                                  \
  memcpy(&s, &t, sizeof(s));                                                                                 \
  const double *C = POWF_EXP2_POLY;                                                                          \
  const double z = FMA(C[0], r, C[1]);                                                                       \
  const double r2 = r * r;                                                                                   \
  double y = FMA(C[2], r, 1.0);                                                                              \
  y = FMA(z, r2, y);                                                                                         \
  return (float)(y * s);

/* (the library is built with -ffp-contract=off: a * b + c is two roundings) */
#define POWF_MUL_ADD(a, b, c) ((a) * (b) + (c))
static inline double powf_log2_sse2(uint32_t ix) { POWF_LOG2(POWF_MUL_ADD) }
static inline float powf_exp2_sse2(double xd) { POWF_EXP2(POWF_MUL_ADD) }
/* (only called where glibc took its FMA build, which it does on a CPU that has FMA) */
__attribute__((target("fma"))) static inline double powf_log2_fma(uint32_t ix) { POWF_LOG2(__builtin_fma) }
__attribute__((target("fma"))) static inline float powf_exp2_fma(double xd) { POWF_EXP2(__builtin_fma) }

/* Whether powf(x, y) takes the way above, for logx = log2_inline(x) of an x
 * for which it does: not if y is 0, infinite or not a number, or if x^y is too
 * large or too small. Then x^y = exp2_inline(*ylogx). */
static inline int powf_fast(double logx, float y, double *ylogx) {
  uint32_t iy;
  memcpy(&iy, &y, sizeof(iy));
  if (2 * iy - 1 >= 2u * 0x7f800000 - 1)
    return 0;
  *ylogx = (double)y * logx;
  uint64_t bits;
  memcpy(&bits, ylogx, sizeof(bits));
  return (bits >> 47 & 0xffff) < 0x80bf;
}

#endif
