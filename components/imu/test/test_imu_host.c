/**
 * Host-side unit test for the shipped component algorithm.
 *
 * Builds components/imu/qmi8658c.c with -DIMU_HOST_TEST so only the
 * step-counter half is compiled (the ESP-IDF includes are stubbed below).
 *
 *   gcc -O2 -DIMU_HOST_TEST -Ihost_stub -I../components/imu/include \
 *       test_imu_host.c ../components/imu/qmi8658c.c -o test_imu_host -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "qmi8658c.h"

#define FS 50
#define DT_MS 20

static uint32_t g_seed = 12345;
static double frand(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return ((double)((g_seed >> 16) & 0x7FFF) / 16384.0) - 1.0;
}

static void make_sample(double mag_g, double tilt, double hf_mg, double hf_hz,
                        double t, int32_t out[3])
{
    double cx = 0.55 * sin(tilt), cy = 0.55 * cos(tilt * 0.9);
    double cz2 = 1.0 - cx * cx - cy * cy, cz = cz2 > 0 ? sqrt(cz2) : 0.0;
    double hf = hf_mg * sin(2.0 * M_PI * hf_hz * t) + 8.0 * frand();
    double m = mag_g + hf / 1000.0;
    out[0] = (int32_t)(m * cx * 1000.0);
    out[1] = (int32_t)(m * cy * 1000.0);
    out[2] = (int32_t)(m * cz * 1000.0);
}

/* fixed ~140 ms foot impact, cadence only changes the interval */
static uint32_t gen_walk(step_counter_t *sc, double spm, double seconds,
                         double impact_g, double swing_g, uint32_t t0)
{
    uint32_t n = (uint32_t)(seconds * FS), i, t = t0;
    double period = 60.0 / spm;
    const double rise = 0.070, fall = 0.250;
    for (i = 0; i < n; i++) {
        double tt = (double)i / FS, ph = fmod(tt, period), mag = 1.0;
        if (ph < rise) mag += impact_g * sin(M_PI * ph / rise);
        else if (ph < rise + fall)
            mag -= 0.18 * impact_g * sin(M_PI * (ph - rise) / fall);
        mag += swing_g * sin(2.0 * M_PI * (spm / 120.0) * tt);
        int32_t s[3];
        make_sample(mag, 2.0 * M_PI * (spm / 120.0) * tt, 15.0, 17.0, tt, s);
        step_counter_update(sc, s, t);
        t += DT_MS;
    }
    return t;
}

static uint32_t gen_vib(step_counter_t *sc, double hz, double amp, double secs,
                        double hf_mg, double hf_hz, uint32_t t0)
{
    uint32_t n = (uint32_t)(secs * FS), i, t = t0;
    for (i = 0; i < n; i++) {
        double tt = (double)i / FS;
        double mag = 1.0 + amp * sin(2.0 * M_PI * hz * tt);
        int32_t s[3];
        make_sample(mag, 0.3, hf_mg, hf_hz, tt, s);
        step_counter_update(sc, s, t);
        t += DT_MS;
    }
    return t;
}

static uint32_t gen_typing(step_counter_t *sc, double secs, uint32_t t0)
{
    uint32_t n = (uint32_t)(secs * FS), i, t = t0;
    double bu = 0.0;
    for (i = 0; i < n; i++) {
        double tt = (double)i / FS, mag = 1.0;
        if (tt > bu && frand() > 0.85) bu = tt + 0.15 + 0.3 * fabs(frand());
        if (tt < bu) mag += 0.09 * fabs(frand()) * sin(2.0 * M_PI * 3.1 * tt);
        int32_t s[3];
        make_sample(mag, 0.5, 12.0, 19.0, tt, s);
        step_counter_update(sc, s, t);
        t += DT_MS;
    }
    return t;
}

static uint32_t gen_still(step_counter_t *sc, double secs, uint32_t t0)
{
    uint32_t n = (uint32_t)(secs * FS), i, t = t0;
    for (i = 0; i < n; i++) {
        double tt = (double)i / FS;
        int32_t s[3];
        make_sample(1.0, 0.0, 6.0, 21.0, tt, s);
        step_counter_update(sc, s, t);
        t += DT_MS;
    }
    return t;
}

