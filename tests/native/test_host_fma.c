// [XCDE] Host FMA3 (runtime/simd.h SIMD_FMA): the vfmadd path must give exactly the library fma's bits for every
// guest fused op (FMADD scalar, FMLA vector, FRECPS, FRSQRTS) and FPCR mode (DN, FZ, RMode). No game data needed.
// On a CPU without FMA3 only the library path runs (and the test says so).
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "simd.h"

#if defined(__x86_64__) && !defined(__FMA__)
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static const uint32_t S_EDGE[] = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0xffc00123,
                                  0x00000001, 0x007fffff, 0x00800000, 0x7f7fffff, 0x3f800000, 0xbf800000, 0x40000000,
                                  0x3fc00000, 0x40400000, 0x33800000, 0x0c000000};
static const uint64_t D_EDGE[] = {0, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
                                  0x7ff8000000000000ull, 0x7ff0000000000001ull, 0x0000000000000001ull,
                                  0x000fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull,
                                  0x3ff0000000000000ull, 0xbff0000000000000ull, 0x4008000000000000ull,
                                  // the five finite inputs zig 0.13's software fma turned into NaN
                                  0x0adb6fd3480b352aull, 0x3fdfffffffffffffull, 0x825c400000000000ull,
                                  0xbf63000000000000ull, 0x21ceec47c7a82793ull, 0x495ca3c9c307c81cull,
                                  0xf871d178d9631cdeull};

static uint32_t s_val(void) {
    uint64_t k = rnd() % 4;
    if (k == 0) return S_EDGE[rnd() % (sizeof S_EDGE / sizeof *S_EDGE)];
    if (k == 1) return (uint32_t)rnd();
    // game-like: |x| in 2^-24 .. 2^24
    return ((uint32_t)rnd() & 0x807fffffu) | (uint32_t)(127 - 24 + rnd() % 48) << 23;
}
static uint64_t d_val(void) {
    uint64_t k = rnd() % 4;
    if (k == 0) return D_EDGE[rnd() % (sizeof D_EDGE / sizeof *D_EDGE)];
    if (k == 1) return rnd();
    return (rnd() & 0x800fffffffffffffull) | (uint64_t)(1023 - 40 + rnd() % 80) << 52;
}
static uint64_t fpcr_val(void) {
    // half the cases in the default mode (fast path), the rest with random DN / FZ / RMode (slow path)
    return (rnd() & 1) ? 0 : (rnd() & (FPCR_DN | FPCR_FZ | FPCR_RM));
}

// Run one case in both modes; return 0 when the bits match.
typedef void (*Case)(Cpu *c);
static int both(Case f, const Cpu *in, Cpu *hw, Cpu *lib) {
    *hw = *in, *lib = *in;
    simd_host_fma = 2; f(hw);
    simd_host_fma = 1; f(lib);
    return memcmp(hw, lib, sizeof *hw);
}

static int op_sel;
static void c_fma_s(Cpu *c) { fp_fma_s(c, op_sel & 1, op_sel >> 1 & 1, 0, 1, 2, 3); }
static void c_fma_d(Cpu *c) { fp_fma_d(c, op_sel & 1, op_sel >> 1 & 1, 0, 1, 2, 3); }
static const int VOPS[] = {FOP_MADD, FOP_RECPS, FOP_RSQRTS};
static void c_vop_s(Cpu *c) { VREG(vf32, 0) = vop_s(c, VOPS[op_sel % 3], VREG(vf32, 1), VREG(vf32, 2), VREG(vf32, 3)); }
static void c_vop_d(Cpu *c) { VREG(vf64, 0) = vop_d(c, VOPS[op_sel % 3], VREG(vf64, 1), VREG(vf64, 2), VREG(vf64, 3)); }

// Exact double answers (Python math.fma / glibc software fma), through the guest FMADD with its FPCR.RMode:
// result = a + n * m. The first five are the finite inputs zig 0.13's fma returned NaN for; the next two are
// zig 0.17.1's misrounding and its ignored rounding mode.
static const struct { uint64_t rm, n, m, a, want; } KNOWN[] = {
    {0, 0x0adb6fd3480b352aull, 0x0adb6fd3480b352aull, 0x3fdfffffffffffffull, 0x3fdfffffffffffffull},
    {0, 0x825c400000000000ull, 0x825c400000000000ull, 0xbf63000000000000ull, 0xbf63000000000000ull},
    {0, 0x000000000000003full, 0x000000000000003full, 0xbc66377d0ae3de5dull, 0xbc66377d0ae3de5dull},
    {0, 0x807fffff3f7ff000ull, 0x80200000c290e687ull, 0xc2d500003f000000ull, 0xc2d500003f000000ull},
    {0, 0x21ceec47c7a82793ull, 0x495ca3c9c307c81cull, 0xf871d178d9631cdeull, 0xf871d178d9631cdeull},
    {0, 0xbe4f2bb05905d2c8ull, 0x3e3623ba1dfc05a7ull, 0x3ff0000000000000ull, 0x3fefffffffffffffull}, // 1 - 7.5e-17
    {1, 0x3ff0000000000000ull, 0x39b4484bfeebc2a0ull, 0x3ff0000000000000ull, 0x3ff0000000000001ull}, // 1 + 1e-30, up
    {3, 0x7e37e43c8800759cull, 0x7e37e43c8800759cull, 0, 0x7fefffffffffffffull}, // 1e300^2 toward zero: max finite
};
static int known_answers(void) {
    int bad = 0;
    simd_host_fma = 2;
    for (unsigned i = 0; i < sizeof KNOWN / sizeof *KNOWN; i++) {
        Cpu c; memset(&c, 0, sizeof c);
        c.fpcr = KNOWN[i].rm << 22;
        c.v[1] = KNOWN[i].n, c.v[2] = KNOWN[i].m, c.v[3] = KNOWN[i].a;
        fp_fma_d(&c, 0, 0, 0, 1, 2, 3);
        if ((uint64_t)c.v[0] != KNOWN[i].want) {
            fprintf(stderr, "known answer %u: got %016llx, want %016llx\n", i, (unsigned long long)(uint64_t)c.v[0],
                    (unsigned long long)KNOWN[i].want);
            bad++;
        }
    }
    printf("known     %u exact double answers, %d wrong\n", (unsigned)(sizeof KNOWN / sizeof *KNOWN), bad);
    return bad;
}

