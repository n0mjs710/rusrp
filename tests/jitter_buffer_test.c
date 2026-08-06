/* Standalone test for the arrival-order FIFO jitter buffer (jitter_buffer.c).
 * Covers the behavior that replaced the seq-windowed design: no
 * sequence-number filtering, and bounded overflow that drops oldest-not-
 * newest. Unlike the sibling usrp-rc project's version of this fix, there
 * is deliberately no priming ramp here -- watchdog.c already defers
 * asserting PTT until jitter_buffer_ms after network keyup, so every pull
 * that finds nothing buffered is a genuine underrun from the very first
 * pull, not a "still filling" state. Run via `make check`. */

#include "../src/jitter_buffer.h"
#include "../src/usrp_protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL - %s\n", msg); \
        failures++; \
    } else { \
        printf("  ok   - %s\n", msg); \
    } \
} while (0)

static void make_frame(int16_t *out, int16_t fill)
{
    for (unsigned i = 0; i < USRP_AUDIO_FRAMES; i++)
        out[i] = fill;
}

static void test_steady_state_order(void)
{
    printf("\n[1] steady-state: frames drain in strict arrival (push) order\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 200u); /* capacity for 10 frames -- comfortably more than the 5 pushed below */

    int16_t out[USRP_AUDIO_FRAMES];
    for (int16_t v = 0; v < 5; v++) {
        int16_t f[USRP_AUDIO_FRAMES];
        make_frame(f, v);
        jitter_buffer_push(jb, f);
    }

    int ok = 1;
    for (int16_t v = 0; v < 5; v++) {
        bool real = jitter_buffer_pull(jb, out);
        if (!real || out[0] != v) ok = 0;
    }
    CHECK(ok, "5 pushed frames drain in the exact order pushed");

    jitter_buffer_destroy(jb);
}

static void test_no_priming(void)
{
    printf("\n[2] no priming ramp: a single pushed frame is immediately playable\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 100u); /* even at a large configured capacity */

    int16_t f[USRP_AUDIO_FRAMES], out[USRP_AUDIO_FRAMES];
    make_frame(f, 77);
    jitter_buffer_push(jb, f);
    bool real = jitter_buffer_pull(jb, out);
    CHECK(real && out[0] == 77,
          "one push is enough -- watchdog.c owns the startup cushion, not this buffer");

    jitter_buffer_destroy(jb);
}

static void test_overflow_drops_oldest(void)
{
    printf("\n[3] overflow: pushing past capacity drops oldest, keeps freshest, counts it\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 20u); /* 1 frame -> clamped up to JITTER_CAPACITY_MIN_MS (40ms/2 frames) */

    /* Capacity clamps to JITTER_CAPACITY_MIN_MS (40ms = 2 frames). Push 5. */
    for (int16_t v = 0; v < 5; v++) {
        int16_t f[USRP_AUDIO_FRAMES];
        make_frame(f, v);
        jitter_buffer_push(jb, f);
    }

    uint64_t overflow = jitter_buffer_overflow_count(jb);
    CHECK(overflow == 3, "3 pushes beyond the clamped 2-frame capacity were counted as overflow");

    int16_t out[USRP_AUDIO_FRAMES];
    bool real = jitter_buffer_pull(jb, out);
    CHECK(real && out[0] == 3, "oldest surviving frame is #3 (0..2 were dropped to make room)");

    jitter_buffer_destroy(jb);
}

static void test_genuine_underrun(void)
{
    printf("\n[4] genuine underrun: buffer runs dry mid-stream\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 40u);

    int16_t f[USRP_AUDIO_FRAMES], out[USRP_AUDIO_FRAMES];
    make_frame(f, 42);
    jitter_buffer_push(jb, f);
    jitter_buffer_pull(jb, out);

    jitter_buffer_reset_silence_count(jb);
    bool real = jitter_buffer_pull(jb, out);
    CHECK(!real, "pull with nothing buffered returns silence");
    int16_t all_zero = 0;
    for (unsigned i = 0; i < USRP_AUDIO_FRAMES; i++) all_zero |= out[i];
    CHECK(all_zero == 0, "underrun silence is actually zeroed, not stale data");

    jitter_buffer_latch_silence(jb);
    uint64_t silence = jitter_buffer_latched_silence_count(jb);
    CHECK(silence == 1, "genuine underrun is counted as silence");

    jitter_buffer_destroy(jb);
}

static void test_flush_drains(void)
{
    printf("\n[5] flush drains pending audio (called on UNKEY)\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 40u);

    int16_t f[USRP_AUDIO_FRAMES], out[USRP_AUDIO_FRAMES];
    make_frame(f, 7);
    jitter_buffer_push(jb, f);
    jitter_buffer_push(jb, f);

    jitter_buffer_flush(jb);

    bool real = jitter_buffer_pull(jb, out);
    CHECK(!real, "after flush, pending frames are gone -- pull is a genuine underrun");

    jitter_buffer_destroy(jb);
}

static void test_no_sequence_gating(void)
{
    printf("\n[6] no sequence-number gating: this is the whole point of the rewrite\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 40u);

    /* Under the old design, pushing frames with wildly discontinuous or
     * out-of-window sequence numbers (as ASL3 apparently sometimes does)
     * would get silently discarded as "late". This buffer takes no
     * sequence number parameter at all -- there is nothing to discard on
     * that basis. Any frame pushed is accepted in arrival order, full stop. */
    int16_t f[USRP_AUDIO_FRAMES], out[USRP_AUDIO_FRAMES];
    make_frame(f, 1);
    jitter_buffer_push(jb, f); /* would have been "seq 0" */
    make_frame(f, 2);
    jitter_buffer_push(jb, f); /* would have been e.g. "seq 999999" -- huge jump, doesn't matter */

    bool real = jitter_buffer_pull(jb, out);
    CHECK(real && out[0] == 1, "first frame accepted regardless of any notion of sequence continuity");
    real = jitter_buffer_pull(jb, out);
    CHECK(real && out[0] == 2, "second frame accepted too -- arrival order is the only order that exists");

    uint64_t overflow = jitter_buffer_overflow_count(jb);
    CHECK(overflow == 0, "no overflow/discard occurred -- a seq discontinuity is not a buffer event here");

    jitter_buffer_destroy(jb);
}

int main(void)
{
    test_steady_state_order();
    test_no_priming();
    test_overflow_drops_oldest();
    test_genuine_underrun();
    test_flush_drains();
    test_no_sequence_gating();

    printf("\n==== %s ====\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