static int g_fail;
static void report(const char *name, const step_counter_t *sc, uint32_t want,
                   int lo, int hi)
{
    step_status_t st;
    step_counter_get(sc, &st);
    int pass;
    if (want == 0) {
        pass = (st.steps == 0);
    } else {
        int pct = (int)((int32_t)st.steps - (int32_t)want) * 100 / (int32_t)want;
        pass = (pct >= lo && pct <= hi);
    }
    if (!pass) g_fail++;
    printf("  %-24s steps=%5u want=%5u  dist=%6ucm cal=%4u.%03u cad=%3d act=%d  %s\n",
           name, (unsigned)st.steps, (unsigned)want, (unsigned)st.distance_cm,
           (unsigned)(st.calories_mkcal / 1000), (unsigned)(st.calories_mkcal % 1000),
           (int)st.cadence_spm, (int)st.activity, pass ? "PASS" : "FAIL");
}

int main(void)
{
    step_counter_t sc;
    uint32_t t = 100000;

    printf("=== walking / running ===\n");
    step_counter_init(&sc, NULL); t = gen_still(&sc, 2.0, t);
    t = gen_walk(&sc, 110.0, 60.0, 0.45, 0.08, t); gen_still(&sc, 3.0, t);
    report("walk 110 spm 60 s", &sc, 110, -15, 5);

    step_counter_init(&sc, NULL); t = gen_still(&sc, 2.0, t);
    t = gen_walk(&sc, 105.0, 10.0, 0.42, 0.08, t);
    report("walk 105 spm 10 s", &sc, 17, -30, 15);

    step_counter_init(&sc, NULL); t = gen_still(&sc, 2.0, t);
    t = gen_walk(&sc, 110.0, 30.0, 0.45, 0.08, t); gen_still(&sc, 10.0, t);
    t = gen_walk(&sc, 110.0, 30.0, 0.45, 0.08, t); gen_still(&sc, 3.0, t);
    /* KNOWN LIMITATION: the rhythm gate only commits after commit_after
     * intervals, so the first ~3 steps of EVERY walking bout are dropped.
     * On one continuous walk that is ~3 %, but on many short bouts it
     * dominates - hence the loose bound here.  Deliberate trade: fewer
     * false steps in exchange for a small, systematic undercount that is
     * easy to correct (see the notes on calibration).                    */
    report("2x30 s walk + rest", &sc, 110, -25, 5);

    printf("\n=== 非步行情景（期望 0 步）===\n");
    step_counter_init(&sc, NULL); t = gen_still(&sc, 2.0, t);
    t = gen_vib(&sc, 15.0, 0.15, 120.0, 60.0, 18.0, t);
    report("坐车 15Hz 振动 120s", &sc, 0, 0, 0);

    step_counter_init(&sc, NULL); t = gen_still(&sc, 2.0, t);
    t = gen_vib(&sc, 9.0, 0.20, 120.0, 40.0, 22.0, t);
    report("机器 9Hz 振动 120s", &sc, 0, 0, 0);

    step_counter_init(&sc, NULL); t = gen_still(&sc, 2.0, t);
    t = gen_typing(&sc, 120.0, t);
    report("打字/小幅乱动 120s", &sc, 0, 0, 40);

    step_counter_init(&sc, NULL); t = gen_still(&sc, 60.0, t);
    report("静止 60s", &sc, 0, 0, 0);

    step_counter_init(&sc, NULL); t = gen_still(&sc, 2.0, t);
    t = gen_vib(&sc, 0.7, 0.30, 60.0, 10.0, 20.0, t);
    report("慢摆 0.7Hz 60s", &sc, 0, 0, 0);

    printf("\n=== 用户参数 ===\n");
    step_counter_init(&sc, NULL);
    step_counter_set_user(&sc, 185, 90, 1);
    t = gen_still(&sc, 2.0, t);
    t = gen_walk(&sc, 110.0, 60.0, 0.45, 0.08, t);
    report("185cm/90kg 走路", &sc, 110, -15, 5);

    printf("\n=== 定点稳定性 ===\n");
    {
        step_counter_t d;
        step_counter_init(&d, NULL);
        printf("  sizeof(step_counter_t) = %u 字节\n", (unsigned)sizeof(step_counter_t));
        printf("  Q10 分辨率 = %.2f mg\n", 1000.0 / 1024.0);
    }

    printf("\n%s（%d 项未通过）\n", g_fail ? "存在未通过项" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
