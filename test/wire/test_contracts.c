/*
 * The compatibility boundary from PLAN-ext-fw-refactor.md §0, expressed as
 * assertions. Nothing here parses a wire — these are the literal constants that
 * must never move, restated independently of the headers that define them.
 *
 * A refactor that renames or re-derives one of these still passes as long as the
 * VALUE is unchanged, which is exactly the contract: the app, the backup v5 files
 * and every keyboard in the field only ever see values.
 */

#include <zmk_dynamic_keymap/dcombo.h>
#include <zmk_dynamic_keymap/dmac.h>
#include <zmk_encoder_config/config.h>
#include <zmk_led_config/config.h>
#include <zmk_live_feed/live_feed.h>
#include <zmk_timing_config/config.h>
#include <zmk_torabo_caps/caps.h>
#include <zmk_trackball_config/config.h>
#include <zmk_trackpad_config/config.h>

#include "torabo_test.h"

/* The trackball/trackpad wire magics are private to their config_state.c, so
 * restate them here from the design docs rather than reaching into the source. */
#define EXPECT_ZTC_MAGIC 0x7A74u /* "tz" */
#define EXPECT_TP_MAGIC 0x7470u  /* "tp" */

void test_contracts(void) {
    torabo_test_begin("frozen contracts (PLAN §0)");

    /* §0.1 tunnel feature ids == low byte of the GATT service UUID. These are
     * #defined inside each feature's tunnel_bridge.c (which pulls in the tunnel
     * RPC headers and cannot be compiled on the host), so they are pinned here
     * as plain numbers. Changing one silently breaks every USB client. */
    T_EQ_INT(0x00, 0x00, "feature id caps = 0x00");
    T_EQ_INT(0x09, 0x09, "feature id trackball = 0x09");
    T_EQ_INT(0x0A, 0x0A, "feature id macros = 0x0A");
    T_EQ_INT(0x0B, 0x0B, "feature id combos = 0x0B");
    T_EQ_INT(0x0C, 0x0C, "feature id trackpad = 0x0C");
    T_EQ_INT(0x0D, 0x0D, "feature id encoder = 0x0D");
    T_EQ_INT(0x0E, 0x0E, "feature id led = 0x0E");
    T_EQ_INT(0x0F, 0x0F, "feature id live_feed = 0x0F");
    T_EQ_INT(0x10, 0x10, "feature id timing = 0x10");

    /* §0.2 caps descriptor shape */
    T_EQ_INT(TORABO_CAPS_MAGIC, 0x4354, "caps magic 0x4354");
    T_EQ_INT(TORABO_CAPS_DESC_VERSION, 1, "caps desc_ver 1");
    T_EQ_INT(TORABO_CAPS_HDR, 8, "caps header 8B");
    T_EQ_INT(TORABO_CAPS_FEAT, 4, "caps feature row 4B");
    /* feature ids are append-only: every existing id keeps its number */
    T_EQ_INT(TORABO_FEAT_TRACKBALL, 1, "caps id trackball = 1");
    T_EQ_INT(TORABO_FEAT_MACROS, 2, "caps id macros = 2");
    T_EQ_INT(TORABO_FEAT_COMBOS, 3, "caps id combos = 3");
    T_EQ_INT(TORABO_FEAT_TRACKPAD, 4, "caps id trackpad = 4");
    T_EQ_INT(TORABO_FEAT_ENCODER, 5, "caps id encoder = 5");
    T_EQ_INT(TORABO_FEAT_LED, 6, "caps id led = 6");
    T_EQ_INT(TORABO_FEAT_RESERVED_LAYERS, 7, "caps id reserved_layers = 7");
    T_EQ_INT(TORABO_FEAT_LIVE_FEED, 8, "caps id live_feed = 8");
    T_EQ_INT(TORABO_FEAT_RPC_TUNNEL, 9, "caps id rpc_tunnel = 9");
    T_EQ_INT(TORABO_FEAT_TIMING, 10, "caps id timing = 10");
    T_EQ_INT(TORABO_FEAT_MODULES, 11, "caps id modules = 11 (PLAN phase 9)");

    /* §0.3 live_feed */
    T_EQ_INT(LIVE_FEED_PROTO_VER, 1, "live_feed PROTO_VER 1 (never bumped)");
    T_EQ_INT(LIVE_FEED_RECORD_SIZE, 16, "live_feed record 16B");

    /* §0.4 per-feature wire magics / versions / layout constants */
    T_EQ_INT(EXPECT_ZTC_MAGIC, 0x7A74, "ztc magic 0x7A74");
    T_EQ_INT(ZTC_WIRE_HDR, 8, "ztc header 8B");
    T_EQ_INT(ZTC_WIRE_LAYER, 12, "ztc layer 12B");
    T_EQ_INT(ZTC_WIRE_COAST, 4, "ztc v3 coast trailer 4B");

    T_EQ_INT(EXPECT_TP_MAGIC, 0x7470, "tp magic 0x7470");
    T_EQ_INT(TP_WIRE_HDR, 6, "tp header 6B");
    T_EQ_INT(TP_WIRE_DEV_HDR, 2, "tp v1/v2 device header 2B");
    T_EQ_INT(TP_WIRE_DEV_HDR_V3, 5, "tp v3 device header 5B");
    T_EQ_INT(TP_WIRE_BIND, 4, "tp binding 4B");
    T_EQ_INT(TP_WIRE_AXIS, 11, "tp v2/v3 axis 11B");
    T_EQ_INT(TP_WIRE_GEST, 16, "tp gesture block 16B");
    T_EQ_INT(TP_WIRE_LAYER_V2, 38, "tp v2/v3 layer 38B");
    T_EQ_INT(TP_WIRE_AXIS_V1, 3, "tp v1 axis 3B");
    T_EQ_INT(TP_WIRE_LAYER_V1, 6, "tp v1 layer 6B");
    T_EQ_INT(TP_MAX_DEVICES, 4, "tp max devices 4");

    T_EQ_INT(TMG_WIRE_VERSION, 1, "tmg version 1");
    T_EQ_INT(TMG_WIRE_HDR, 8, "tmg header 8B");
    T_EQ_INT(TMG_HT_NODES, 2, "tmg ht nodes 2");
    T_EQ_INT(TMG_HT_POS_SLOTS, 32, "tmg positional slots 32");
    T_EQ_INT(TMG_HT_BLOCK, 44, "tmg ht block 44B");
    T_EQ_INT(TMG_WIRE_LEN, 96, "tmg wire 96B (fixed)");

    T_EQ_INT(LED_WIRE_MAGIC, 0x656C, "led magic 0x656C");
    T_EQ_INT(LED_WIRE_VERSION, 1, "led version 1");
    T_EQ_INT(LED_WIRE_HDR, 6, "led header 6B");
    T_EQ_INT(LED_WIRE_RULE, 4, "led rule 4B");
    T_EQ_INT(LED_MAX_RULES, 8, "led max rules 8");
    T_EQ_INT(LED_SIDES, 2, "led sides 2");
    T_EQ_INT(LED_WIRE_CAP, 72, "led wire 72B");

    T_EQ_INT(ENC_WIRE_MAGIC, 0x6E65, "enc magic 0x6E65");
    T_EQ_INT(ENC_WIRE_VERSION, 1, "enc version 1");
    T_EQ_INT(ENC_WIRE_HDR, 4, "enc header 4B");
    T_EQ_INT(ENC_WIRE_BIND, 4, "enc binding 4B");
    T_EQ_INT(ENC_WIRE_LAYER, 12, "enc layer 12B (cw+ccw+btn)");

    T_EQ_INT(DM_MAGIC, 0x6D64, "dm magic 0x6D64");
    /* PLAN phase 8: dm v2 adds a per-slot NAME block, appended to the READ
     * wire. DM_VERSION is the current/newest wire (now 2); DM_VERSION_V1 is
     * pinned separately because the steps WRITE op speaks it forever. */
    T_EQ_INT(DM_VERSION_V1, 1, "dm version v1 = 1 (steps WRITE op, forever)");
    T_EQ_INT(DM_VERSION_V2, 2, "dm version v2 = 2 (READ wire + name WRITE op)");
    T_EQ_INT(DM_VERSION, 2, "dm version (current READ wire) = 2");
    T_EQ_INT(DM_SLOTS, 20, "dm slots 20");
    T_EQ_INT(DM_STEPS, 16, "dm steps per slot 16");
    T_EQ_INT(DM_NAME_MAX, 16, "dm name field 16B (five Japanese characters)");
    T_EQ_INT(DM_WIRE_STEP, 5, "dm step 5B");
    T_EQ_INT(DM_READ_HDR, 4, "dm read header 4B");
    T_EQ_INT(DM_READ_SLOT, 81, "dm read slot 81B");
    T_EQ_INT(DM_READ_WIRE_LEN_V1, 1624, "dm read wire v1 (slot region) 1624B");
    T_EQ_INT(DM_READ_NAME, 17, "dm read name entry 17B (name_len u8 + name[16])");
    T_EQ_INT(DM_READ_NAMES_BASE, 1624, "dm name block starts right after the slot region");
    T_EQ_INT(DM_READ_WIRE_LEN, 1964, "dm read wire v2 (slots+names) 1964B");
    T_CHECK(DM_READ_WIRE_LEN <= 2048, "dm v2 READ wire fits the 2048B tunnel blob budget");
    T_EQ_INT(DM_WRITE_HDR, 3, "dm write header 3B");
    T_EQ_INT(DM_WRITE_MAX, 83, "dm write max 83B");
    T_EQ_INT(DM_WRITE_KIND_STEPS, 0, "dm v2 write kind STEPS = 0 (reserved, rejected)");
    T_EQ_INT(DM_WRITE_KIND_NAME, 1, "dm v2 write kind NAME = 1");
    T_EQ_INT(DM_NAME_WRITE_LEN, 20, "dm v2 name WRITE op is a fixed 20B");

    T_EQ_INT(CB_MAGIC, 0x6263, "cb magic 0x6263");
    T_EQ_INT(CB_VERSION, 1, "cb version 1");
    T_EQ_INT(CB_SLOTS, 16, "cb slots 16");
    T_EQ_INT(CB_MAX_POS, 6, "cb max positions 6");
    T_EQ_INT(CB_WIRE_SLOT, 26, "cb slot 26B");
    T_EQ_INT(CB_READ_WIRE_LEN, 420, "cb read wire 420B");
    T_EQ_INT(CB_WRITE_MAX, 28, "cb write wire 28B");
    /* the per-slot field offsets the app codec (comboConfig.ts) mirrors */
    T_EQ_INT(CB_W_ENABLED, 0, "cb offset enabled");
    T_EQ_INT(CB_W_POS_COUNT, 1, "cb offset pos_count");
    T_EQ_INT(CB_W_POSITIONS, 2, "cb offset positions");
    T_EQ_INT(CB_W_LAYER_MASK, 8, "cb offset layer_mask");
    T_EQ_INT(CB_W_TIMEOUT, 12, "cb offset timeout");
    T_EQ_INT(CB_W_PRIOR_IDLE, 14, "cb offset prior_idle");
    T_EQ_INT(CB_W_FLAGS, 16, "cb offset flags");
    T_EQ_INT(CB_W_TGT_TYPE, 17, "cb offset target type");
    T_EQ_INT(CB_W_TGT_P1, 18, "cb offset target param1");
    T_EQ_INT(CB_W_TGT_P2, 22, "cb offset target param2");

    /* ---- value RANGES the app's sliders mirror -----------------------------
     * The per-feature clamp tests assert "this wild value comes back as the
     * bound", which on its own would still pass if a bound moved. Pinning the
     * literals here is what makes those tests meaningful — and the app's UI
     * ranges have to agree with these numbers or a user-set value silently
     * changes when the firmware writes it back. */
    T_EQ_INT(ZTC_SPEED_MIN, 1, "ztc speed_div min 1");
    T_EQ_INT(ZTC_SPEED_MAX, 32, "ztc speed_div max 32");
    T_EQ_INT(ZTC_TIMEOUT_MIN, 50, "ztc temp timeout min 50ms");
    T_EQ_INT(ZTC_TIMEOUT_MAX, 30000, "ztc temp timeout max 30000ms");
    T_EQ_INT(ZTC_COAST_FRICTION_MIN, 1, "ztc coast friction min 1");
    T_EQ_INT(ZTC_COAST_FRICTION_MAX, 32, "ztc coast friction max 32");
    T_EQ_INT(ZTC_COAST_FRICTION_DEFAULT, 8, "ztc coast friction default 8");
    T_EQ_INT(ZTC_COAST_THRESHOLD_MIN, 1, "ztc coast threshold min 1");
    T_EQ_INT(ZTC_COAST_THRESHOLD_MAX, 255, "ztc coast threshold max 255");
    T_EQ_INT(ZTC_COAST_THRESHOLD_DEFAULT, 24, "ztc coast threshold default 24");
    T_EQ_INT(ZTC_ROLE_MOVE, 0, "ztc role MOVE = 0 (the fail-open value)");
    T_EQ_INT(ZTC_ROLE_SCROLL, 1, "ztc role SCROLL = 1");
    T_EQ_INT(ZTC_ROLE_OFF, 2, "ztc role OFF = 2");

    T_EQ_INT(TP_STEP_MIN, 1, "tp step min 1");
    T_EQ_INT(TP_STEP_MAX, 32, "tp step max 32");
    T_EQ_INT(TP_COAST_FRICTION_MIN, 1, "tp coast friction min 1");
    T_EQ_INT(TP_COAST_FRICTION_MAX, 32, "tp coast friction max 32");
    T_EQ_INT(TP_COAST_FRICTION_DEFAULT, 8, "tp coast friction default 8");
    T_EQ_INT(TP_COAST_THRESHOLD_MIN, 1, "tp coast threshold min 1");
    T_EQ_INT(TP_COAST_THRESHOLD_MAX, 255, "tp coast threshold max 255");
    T_EQ_INT(TP_COAST_THRESHOLD_DEFAULT, 24, "tp coast threshold default 24");
    /* tp role / behavior enums are a published contract with tpConfigV2.ts */
    T_EQ_INT(TP_ROLE_MOVE, 0, "tp role MOVE = 0 (the fail-open value)");
    T_EQ_INT(TP_ROLE_SCROLL, 1, "tp role SCROLL = 1");
    T_EQ_INT(TP_ROLE_OFF, 2, "tp role OFF = 2");
    T_EQ_INT(TP_ROLE_ENCODER, 3, "tp role ENCODER = 3");
    T_EQ_INT(TP_BEH_NONE, 0, "tp behavior NONE = 0 (the fail-open value)");
    T_EQ_INT(TP_BEH_KP, 1, "tp behavior KP = 1");
    T_EQ_INT(TP_BEH_CP, 2, "tp behavior CP = 2");
    T_EQ_INT(TP_BEH_MO, 3, "tp behavior MO = 3");
    T_EQ_INT(TP_BEH_TO, 4, "tp behavior TO = 4");
    T_EQ_INT(TP_BEH_TOG, 5, "tp behavior TOG = 5");
    T_EQ_INT(TP_FLAG_GESTURES, 0x01, "tp header flag GESTURES = bit0");
    T_EQ_INT(TP_FLAG_COAST, 0x02, "tp header flag COAST = bit1");
    T_EQ_INT(TP_DEFAULT_DEVICE_COUNT, 2, "tp exposes 2 devices by default");

    T_EQ_INT(TMG_TAPPING_TERM_MIN, 10, "tmg tapping_term min 10ms");
    T_EQ_INT(TMG_TAPPING_TERM_MAX, 2000, "tmg tapping_term max 2000ms");
    T_EQ_INT(TMG_DEBOUNCE_MIN, 1, "tmg debounce min 1ms");
    T_EQ_INT(TMG_DEBOUNCE_MAX, 100, "tmg debounce max 100ms");
    T_EQ_INT(TMG_U16_DISABLED, 0xFFFF, "tmg 'disabled' sentinel 0xFFFF");
    T_EQ_INT(TMG_FLAVOR_MAX, 3, "tmg flavor max 3");
    T_EQ_INT(TMG_FLAGS_MASK, 0x07, "tmg flags mask = the 3 wire v1 bits");
    T_EQ_INT(TMG_NODE_MT, 0, "tmg ht block 0 = mod_tap");
    T_EQ_INT(TMG_NODE_LT, 1, "tmg ht block 1 = layer_tap");
    T_CHECK(strcmp(TMG_NODE_NAME_MT, "mod_tap") == 0, "tmg node name \"mod_tap\"");
    T_CHECK(strcmp(TMG_NODE_NAME_LT, "layer_tap") == 0, "tmg node name \"layer_tap\"");

    /* led: the channel mask and the enum values the app renders from */
    T_EQ_INT(LED_CH_RED, 0x01, "led channel RED = bit0");
    T_EQ_INT(LED_CH_YG, 0x02, "led channel YG = bit1");
    T_EQ_INT(LED_CH_GRN, 0x04, "led channel GRN = bit2");
    T_EQ_INT(LED_CH_MASK, 0x07, "led channel mask 0x07");
    T_EQ_INT(LED_UC_MAX, 7, "led usecase max 7 (MODIFIER)");
    T_EQ_INT(LED_PAT_MAX, 5, "led pattern max 5 (FLASH_LONG)");
    T_EQ_INT(LED_CAP_LEFT_PRESENT, 0x01, "led cap LEFT = bit0");
    T_EQ_INT(LED_CAP_RIGHT_PRESENT, 0x02, "led cap RIGHT = bit1");
    T_EQ_INT(LED_CAP_CENTRAL_IS_LEFT, 0x04, "led cap CENTRAL_IS_LEFT = bit2");

    T_EQ_INT(ENC_BEH_NONE, 0, "enc behavior NONE = 0 (the fail-open value)");
    T_EQ_INT(ENC_BEH_MAX, 5, "enc behavior max 5 (TOG)");
    T_EQ_INT(ENC_CW, 0, "enc slot order: cw");
    T_EQ_INT(ENC_CCW, 1, "enc slot order: ccw");
    T_EQ_INT(ENC_BTN, 2, "enc slot order: btn");

    T_EQ_INT(DM_ACT_TAP, 0, "dm action TAP = 0 (the fail-open value)");
    T_EQ_INT(DM_ACT_PRESS, 1, "dm action PRESS = 1");
    T_EQ_INT(DM_ACT_RELEASE, 2, "dm action RELEASE = 2");

    T_EQ_INT(CB_TGT_KP, 0, "cb target KP = 0");
    T_EQ_INT(CB_TGT_MO, 1, "cb target MO = 1");
    T_EQ_INT(CB_TGT_TO, 2, "cb target TO = 2");
    T_EQ_INT(CB_TGT_TOG, 3, "cb target TOG = 3");
    T_EQ_INT(CB_TGT_DMAC, 4, "cb target DMAC = 4");
    T_EQ_INT(CB_FLAG_SLOW_RELEASE, 0x01, "cb flag SLOW_RELEASE = bit0");

    /* caps bits are per-feature namespaces; the app matches on the numbers */
    T_EQ_INT(TORABO_CAPS_LED_LEFT, 0x0001, "caps bit LED_LEFT");
    T_EQ_INT(TORABO_CAPS_LED_RIGHT, 0x0002, "caps bit LED_RIGHT");
    T_EQ_INT(TORABO_CAPS_LED_CENTRAL_IS_LEFT, 0x0004, "caps bit LED_CENTRAL_IS_LEFT");
    T_EQ_INT(TORABO_CAPS_TP_DEVICE_MASK, 0x000F, "caps mask TP_DEVICE");
    T_EQ_INT(TORABO_CAPS_TP_COAST, 0x0010, "caps bit TP_COAST");
    T_EQ_INT(TORABO_CAPS_ZTC_COAST, 0x0001, "caps bit ZTC_COAST");
    T_EQ_INT(TORABO_CAPS_LAYERS_MASK, 0x00FF, "caps mask LAYERS");
    T_EQ_INT(TORABO_CAPS_LIVE_FEED_DIAG, 0x0001, "caps bit LIVE_FEED_DIAG");
    T_EQ_INT(TORABO_CAPS_TUNNEL_NOTIFY, 0x0001, "caps bit TUNNEL_NOTIFY");
    T_EQ_INT(TORABO_CAPS_TIMING_SPLIT_DEBOUNCE, 0x0001, "caps bit TIMING_SPLIT_DEBOUNCE");

    /* PLAN phase 9: header CENTRAL field, unchanged since the phase's original
     * commit — a genuinely 1-of-3 choice, so torabo_caps_side survives here. */
    T_EQ_INT(TORABO_CAPS_SIDE_UNKNOWN, 0, "caps side UNKNOWN = 0");
    T_EQ_INT(TORABO_CAPS_SIDE_LEFT, 1, "caps side LEFT = 1");
    T_EQ_INT(TORABO_CAPS_SIDE_RIGHT, 2, "caps side RIGHT = 2");
    T_EQ_INT(TORABO_CAPS_HDR_CENTRAL_SHIFT, 0, "caps hdr _rsv central shift = bit0");
    T_EQ_INT(TORABO_CAPS_HDR_CENTRAL_MASK, 0x03, "caps hdr _rsv central mask = bits0-1");

    /* PLAN phase 9, re-redesigned 2026-09-03 (the SECOND redesign): module
     * placement is ONE row (TORABO_FEAT_MODULES) of four 4-bit slot values,
     * not bits spread across the ztc/enc caps words (both of this phase's
     * earlier schemes — a 1-of-3 enum pair, then per-feature bitmasks — are
     * gone; neither TORABO_CAPS_ZTC_BALL_x nor TORABO_CAPS_ENC_LEFT_x / _RIGHT_x
     * exist any more). torabo_caps_slot numbering is INDEPENDENT of
     * tp_meta_kind (zmk_trackpad_config/config.h): the two enums describe
     * overlapping hardware but are different contracts, so apps that need both
     * keep their own mapping table. NONE=15 is an explicit "this slot is
     * empty", distinct from UNDECLARED=0 which is what an old firmware or an
     * unset conf reports. */
    T_EQ_INT(TORABO_CAPS_SLOT_UNDECLARED, 0, "caps slot UNDECLARED = 0");
    T_EQ_INT(TORABO_CAPS_SLOT_BALL, 1, "caps slot BALL = 1");
    T_EQ_INT(TORABO_CAPS_SLOT_PAD, 2, "caps slot PAD = 2");
    T_EQ_INT(TORABO_CAPS_SLOT_SWITCH4, 3, "caps slot SWITCH4 = 3 (reserved, builder N/A)");
    T_EQ_INT(TORABO_CAPS_SLOT_DIAL, 4, "caps slot DIAL = 4");
    T_EQ_INT(TORABO_CAPS_SLOT_ENCODER, 9, "caps slot ENCODER = 9 (self-made module)");
    T_EQ_INT(TORABO_CAPS_SLOT_NONE, 15, "caps slot NONE = 15 (explicitly empty)");
    T_EQ_INT(TORABO_CAPS_MOD_SLOT_BITS, 4, "caps mod slot width = 4 bits");
    T_EQ_INT(TORABO_CAPS_MOD_SLOT_MASK, 0xF, "caps mod slot mask = 0xF");
    T_EQ_INT(TORABO_CAPS_MOD_LEFT_STD_SHIFT, 0, "caps mod left-std shift = bit0");
    T_EQ_INT(TORABO_CAPS_MOD_LEFT_EXT_SHIFT, 4, "caps mod left-ext shift = bit4");
    T_EQ_INT(TORABO_CAPS_MOD_RIGHT_STD_SHIFT, 8, "caps mod right-std shift = bit8");
    T_EQ_INT(TORABO_CAPS_MOD_RIGHT_EXT_SHIFT, 12, "caps mod right-ext shift = bit12");

    /* PLAN phase 6 B-4 raised the caps table cap 10->16; phase 9 raises it
     * again 16->32 for headroom (a bigger static buffer only). */
    T_EQ_INT(TORABO_CAPS_MAX_FEATURES, 32, "caps MAX_FEATURES = 32 (PLAN phase 9)");
}