static double bench(int mode, int n) {
    vf32 a = {1.0f, 2.0f, 3.0f, 4.0f}, b = {0.999f, 0.998f, 0.997f, 0.996f}, d = {0.5f, 0.25f, 0.125f, 0.0625f};
    Cpu c; memset(&c, 0, sizeof c);
    simd_host_fma = mode;
    clock_t t0 = clock();
    for (int i = 0; i < n; i++) {
        a = vop_s(&c, FOP_MADD, d, a, b); // the FMLA dependency chain: a = d + a * b
        __asm__ volatile("" : "+x"(a));
    }
    clock_t t1 = clock();
    volatile float sink = a[0]; (void)sink;
    return (double)(t1 - t0) * 1e9 / CLOCKS_PER_SEC / n;
}

int main(void) {
    int hw = simd_host_fma_detect() == 2;
    if (!hw) {
        printf("test_host_fma: this CPU has no FMA3 (or XCDE_HOST_FMA=0): only the library path is used, nothing to "
               "compare\n");
        return 0;
    }
    static const struct { const char *name; Case f; int prec; } cases[] = {
        {"FMADD s", c_fma_s, 32}, {"FMADD d", c_fma_d, 64}, {"vector s", c_vop_s, 32}, {"vector d", c_vop_d, 64}};
    const int N = 200000;
    int bad = 0;
    for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
#ifdef _WIN32
        // Windows links zig's software fma (compiler_rt), which is no reference for doubles: it ignores the rounding
        // mode and misrounds some results next to a power of two (see KNOWN). Doubles are compared with glibc on
        // Linux, and with the exact answers below here; floats (zig's fmaf is exact) are compared everywhere.
        if (cases[k].prec == 64) continue;
#endif
        int mism = 0;
        for (int i = 0; i < N; i++) {
            Cpu in, h, l;
            memset(&in, 0, sizeof in);
            in.fpcr = fpcr_val();
            op_sel = (int)(rnd() % 12);
            for (int r = 1; r <= 3; r++)
                for (int lane = 0; lane < 4; lane++) {
                    if (cases[k].prec == 32) ((uint32_t *)&in.v[r])[lane] = s_val();
                    else if (lane < 2) ((uint64_t *)&in.v[r])[lane] = d_val();
                }
            if (both(cases[k].f, &in, &h, &l)) {
                if (mism++ < 5)
                    fprintf(stderr, "%s mismatch: fpcr %llx sel %d in %016llx%016llx %016llx%016llx %016llx%016llx"
                            " hw %016llx%016llx lib %016llx%016llx\n", cases[k].name, (unsigned long long)in.fpcr, op_sel,
                            (unsigned long long)(uint64_t)(in.v[1] >> 64), (unsigned long long)(uint64_t)in.v[1],
                            (unsigned long long)(uint64_t)(in.v[2] >> 64), (unsigned long long)(uint64_t)in.v[2],
                            (unsigned long long)(uint64_t)(in.v[3] >> 64), (unsigned long long)(uint64_t)in.v[3],
                            (unsigned long long)(uint64_t)(h.v[0] >> 64), (unsigned long long)(uint64_t)h.v[0],
                            (unsigned long long)(uint64_t)(l.v[0] >> 64), (unsigned long long)(uint64_t)l.v[0]);
            }
        }
        printf("%-9s %d cases, %d mismatches\n", cases[k].name, N, mism);
        bad += mism;
    }
    bad += known_answers();
    double t_lib = bench(1, 20000000), t_hw = bench(2, 20000000);
    printf("vector FMLA (4 x f32): library %.1f ns, vfmadd %.1f ns (%.1fx)\n", t_lib, t_hw, t_lib / t_hw);
    if (bad) { fprintf(stderr, "test_host_fma: FAIL\n"); return 1; }
    printf("test_host_fma: OK\n");
    return 0;
}
#else
int main(void) { printf("test_host_fma: not an x86-64-v2 build, SIMD_FMA is the compiler's fma\n"); return 0; }
#endif
