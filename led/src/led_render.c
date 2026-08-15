/*
 * LED renderer — the only code that touches the GPIOs. Runs on BOTH halves.
 *
 * Two things it has to get right:
 *
 * 1. NEVER drive two channels at once. The 3-colour array shares one anode fed
 *    from a GPIO (the extender pad's P0.24 rail), which cannot source enough
 *    current for two colours simultaneously. So a "mixed" colour is time-
 *    multiplexed one channel at a time, fast enough that persistence of vision
 *    blends it. Peak current stays at the single-channel budget — which also
 *    means a mixed colour costs no more battery than a single one.
 *    (This constraint is inherited from status_led_ext, which discovered it.)
 *
 * 2. Duty cycle is the ONLY battery lever (there is no PWM). The blink patterns
 *    are therefore both the signal vocabulary and the power knob: a slow blink is
 *    ~2.5% of the current a solid colour draws.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <zmk_led_config/config.h>

LOG_MODULE_DECLARE(led_config, CONFIG_ZMK_LED_CONFIG_LOG_LEVEL);

#define LEDX_RED_NODE DT_NODELABEL(ledx_red)
#define LEDX_YG_NODE DT_NODELABEL(ledx_yg)
#define LEDX_GRN_NODE DT_NODELABEL(ledx_grn)

#if DT_NODE_EXISTS(LEDX_RED_NODE) && DT_NODE_EXISTS(LEDX_YG_NODE) && DT_NODE_EXISTS(LEDX_GRN_NODE)

#define CH_COUNT 3

static const struct gpio_dt_spec ledx_ch[CH_COUNT] = {
    GPIO_DT_SPEC_GET(LEDX_RED_NODE, gpios),
    GPIO_DT_SPEC_GET(LEDX_YG_NODE, gpios),
    GPIO_DT_SPEC_GET(LEDX_GRN_NODE, gpios),
};

/* per-channel dwell; 2 colours => ~8 ms cycle (~125 Hz), no visible flicker */
#define LEDX_MUX_STEP_MS 4

static struct k_spinlock ledx_lock;
static struct k_work_delayable ledx_mux_work;
static struct k_work_delayable ledx_phase_work;

static uint8_t ledx_colour;  /* requested channels */
static uint8_t ledx_pattern; /* enum led_pattern */
static bool ledx_phase_on;   /* is the blink currently in its ON phase */
static uint8_t ledx_mux_pos;
static uint8_t ledx_dbl_step; /* sub-step for LED_PAT_DOUBLE */
static bool ledx_ready;
/* When a one-shot flash is due to end. The expiry handler may already be running
 * when a new state arrives (k_work_cancel_delayable does not wait for it), and
 * without this it would clear the colour we had JUST been asked to show and leave
 * the LED dark for good. It only clears if the flash it was scheduled for is in
 * fact the one still running. */
static int64_t ledx_flash_deadline;

/* What the LED should be showing right now = requested colour, gated by the
 * blink phase. */
static inline uint8_t effective_mask(void) { return ledx_phase_on ? ledx_colour : 0; }

/* Break before make, always: clear every channel we are turning OFF before we turn
 * any ON. A set-then-clear loop would briefly light two cathodes on the wrap-around
 * tick, which the shared anode cannot feed. */
static void drive_static(uint8_t mask) {
    for (int i = 0; i < CH_COUNT; i++) {
        if (!(mask & BIT(i))) {
            gpio_pin_set_dt(&ledx_ch[i], 0);
        }
    }
    for (int i = 0; i < CH_COUNT; i++) {
        if (mask & BIT(i)) {
            gpio_pin_set_dt(&ledx_ch[i], 1);
        }
    }
}

static void drive_one(int ch) {
    for (int i = 0; i < CH_COUNT; i++) {
        if (i != ch) {
            gpio_pin_set_dt(&ledx_ch[i], 0);
        }
    }
    gpio_pin_set_dt(&ledx_ch[ch], 1);
}

/* Light one set channel per tick while >=2 are requested. Re-reads the live state
 * each tick, so it self-heals back to a static drive the moment the request drops
 * to <=1 channel (we must never end up driving two at once). */
static void ledx_mux_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&ledx_lock);

    const uint8_t mask = effective_mask();
    if (POPCOUNT(mask) < 2) {
        drive_static(mask);
        k_spin_unlock(&ledx_lock, key);
        return;
    }
    int ch = ledx_mux_pos;
    for (int step = 0; step < CH_COUNT; step++) {
        ch = (ch + 1) % CH_COUNT;
        if (mask & BIT(ch)) {
            break;
        }
    }
    ledx_mux_pos = (uint8_t)ch;
    drive_one(ch);
    k_spin_unlock(&ledx_lock, key);

    k_work_reschedule(&ledx_mux_work, K_MSEC(LEDX_MUX_STEP_MS));
}

/* Apply the current phase: static drive for 0/1 channel, hand off to the mux for 2+.
 * Callers hold ledx_lock. */
static void refresh(void) {
    const uint8_t mask = effective_mask();
    if (POPCOUNT(mask) < 2) {
        k_work_cancel_delayable(&ledx_mux_work);
        drive_static(mask);
    } else {
        k_work_reschedule(&ledx_mux_work, K_NO_WAIT);
    }
}

/* Blink state machine. Returns the delay until the next phase flip, or K_FOREVER
 * for a steady state (nothing more to do). One-shot patterns end by clearing the
 * colour, so they cost nothing after they finish. */
