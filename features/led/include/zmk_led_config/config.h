/*
 * Runtime-configurable extender LED (torabo-tsuki).
 *
 * Replaces the hardcoded status_led_ext behaviour (BLE profile flash + steady red
 * on split disconnect) with a rule table the app edits live: per side, a list of
 * "when X happens, show colour C with pattern P", arbitrated by priority.
 *
 * ---------------------------------------------------------------------------
 * Architecture — the peripheral is a DUMB DISPLAY.
 *
 * The central already knows everything worth showing, including things you might
 * assume are peripheral-local:
 *   - the peripheral's battery (split battery-level fetching/proxy),
 *   - Caps/Num/Scroll (it owns the HID report).
 * So the central holds the whole rule table, decides what BOTH LEDs should show,
 * drives its own directly, and pushes the peripheral's rendered state over the
 * split with zmk_split_central_invoke_behavior(). The peripheral keeps no config
 * and needs no sync — it just renders what it is told.
 *
 * The ONE exception: when the split link drops, the central cannot tell the
 * peripheral anything. So "I lost my partner" (LED_UC_LINK_LOST) is also
 * evaluated locally on the peripheral, as a fallback while it is on its own.
 * ---------------------------------------------------------------------------
 *
 * Deliberately standalone (own GATT, own wire): status_led_ext keeps working
 * untouched, so this can't regress a keyboard already in the field.
 */

#pragma once

#include <zephyr/types.h>

/* ---- LED hardware ---------------------------------------------------------
 * Three cathode-driven channels on a common anode that rides the extender pad's
 * power rail. No PWM: brightness is not adjustable, only WHICH channels are on
 * and for HOW LONG. Two or more channels at once are time-multiplexed by the
 * renderer, so a mixed colour costs about the same current as a single one. */
#define LED_CH_RED 0x01
#define LED_CH_YG 0x02  /* yellow-green */
#define LED_CH_GRN 0x04 /* "green" (datasheet labels this element Y) */
#define LED_CH_MASK 0x07

/* The 7 lightable combinations. Which of these a human can actually tell apart
 * needs checking on the real board — see docs/DESIGN-status-led-usecases.md. */
#define LED_COLOUR_COUNT 7

/* ---- patterns -------------------------------------------------------------
 * The only lever we have on battery drain is duty cycle, so the pattern is also
 * the power knob: blinking is both a distinct signal AND cheaper than solid. */
enum led_pattern {
    LED_PAT_SOLID = 0,      /* on while the condition holds — most expensive */
    LED_PAT_BLINK_SLOW = 1, /* ~50 ms every 2 s => ~2.5% duty */
    LED_PAT_BLINK_FAST = 2, /* ~100 ms every 500 ms => ~20% duty */
    LED_PAT_DOUBLE = 3,     /* two quick blinks, then a pause */
    LED_PAT_FLASH = 4,      /* one-shot ~1 s, then off (for "something changed") */
    LED_PAT_FLASH_LONG = 5, /* one-shot ~1.5 s (what the profile display uses today) */
};
#define LED_PAT_MAX LED_PAT_FLASH_LONG

/* ---- use cases ------------------------------------------------------------
 * The rule's trigger. Grouped by WHEN it lights, because that is what decides
 * battery cost (see the design doc): warnings > notices > steady states. */
enum led_usecase {
    LED_UC_NONE = 0,

    /* --- warnings (rare, important) --- */
    LED_UC_LINK_LOST = 1,   /* lost the other half. Also evaluated peripheral-locally. */
    LED_UC_BATTERY_LOW = 2, /* this side's own cell below the rule's threshold */

    /* --- notices (fire on a change, then go dark) --- */
    LED_UC_PROFILE_CHANGED = 3, /* BLE profile switched (today's behaviour) */
    LED_UC_LAYER_CHANGED = 4,   /* highest active layer changed */
    LED_UC_ENDPOINT_CHANGED = 5,/* USB <-> BLE */

    /* --- steady states (on while true — watch the battery) --- */
    LED_UC_CAPS_LOCK = 6,  /* Caps Lock is on */
    LED_UC_MODIFIER = 7,   /* a modifier is held; rule->param picks which (LED_MOD_*) */
};
#define LED_UC_MAX LED_UC_MODIFIER

