/*
 * The handful of firmware-side symbols the wire codecs CALL but do not define.
 *
 * On the keyboard these live in the ZMK fork's split central; on the host they
 * are no-ops. Keeping them here (rather than weak-stubbing them) means a link
 * error is the signal if a codec ever grows a new outward dependency — which is
 * exactly the kind of coupling phase 0 is meant to notice.
 */

#include <stdint.h>

#include <zmk/torabo_timing.h>

/* Called by tmg_apply_wire() to re-push the debounce windows over the split
 * link. Nothing to push in a host test. */
void zmk_torabo_debounce_split_push(void) {}

/* Peripheral-side receiver; never invoked by the codecs under test, but declared
 * in the same fork header, so define it once for completeness. */
void zmk_torabo_debounce_split_apply(uint8_t press_ms, uint8_t release_ms) {
    (void)press_ms;
    (void)release_ms;
}