static void ledx_phase_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_timeout_t next = K_FOREVER;
    k_spinlock_key_t key = k_spin_lock(&ledx_lock);

    switch (ledx_pattern) {
    case LED_PAT_SOLID:
        ledx_phase_on = true;
        break;

    case LED_PAT_BLINK_SLOW: /* 50 ms on every 2 s => ~2.5% duty */
        ledx_phase_on = !ledx_phase_on;
        next = ledx_phase_on ? K_MSEC(50) : K_MSEC(1950);
        break;

    case LED_PAT_BLINK_FAST: /* 100 ms on every 500 ms => ~20% duty */
        ledx_phase_on = !ledx_phase_on;
        next = ledx_phase_on ? K_MSEC(100) : K_MSEC(400);
        break;

    case LED_PAT_DOUBLE: /* blink, blink, long pause */
        ledx_dbl_step = (uint8_t)((ledx_dbl_step + 1) % 4);
        switch (ledx_dbl_step) {
        case 0:
            ledx_phase_on = true;
            next = K_MSEC(60);
            break;
        case 1:
            ledx_phase_on = false;
            next = K_MSEC(120);
            break;
        case 2:
            ledx_phase_on = true;
            next = K_MSEC(60);
            break;
        default:
            ledx_phase_on = false;
            next = K_MSEC(1760);
            break;
        }
        break;

    case LED_PAT_FLASH:      /* one-shot: we are here because the flash expired */
    case LED_PAT_FLASH_LONG: /* fall through */
        if (ledx_flash_deadline != 0 && k_uptime_get() >= ledx_flash_deadline) {
            ledx_phase_on = false;
            ledx_colour = 0; /* done — draw no more current */
            ledx_flash_deadline = 0;
        }
        /* else: a newer flash replaced ours while we were being dispatched. Leave
         * it alone — its own expiry is already scheduled. */
        break;

    default:
        ledx_phase_on = false;
        break;
    }

    refresh();
    k_spin_unlock(&ledx_lock, key);

    if (!K_TIMEOUT_EQ(next, K_FOREVER)) {
        k_work_reschedule(&ledx_phase_work, next);
    }
}

void led_render_set(uint8_t colour_mask, uint8_t pattern) {
    colour_mask &= LED_CH_MASK;
    if (pattern > LED_PAT_MAX) {
        pattern = LED_PAT_SOLID;
    }

    /* Behaviors init at POST_KERNEL but the GPIOs only come up at APPLICATION, so
     * the very first request (the peripheral's "I have no partner" fallback) lands
     * before we can drive anything. Record it anyway and let init paint it —
     * otherwise a half that never connects sits dark, which is precisely the case
     * the fallback exists for. */
    k_work_cancel_delayable(&ledx_phase_work);

    k_spinlock_key_t key = k_spin_lock(&ledx_lock);
    ledx_colour = colour_mask;
    ledx_pattern = pattern;
    ledx_dbl_step = 0;
    ledx_phase_on = (colour_mask != 0);
    ledx_flash_deadline =
        (colour_mask && pattern == LED_PAT_FLASH)        ? k_uptime_get() + 1000
        : (colour_mask && pattern == LED_PAT_FLASH_LONG) ? k_uptime_get() + 1500
                                                         : 0;
    if (ledx_ready) {
        refresh(); /* every pattern starts lit, so the user sees it react at once */
    }
    k_spin_unlock(&ledx_lock, key);

    if (!ledx_ready || colour_mask == 0) {
        return;
    }

    switch (pattern) {
    case LED_PAT_SOLID:
        break; /* nothing to schedule */
    case LED_PAT_FLASH:
        k_work_reschedule(&ledx_phase_work, K_MSEC(1000));
        break;
    case LED_PAT_FLASH_LONG:
        k_work_reschedule(&ledx_phase_work, K_MSEC(1500));
        break;
    case LED_PAT_BLINK_SLOW:
        k_work_reschedule(&ledx_phase_work, K_MSEC(50));
        break;
    case LED_PAT_BLINK_FAST:
        k_work_reschedule(&ledx_phase_work, K_MSEC(100));
        break;
    case LED_PAT_DOUBLE:
        k_work_reschedule(&ledx_phase_work, K_MSEC(60));
        break;
    default:
        break;
    }
}

static int ledx_render_init(void) {
    for (int i = 0; i < CH_COUNT; i++) {
        if (!gpio_is_ready_dt(&ledx_ch[i])) {
            LOG_ERR("led channel %d not ready", i);
            return -ENODEV;
        }
        int rc = gpio_pin_configure_dt(&ledx_ch[i], GPIO_OUTPUT_INACTIVE);
        if (rc) {
            LOG_ERR("led channel %d configure failed (%d)", i, rc);
            return rc;
        }
    }
    k_work_init_delayable(&ledx_mux_work, ledx_mux_handler);
    k_work_init_delayable(&ledx_phase_work, ledx_phase_handler);
    ledx_ready = true;

    /* Replay whatever was asked for before the GPIOs existed (see led_render_set). */
    const uint8_t colour = ledx_colour;
    const uint8_t pattern = ledx_pattern;
    if (colour) {
        ledx_colour = 0; /* force led_render_set to treat this as a fresh request */
        led_render_set(colour, pattern);
    } else {
        drive_static(0);
    }
    return 0;
}

SYS_INIT(ledx_render_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else /* no LED nodes on this build */

void led_render_set(uint8_t colour_mask, uint8_t pattern) {
    ARG_UNUSED(colour_mask);
    ARG_UNUSED(pattern);
}

#endif