/* rule->param for LED_UC_MODIFIER. Matches ZMK's MOD_L* bit order. Assigning one
 * modifier per channel makes chords mix naturally (Ctrl+Shift => red+green). */
#define LED_MOD_CTL 0x01
#define LED_MOD_SFT 0x02
#define LED_MOD_ALT 0x04
#define LED_MOD_GUI 0x08

/* colour == LED_COLOUR_AUTO on an index-valued use case (profile / layer) means
 * "derive the colour from the index", i.e. profile 0, 1, 2... each get their own.
 * A fixed colour there would make every profile look the same. */
#define LED_COLOUR_AUTO 0

struct led_rule {
    uint8_t usecase; /* enum led_usecase */
    uint8_t colour;  /* LED_CH_* mask, or LED_COLOUR_AUTO */
    uint8_t pattern; /* enum led_pattern */
    uint8_t param;   /* MODIFIER: which mod. BATTERY_LOW: percent threshold. else 0 */
};

/* Rules are tried in order; the first whose condition holds wins. So put warnings
 * first and steady states last — the app owns that ordering. */
#define LED_MAX_RULES 8
#define LED_SIDES 2
#define LED_SIDE_LEFT 0
#define LED_SIDE_RIGHT 1

struct led_side_cfg {
    uint8_t rule_count;
    struct led_rule rules[LED_MAX_RULES];
};

struct led_snapshot {
    struct led_side_cfg sides[LED_SIDES];
};

/* ---- capabilities ---------------------------------------------------------
 * FW-authoritative, READ-ONLY for the app: it renders only what this build can
 * actually do, instead of assuming a layout. A firmware without an LED on a side
 * simply doesn't advertise it and the app hides that side. Old firmware reports
 * 0 and the app says "not available" rather than guessing.
 *
 * Comes from Kconfig, which the firmware builder fills in per hardware pattern. */
#define LED_CAP_LEFT_PRESENT 0x01  /* an LED exists on the left half */
#define LED_CAP_RIGHT_PRESENT 0x02 /* an LED exists on the right half */
#define LED_CAP_CENTRAL_IS_LEFT 0x04 /* which half is central (affects nothing else) */

/* ---- wire (LE, versioned) -------------------------------------------------
 *   header: magic u16 | version u8 | caps u8 | rule_max u8 | _rsv u8
 *   per side (LED_SIDES, left then right): rule_count u8, then rules
 *   rule: usecase u8 | colour u8 | pattern u8 | param u8
 *
 * 6 + 2 * (1 + 8*4) = 72 B — one ATT write, no Write Long. */
#define LED_WIRE_MAGIC 0x656C /* "le" little-endian */
#define LED_WIRE_VERSION 1
#define LED_WIRE_HDR 6
#define LED_WIRE_RULE 4
#define LED_WIRE_SIDE (1 + LED_MAX_RULES * LED_WIRE_RULE)
#define LED_WIRE_CAP (LED_WIRE_HDR + LED_SIDES * LED_WIRE_SIDE)

/* ---- API ------------------------------------------------------------------ */

const struct led_snapshot *led_live(void);
int led_apply_wire(const uint8_t *buf, uint16_t len);
int led_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len);
int led_save(void);

/* Re-evaluate the rules and repaint both LEDs. Called whenever the table changes
 * (a GATT write, an NVS load) — without it the LEDs keep showing decisions made
 * from the OLD table until some unrelated event happens to come along, and the app
 * that just said "applied instantly" would be lying. No-op off the central. */
void led_repaint(void);

/* Renderer (both halves): show this colour with this pattern until told otherwise.
 * colour == 0 turns the LED off. Safe to call from any thread. */
void led_render_set(uint8_t colour_mask, uint8_t pattern);

/* Pack/unpack the render command carried in a behavior param across the split. */
static inline uint32_t led_render_encode(uint8_t colour_mask, uint8_t pattern) {
    return ((uint32_t)(colour_mask & LED_CH_MASK)) | ((uint32_t)(pattern & 0xff) << 8);
}
static inline void led_render_decode(uint32_t v, uint8_t *colour_mask, uint8_t *pattern) {
    *colour_mask = (uint8_t)(v & LED_CH_MASK);
    *pattern = (uint8_t)((v >> 8) & 0xff);
}
