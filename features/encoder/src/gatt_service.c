/*
 * BLE settings window for the encoder store — its own service, deliberately not
 * sharing the trackpad's, so a bug here cannot disturb a wire that is already in
 * the field.
 *
 * The wire grows with the keymap (4 + 12*ZMK_KEYMAP_LAYERS_LEN), so like the
 * trackpad, timing and trackball windows this one reassembles chunked writes
 * rather than rejecting offset != 0 — see the note above enc_asm below.
 *
 * UUIDs must match zmk-studio's transport table (src-tauri .../transport/).
 * Allocated so far: trackball e1f4a900, macros e1f4aa00, combos e1f4ab00,
 * trackpad e1f4ac00, encoder e1f4ad00 (this one).
 */

#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZMK_ENCODER_CONFIG_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include <zmk_encoder_config/config.h>

LOG_MODULE_DECLARE(enc_config, CONFIG_ZMK_ENCODER_CONFIG_LOG_LEVEL);

/* After LOG_MODULE_DECLARE above: the assembler's LOG_WRN calls bind to this
 * file's log module. */
#include <torabo_common/wire_asm.h>
#include <torabo_common/window_read.h>
#include <torabo_common/window_read_gatt.h>

#define ENC_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4ad00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define ENC_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4ad01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 enc_svc_uuid = BT_UUID_INIT_128(ENC_BT_UUID_SVC);
static struct bt_uuid_128 enc_cfg_uuid = BT_UUID_INIT_128(ENC_BT_UUID_CFG);

/* Client-driven windowed READ (torabo_common/window_read.h, 2026-09-05). The
 * encoder wire stays inside Android's 512 B read ceiling, so the window is not
 * needed here in practice; it is carried anyway so all seven settings
 * characteristics answer the control frame identically. Zero = disarmed = whole
 * blob, as always. */
static struct torabo_window_read enc_window;

static ssize_t enc_read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    /* Static (not on the BLE-callback stack), as this feature's READ always has:
     * GATT callbacks are serialised on the BT RX thread, so one shared buffer is
     * safe. A Read Long re-enters this for each offset; we re-encode each time
     * (cheap, and always reflects the current live snapshot).
     *
     * TORABO_WINDOW_READ_HDR spare bytes IN FRONT of the wire let a windowed
     * response be stamped in place rather than copied elsewhere. */
    static uint8_t scratch[TORABO_WINDOW_READ_HDR + ENC_WIRE_CAP];
    uint8_t *wire = &scratch[TORABO_WINDOW_READ_HDR];
    uint16_t wlen = 0;
    if (enc_encode_wire(wire, (uint16_t)ENC_WIRE_CAP, &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    if (enc_window.armed) {
        return torabo_window_read_gatt_serve(&enc_window, conn, attr, buf, len, offset, scratch,
                                             wlen);
    }
    /* bt_gatt_attr_read handles Read Blob / offset slicing for us. */
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

/* Wire reassembly for BOTH write transports, exactly as trackpad and timing do
 * it (torabo_common/wire_asm.h; DESIGN-trackpad-v2.md §4.5 for the rationale).
 *
 * WHY THE ENCODER NEEDS IT (2026-09-05, alongside the trackball fix): this wire
 * grows with the keymap. ENC_WIRE_CAP = 4 + 12*ZMK_KEYMAP_LAYERS_LEN, which at
 * the field build's 20 layers (10 keymap + -DTORABO_RESERVED_LAYERS=10) is
 * exactly 244 B — the largest payload a single ATT Write can carry on a 247-byte
 * MTU. One more layer and every host has to split the write, either into a
 * proper ATT Write Long at rising offsets or, on WinRT, into a run of ordinary
 * Write Requests that ALL carry offset 0. The old simple handler rejected
 * offset != 0 outright; sitting exactly on the limit is not a margin worth
 * keeping, and the trackball has already fallen off the same edge.
 *
 * The assembler only FRAMES. All validation stays in enc_apply_wire
 * (magic/version/layer_count/length, atomic publish), which sees the completed
 * blob before anything is applied. Framing uses enc_expected_len()
 * (config_state.c, declared in config.h) rather than a second copy of the length
 * arithmetic — it is the same function enc_apply_wire uses for its own length
 * check, so the two can never disagree about where a wire ends. */
static uint8_t enc_asm_buf[ENC_WIRE_CAP];

static struct torabo_wire_asm enc_asm = {
    .buf = enc_asm_buf,
    .cap = sizeof(enc_asm_buf),
    .hdr_len = ENC_WIRE_HDR,
    .expected_len = enc_expected_len,
    .apply = enc_apply_wire,
    .save = enc_save,
    .tag = "enc",
};

static ssize_t enc_write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    /* A 4-byte [0xFF]['W'][offset] frame arms the next READ instead of being a
     * settings write: enc is magic 0x6E65 LE => buf[0] == 0x65, never 0xFF. Note
     * ENC_WIRE_HDR is itself 4, so this check must come FIRST — the assembler
     * would otherwise stage the frame as a (rejected) header. Not armed while a
     * chunked transfer is staged — see the trackpad service for why. */
    if (!torabo_wire_asm_assembling(&enc_asm, k_uptime_get()) &&
        torabo_window_read_gatt_arm(&enc_window, buf, len, offset, flags)) {
        return len;
    }

    switch (torabo_wire_asm_feed(&enc_asm, (const uint8_t *)buf, len, offset,
                                 (flags & BT_GATT_WRITE_FLAG_PREPARE) != 0, k_uptime_get())) {
    case TORABO_WIRE_ASM_ACCEPTED:
        return len;
    case TORABO_WIRE_ASM_REJECT_LEN:
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    case TORABO_WIRE_ASM_REJECT_OFFSET:
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    default:
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
}

/* COMPATIBILITY (docs/COMPATIBILITY.md §8): still exactly the two entries
 * TORABO_GATT_SIMPLE_SERVICE_DEFINE used to expand to, in the same order and
 * with the same properties — [0] primary service, [1] characteristic. Only
 * BT_GATT_PERM_PREPARE_WRITE is added, which is a permission bit on an existing
 * attribute, not an attribute. Handle order is unchanged. */
/* clang-format off */
BT_GATT_SERVICE_DEFINE(enc_svc,
    BT_GATT_PRIMARY_SERVICE(&enc_svc_uuid),
    BT_GATT_CHARACTERISTIC(&enc_cfg_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT |
                               BT_GATT_PERM_PREPARE_WRITE,
                           enc_read_cfg, enc_write_cfg, NULL),
);
/* clang-format on */

#endif /* CONFIG_ZMK_ENCODER_CONFIG_BLE */
