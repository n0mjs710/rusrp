/* Numeric sanity check for the MMDVM-matching bandpass filter ported into
 * audio_processing.c from usrp-rc's voice_filter.c. Verifies the filter's
 * actual measured frequency response end-to-end (not just a re-read of the
 * transcribed coefficients) is bandpass-shaped around 300-2700 Hz, catching
 * any transcription error in the port. Run via `make check`. */

#include "../src/audio_processing.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define FS 8000.0
#define SETTLE_SAMPLES 4000   /* let the IIR settle well past its transient */
#define MEASURE_SAMPLES 4000

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL - %s\n", msg); \
        failures++; \
    } else { \
        printf("  ok   - %s\n", msg); \
    } \
} while (0)

/* Feeds a full-scale sine at freq_hz through the filter (bandpass only, no
 * gain/HPF) in 160-sample frames, measures RMS in dBFS of the tail after the
 * IIR transient has settled. */
static float measure_gain_db(float freq_hz)
{
    audio_proc_t *p;
    audio_proc_create(&p, false /*hpf*/, true /*bandpass*/, 0.0f /*gain_db*/);

    int total = SETTLE_SAMPLES + MEASURE_SAMPLES;
    double sum_sq = 0.0;
    int measured = 0;

    int16_t frame[160];
    int n = 0;
    for (int i = 0; i < total; i++) {
        double t = (double)i / FS;
        frame[n++] = (int16_t)(32000.0 * sin(2.0 * M_PI * freq_hz * t));
        if (n == 160) {
            audio_proc_run(p, frame, 160);
            for (int k = 0; k < 160; k++) {
                int idx = i - 159 + k;
                if (idx >= SETTLE_SAMPLES) {
                    double s = (double)frame[k];
                    sum_sq += s * s;
                    measured++;
                }
            }
            n = 0;
        }
    }

    audio_proc_destroy(p);

    double rms = sqrt(sum_sq / (double)measured);
    /* Reference is the RMS of a full-scale sine (peak/sqrt(2)), not the
     * peak itself -- comparing RMS to peak would introduce a spurious
     * constant -3.01 dB offset into every measurement. */
    double input_rms = 32000.0 / sqrt(2.0);
    return 20.0f * log10f((float)(rms / input_rms));
}

int main(void)
{
    printf("\n[1] bandpass shape: in-band passes near 0 dB, well out-of-band is heavily attenuated\n");

    float g_150   = measure_gain_db(150.0f);    /* below 300 Hz corner */
    float g_1000  = measure_gain_db(1000.0f);   /* mid passband */
    float g_2000  = measure_gain_db(2000.0f);   /* still in passband */
    float g_3500  = measure_gain_db(3500.0f);   /* above 2700 Hz corner */

    printf("  150 Hz:  %6.1f dB\n",  g_150);
    printf("  1000 Hz: %6.1f dB\n",  g_1000);
    printf("  2000 Hz: %6.1f dB\n",  g_2000);
    printf("  3500 Hz: %6.1f dB\n",  g_3500);

    CHECK(g_1000 > -1.0f && g_1000 < 1.0f, "1000 Hz (mid passband) is within 1 dB of unity gain");
    CHECK(g_2000 > -3.0f, "2000 Hz (in-band) is not heavily attenuated");
    CHECK(g_150 < g_1000 - 10.0f, "150 Hz (below 300 Hz corner) is well below passband level");
    CHECK(g_3500 < g_1000 - 10.0f, "3500 Hz (above 2700 Hz corner) is well below passband level");

    printf("\n==== %s ====\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
