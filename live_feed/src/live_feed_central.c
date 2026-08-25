/*
 * The brain of the live feed. Runs on the CENTRAL only — it is the sole side that
 * knows the layer state and the global key position (Torabo-Float/PLAN.md §3).
 *
 * It watches key and layer events, coalesces them, and pushes a small packed
 * event (gatt_service.c does the actual NOTIFY). It also keeps a CRC32 of the
 * whole keymap so the app can tell when its cached layout has gone stale.
 *
 * COALESCING (PLAN §6-5, MANDATORY)
 * ZMK event listeners run synchronously on the system workqueue, and
 * bt_gatt_notify() blocks (K_FOREVER) when the ACL TX buffers are full — so
 * notifying straight from a listener could stall the whole keyboard. Instead the
 * listeners only stash the latest state and submit a single k_work; the work item
 * calls bt_gatt_notify() and drops errors silently (no retry — recovery is the
 * SNAPSHOT sent on resubscribe).
 *
 * The work runs on a DEDICATED work queue (live_feed_wq), never the system
 * workqueue: a Studio RPC keymap-sync burst can keep the ACL TX pool exhausted
 * long enough that a bt_gatt_notify() on the sysworkq would wedge kscan/HID/split
 * (all of which also ride the sysworkq). On its own thread, a blocked notify pends
 * only this thread and the keyboard keeps scanning (PLAN §6-5). See live_feed_wq.
 *
 * Pure "latest state wins" would coalesce a fast press+release into a single
 * event and visually drop one of them. To keep key press/release intact while
 * still bounding memory, we keep TWO pending slots — the latest KEY event and the
 * latest LAYER/SNAPSHOT — and the work item flushes both (at most 2 notifies per
 * run). Bursts on the SAME slot still collapse to the newest, which is fine: a
 * later layer/snapshot already carries the freshest full state.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <string.h>

#if IS_ENABLED(CONFIG_INPUT)
#include <zephyr/input/input.h>
#endif

#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/behavior.h>
#include <zmk/physical_layouts.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>

/* Studio's RPC-notification event (<zmk/studio/rpc.h>) cannot be included here:
 * it pulls in nanopb headers that are generated PRIVATELY into the app target,
 * invisible to an out-of-tree module. We never read the payload — the event is
 * only a "keymap may have changed" tick — so declare just the pieces of its
 * ZMK_EVENT_DECLARE expansion we use, with the struct left opaque. The symbols
 * are emitted by the app image whenever CONFIG_ZMK_STUDIO is on. */
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
struct zmk_studio_rpc_notification; /* opaque: payload not needed */
extern const struct zmk_event_type zmk_event_zmk_studio_rpc_notification;
struct zmk_studio_rpc_notification *as_zmk_studio_rpc_notification(const zmk_event_t *eh);
#endif

#include <zmk_live_feed/live_feed.h>

LOG_MODULE_REGISTER(live_feed, CONFIG_ZMK_LIVE_FEED_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_LIVE_FEED)

/* ---- coalescing state ----------------------------------------------------- */

/* The lock guards the two pending slots. Listeners run on the system workqueue,
 * but live_feed_on_subscribe() is called from the BT RX thread, so the slots are
 * touched from more than one context. */
static struct k_spinlock lock;

/* Latest KEY event fields (the layer/crc fields are filled fresh at flush time). */
struct pend_key {
    uint16_t position;
    uint8_t pressed;
    uint8_t source;
};
static struct pend_key pk;
static bool key_pending;

/* Latest LAYER/SNAPSHOT request: 0 = none, else the evt_type to send. SNAPSHOT
 * takes precedence over LAYER (it is a superset and is what a fresh subscriber
 * needs). Both carry identical payload; only evt_type differs. */
static uint8_t state_pending;

static struct k_work feed_work;

/* Dedicated work queue for the feed. The work handler calls bt_gatt_notify(),
 * which blocks with K_FOREVER while the (only 3) ACL TX buffers are exhausted —
 * exactly what happens during a Studio RPC keymap-sync burst. Running feed_work on
 * the SYSTEM workqueue would then wedge the whole keyboard, because ZMK's kscan /
 * position processing also runs there. On its own thread a blocked notify pends
 * ONLY this thread; scanning/HID/split keep running, and the coalescing (2 pending
 * slots) collapses any events that pile up during the stall (PLAN §6-5). Priority
 * is low + preemptible (like ZMK's low-prio queue) so it never preempts real work;
 * stack matches ZMK's own BLE notify thread with headroom for compute_keymap_crc. */
static struct k_work_q live_feed_wq;
K_THREAD_STACK_DEFINE(live_feed_wq_stack, CONFIG_ZMK_LIVE_FEED_THREAD_STACK_SIZE);

/* Cached keymap CRC. Written only in work context, read from listeners and the
 * snapshot fill. atomic_t makes the 32-bit read/write tear-free across contexts. */
static atomic_t keymap_crc;
static atomic_t crc_dirty; /* set by the studio-rpc listener; cleared in work */

/* ---- keymap CRC ----------------------------------------------------------- */

/* CRC32 over every (layer_id, binding): the layer id byte, then for each binding
 * the behavior name string, param1 and param2. Mirrors how keymap_subsystem.c's
 * encode_layer_bindings enumerates the keymap (index->id, ZMK_KEYMAP_LEN bindings
 * per layer), so the app can recompute the same value from what it fetches over
 * RPC. Runs at boot and whenever a studio RPC notification signals a change. */
static uint32_t compute_keymap_crc(void) {
    uint32_t crc = 0;

    for (zmk_keymap_layer_index_t l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(l);
        if (id == ZMK_KEYMAP_LAYER_ID_INVAL) {
            break;
        }
        crc = crc32_ieee_update(crc, &id, sizeof(id));

        for (uint8_t b = 0; b < ZMK_KEYMAP_LEN; b++) {
            const struct zmk_behavior_binding *bind = zmk_keymap_get_layer_binding_at_idx(id, b);
            const char *name = (bind && bind->behavior_dev) ? bind->behavior_dev : "";
            uint32_t p1 = bind ? bind->param1 : 0;
            uint32_t p2 = bind ? bind->param2 : 0;

            crc = crc32_ieee_update(crc, (const uint8_t *)name, strlen(name));
            crc = crc32_ieee_update(crc, (const uint8_t *)&p1, sizeof(p1));
            crc = crc32_ieee_update(crc, (const uint8_t *)&p2, sizeof(p2));
        }
    }

    return crc;
}

/* ---- event building ------------------------------------------------------- */

/* Fill the layer/layout/crc fields shared by every event from the CURRENT state,
 * so whatever we flush carries the freshest snapshot (PLAN §5). id-keyed, not
 * index-keyed: reordering makes id != index real, and the app caches by Layer.id. */
static void fill_state_fields(struct live_feed_evt *e) {
    e->highest_layer = zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active());
    e->active_layout = (uint8_t)zmk_physical_layouts_get_selected();
    e->layer_mask = zmk_keymap_layer_state();
    e->keymap_crc = (uint32_t)atomic_get(&keymap_crc);
}

void live_feed_fill_snapshot(struct live_feed_evt *out) {
    memset(out, 0, sizeof(*out));
    out->proto_ver = LIVE_FEED_PROTO_VER;
    out->evt_type = LIVE_FEED_EVT_SNAPSHOT;
    out->position = LIVE_FEED_POSITION_NONE;
    fill_state_fields(out);
}

/* ---- the single work item that owns bt_gatt_notify ------------------------ */

/* Every push goes out on both windows: the BLE NOTIFY characteristic, and the
 * Studio RPC tunnel when a client subscribed there (which is how a USB-connected
 * Float receives the feed). Each side no-ops cheaply when nobody is listening —
 * bt_gatt_notify on a clear CCC, torabo_tunnel_notify on an empty subscription —
 * so an unused transport costs one branch per event. Both may block while their
 * transport drains, which is exactly why this only ever runs on live_feed_wq. */
static void lf_push_evt(const struct live_feed_evt *e) {
    (void)live_feed_gatt_notify(e); /* fire-and-forget; drop on error */
#if IS_ENABLED(CONFIG_ZMK_LIVE_FEED_TUNNEL)
    (void)live_feed_tunnel_notify(e);
#endif
}

static void lf_push_diag(const struct live_feed_diag *d) {
    (void)live_feed_diag_notify(d);
#if IS_ENABLED(CONFIG_ZMK_LIVE_FEED_TUNNEL)
    (void)live_feed_tunnel_notify(d);
#endif
}

static void feed_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    /* Recompute the CRC here (work context), never in a listener. */
    if (atomic_cas(&crc_dirty, 1, 0)) {
        atomic_set(&keymap_crc, (atomic_val_t)compute_keymap_crc());
    }

    struct pend_key lpk;
    bool send_key;
    uint8_t send_state;
    K_SPINLOCK(&lock) {
        send_key = key_pending;
        lpk = pk;
        key_pending = false;
        send_state = state_pending;
        state_pending = 0;
    }

    if (send_key) {
        struct live_feed_evt e;
        memset(&e, 0, sizeof(e));
        e.proto_ver = LIVE_FEED_PROTO_VER;
        e.evt_type = LIVE_FEED_EVT_KEY;
        e.position = lpk.position;
        e.pressed = lpk.pressed;
        e.source = lpk.source;
        fill_state_fields(&e);
        lf_push_evt(&e);
    }

    if (send_state) {
        struct live_feed_evt e;
        memset(&e, 0, sizeof(e));
        e.proto_ver = LIVE_FEED_PROTO_VER;
        e.evt_type = send_state; /* LIVE_FEED_EVT_LAYER or _SNAPSHOT */
        e.position = LIVE_FEED_POSITION_NONE;
        fill_state_fields(&e);
        lf_push_evt(&e);
    }
}

/* ---- subscription --------------------------------------------------------- */

void live_feed_on_subscribe(bool enabled) {
    /* On unsubscribe there is nothing to tear down: bt_gatt_notify simply no-ops
     * once the CCC is clear. On subscribe, push a SNAPSHOT so a fresh client
     * immediately learns the current layer/pressed state. */
    if (!enabled) {
        return;
    }
    /* The boot-time CRC may predate the keymap's settings restore (keymap_init
     * runs at the same APPLICATION init priority — ordering is not guaranteed).
     * A subscribe happens long after boot, so recomputing here guarantees the
     * snapshot a fresh client receives always carries the post-restore CRC. */
    atomic_set(&crc_dirty, 1);
    K_SPINLOCK(&lock) {
        state_pending = LIVE_FEED_EVT_SNAPSHOT; /* precedence over a pending LAYER */
    }
    k_work_submit_to_queue(&live_feed_wq, &feed_work);
}

/* ---- event sources -------------------------------------------------------- */

static int lf_key_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev) {
        K_SPINLOCK(&lock) {
            pk.position = (uint16_t)ev->position;
            pk.pressed = ev->state ? 1 : 0;
            pk.source = ev->source; /* 0xFF = central-local, else peripheral slot */
            key_pending = true;
        }
        k_work_submit_to_queue(&live_feed_wq, &feed_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(live_feed_key, lf_key_listener);
ZMK_SUBSCRIPTION(live_feed_key, zmk_position_state_changed);

static int lf_layer_listener(const zmk_event_t *eh) {
    if (as_zmk_layer_state_changed(eh)) {
        K_SPINLOCK(&lock) {
            if (state_pending != LIVE_FEED_EVT_SNAPSHOT) {
                state_pending = LIVE_FEED_EVT_LAYER;
            }
        }
        k_work_submit_to_queue(&live_feed_wq, &feed_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(live_feed_layer, lf_layer_listener);
ZMK_SUBSCRIPTION(live_feed_layer, zmk_layer_state_changed);

/* A studio RPC notification is raised on keymap save (unsaved-changes status), so
 * treat it as "the keymap may have changed": mark the CRC dirty and let the work
 * item recompute it. The new value rides the next KEY/LAYER/SNAPSHOT event.
 * Only compiled with Studio: without it the event symbol isn't emitted and the
 * keymap cannot change at runtime anyway (the boot CRC stays valid). */
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
static int lf_rpc_listener(const zmk_event_t *eh) {
    if (as_zmk_studio_rpc_notification(eh)) {
        atomic_set(&crc_dirty, 1);
        k_work_submit_to_queue(&live_feed_wq, &feed_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(live_feed_rpc, lf_rpc_listener);
ZMK_SUBSCRIPTION(live_feed_rpc, zmk_studio_rpc_notification);
#endif

/* ==========================================================================
 * Diagnostic mode (Torabo-Float §13): per-device liveness on char e1f4af02.
 *
 * A central-side table of the pointing devices we can actually probe
 * (device_is_ready gives init_ok — this is what catches a mis-wired ext pad that
 * fails I2C init), plus one pseudo-device for the encoder whose cw/ccw/btn come
 * from the encoder module's counters. All notifies go through live_feed_wq, never
 * the sysworkq (bt_gatt_notify can block — §6-5), and on a DIFFERENT char from the
 * hot feed so diagnostics never disturb the press visualisation.
 *
 * Peripheral-side devices are enumerated INDIRECTLY, via the central's
 * zmk,input-split RECEIVER slots: on the central they arrive over the split as
 * proxy devices, not as their real driver, so init health is invisible (§13-7) —
 * those rows carry the PERIPHERAL bit ("health is inferred") and never INIT_OK.
 * They still count input events (the proxy IS evt->dev on the central), which is
 * exactly the "is anything flowing from the other half" signal the panel needs.
 * Only central-local pointing devices show a true init_ok.
 *
 * NOTE on compat scope: zmk,input-split nodes exist on BOTH halves (receiver on
 * the central, sender on the peripheral), but this file only compiles on the
 * central (Kconfig depends on ZMK_SPLIT_ROLE_CENTRAL), so every okay-status node
 * seen here is a receiver with a real proxy device behind DEVICE_DT_GET.
 * ========================================================================== */

/* Encoder counters live in the encoder module; declare its getter __weak so a
 * central build WITHOUT that module still links (reporting the encoder absent).
 * The encoder module's strong definition wins when it is compiled in. */
__weak bool enc_diag_get(uint16_t *cw, uint16_t *ccw, uint16_t *btn) {
    ARG_UNUSED(cw);
    ARG_UNUSED(ccw);
    ARG_UNUSED(btn);
    return false;
}

struct diag_entry {
    const struct device *dev; /* NULL for the encoder pseudo-device */
    uint8_t base_meta;        /* kind bits (side/conn unknown at this layer) */
    bool is_encoder;
    bool is_split; /* zmk,input-split receiver: health inferred, never INIT_OK */
    uint8_t slot;  /* split only: DT reg = peripheral slot number, echoed in detail */
    atomic_t event_count;
    atomic_t last_tick;
    atomic_t seen;
};

#define DIAG_IQS_ENTRY(node) {.dev = DEVICE_DT_GET(node), .base_meta = LIVE_FEED_META_KIND_PAD},
#define DIAG_BALL_ENTRY(node) {.dev = DEVICE_DT_GET(node), .base_meta = LIVE_FEED_META_KIND_BALL},

/* Split receiver rows always carry meta 0, even slot 2 (by convention the
 * encoder push button): the app renders a cw/ccw/btn counter line for any row
 * whose meta kind is encoder by decoding `detail`, and split rows put the slot
 * number in detail byte0 — a kind=ENC split row would misrender as counters.
 * meta 0 makes the app fall back to its generic far-side label ("slot N"); the
 * real encoder counters live on the encoder pseudo-device row below. */
#define DIAG_SPLIT_ENTRY(node)                                                                     \
    {.dev = DEVICE_DT_GET(node),                                                                   \
     .base_meta = 0,                                                                               \
     .is_split = true,                                                                             \
     .slot = (uint8_t)DT_REG_ADDR(node)},

/* Enumeration order fixes the wire device_id (= index): central-local devices
 * first (pads then trackball — true init health), then the split receiver slots
 * (inferred health), and the encoder pseudo-device LAST so its id keeps trailing
 * the real devices whatever the build enables. */
static struct diag_entry diag_devs[] = {
#if DT_HAS_COMPAT_STATUS_OKAY(azoteq_iqs7211e)
    DT_FOREACH_STATUS_OKAY(azoteq_iqs7211e, DIAG_IQS_ENTRY)
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(pixart_paw3222)
    DT_FOREACH_STATUS_OKAY(pixart_paw3222, DIAG_BALL_ENTRY)
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_input_split)
    DT_FOREACH_STATUS_OKAY(zmk_input_split, DIAG_SPLIT_ENTRY)
#endif
    {.dev = NULL, .base_meta = LIVE_FEED_META_KIND_ENC, .is_encoder = true},
};
#define DIAG_DEV_COUNT ARRAY_SIZE(diag_devs)

/* The wire caps the READ at LIVE_FEED_DIAG_MAX_DEVICES records; use the same
 * bound for every notify loop so an oversized table degrades consistently
 * (extra rows silently dropped everywhere, not just from the READ). Also keeps
 * every reported id a valid bit of the 32-bit pending mask. */
#define DIAG_DEV_LIMIT MIN(DIAG_DEV_COUNT, LIVE_FEED_DIAG_MAX_DEVICES)

static atomic_t diag_pending_mask; /* bit i = device i has an on-change record to send */
static atomic_t diag_stream_on;    /* heartbeat enabled by an opted-in client */
static struct k_work diag_work;
static struct k_work_delayable diag_hb_work;

static void diag_build_record(int i, struct live_feed_diag *d) {
    struct diag_entry *e = &diag_devs[i];
    memset(d, 0, sizeof(*d));
    d->proto_ver = LIVE_FEED_PROTO_VER;
    d->evt_type = LIVE_FEED_EVT_DIAG;
    d->device_id = (uint8_t)i;
    d->meta = e->base_meta;

    if (e->is_encoder) {
        uint16_t cw = 0, ccw = 0, btn = 0;
        if (enc_diag_get(&cw, &ccw, &btn)) {
            d->status |= LIVE_FEED_DIAG_PRESENT | LIVE_FEED_DIAG_INIT_OK;
            uint32_t total = (uint32_t)cw + ccw + btn;
            if (total) {
                d->status |= LIVE_FEED_DIAG_EVENT_SEEN;
            }
            d->event_count = (uint16_t)total;
            d->detail = ((uint32_t)(cw & 0xFF)) | ((uint32_t)(ccw & 0xFF) << 8) |
                        ((uint32_t)(btn & 0xFF) << 16);
        }
    } else if (e->is_split) {
        /* Split receiver slot: the real driver lives on the OTHER half, so all
         * the central can honestly claim is "this slot exists and events did /
         * did not flow". PRESENT + PERIPHERAL, never INIT_OK — device_is_ready
         * on the proxy would only vouch for the proxy itself, not the far-side
         * driver, and lying green here would defeat the wiring-checker purpose.
         * detail byte0 carries the slot number so the app can label "slot N"
         * (see live_feed.h's detail contract). */
        d->status |= LIVE_FEED_DIAG_PRESENT | LIVE_FEED_DIAG_PERIPHERAL;
        if (atomic_get(&e->seen)) {
            d->status |= LIVE_FEED_DIAG_EVENT_SEEN;
        }
        d->event_count = (uint16_t)atomic_get(&e->event_count);
        d->last_tick_ms = (uint32_t)atomic_get(&e->last_tick);
        d->detail = e->slot;
    } else {
        if (e->dev) {
            d->status |= LIVE_FEED_DIAG_PRESENT;
            if (device_is_ready(e->dev)) {
                d->status |= LIVE_FEED_DIAG_INIT_OK;
            }
        }
        if (atomic_get(&e->seen)) {
            d->status |= LIVE_FEED_DIAG_EVENT_SEEN;
        }
        d->event_count = (uint16_t)atomic_get(&e->event_count);
        d->last_tick_ms = (uint32_t)atomic_get(&e->last_tick);
    }
}

static void diag_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_val_t mask = atomic_clear(&diag_pending_mask);
    for (int i = 0; i < (int)DIAG_DEV_LIMIT; i++) {
        if (mask & BIT(i)) {
            struct live_feed_diag d;
            diag_build_record(i, &d);
            lf_push_diag(&d);
        }
    }
}

static void diag_hb_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!atomic_get(&diag_stream_on)) {
        return;
    }
    for (int i = 0; i < (int)DIAG_DEV_LIMIT; i++) {
        struct live_feed_diag d;
        diag_build_record(i, &d);
        lf_push_diag(&d);
    }
    k_work_reschedule_for_queue(&live_feed_wq, &diag_hb_work,
                                K_MSEC(CONFIG_ZMK_LIVE_FEED_DIAG_HEARTBEAT_MS));
}

static void diag_submit_onchange(int i) {
    atomic_or(&diag_pending_mask, BIT(i));
    k_work_submit_to_queue(&live_feed_wq, &diag_work);
}

uint16_t live_feed_diag_fill_all(uint8_t *buf, uint16_t cap) {
    uint16_t off = 0;
    for (int i = 0; i < (int)DIAG_DEV_LIMIT; i++) {
        if (off + sizeof(struct live_feed_diag) > cap) {
            break;
        }
        struct live_feed_diag d;
        diag_build_record(i, &d);
        memcpy(buf + off, &d, sizeof(d));
        off += sizeof(d);
    }
    return off;
}

void live_feed_diag_set_stream(bool on) {
    atomic_set(&diag_stream_on, on ? 1 : 0);
    if (on) {
        k_work_reschedule_for_queue(&live_feed_wq, &diag_hb_work, K_NO_WAIT);
    } else {
        (void)k_work_cancel_delayable(&diag_hb_work);
    }
}

#if IS_ENABLED(CONFIG_INPUT)
/* Bump the matching device's counters. Runs on the input thread — keep it to atomic
 * writes + a work submit; the actual notify happens on live_feed_wq. */
static void diag_input_cb(struct input_event *evt) {
    if (!evt || !evt->dev) {
        return;
    }
    /* Bound by DIAG_DEV_LIMIT, not DIAG_DEV_COUNT: an entry past the wire cap can
     * never be reported, so counting its events (and submitting on-change work for
     * a bit that doesn't exist in the mask) would be wasted effort. */
    for (int i = 0; i < (int)DIAG_DEV_LIMIT; i++) {
        if (diag_devs[i].dev == evt->dev) {
            atomic_inc(&diag_devs[i].event_count);
            atomic_set(&diag_devs[i].last_tick, (atomic_val_t)k_uptime_get_32());
            if (atomic_set(&diag_devs[i].seen, 1) == 0) {
                diag_submit_onchange(i); /* first sign of life: push immediately */
            }
            break;
        }
    }
}
INPUT_CALLBACK_DEFINE(NULL, diag_input_cb);
#endif /* CONFIG_INPUT */

static void diag_init(void) {
    k_work_init(&diag_work, diag_work_handler);
    k_work_init_delayable(&diag_hb_work, diag_hb_handler);
}

/* ---- init ----------------------------------------------------------------- */

static int live_feed_init(void) {
    k_work_init(&feed_work, feed_work_handler);

    static const struct k_work_queue_config wq_config = {.name = "live_feed"};
    k_work_queue_start(&live_feed_wq, live_feed_wq_stack,
                       K_THREAD_STACK_SIZEOF(live_feed_wq_stack),
                       CONFIG_ZMK_LIVE_FEED_THREAD_PRIORITY, &wq_config);

    atomic_set(&keymap_crc, (atomic_val_t)compute_keymap_crc());
    diag_init();
    return 0;
}

SYS_INIT(live_feed_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_LIVE_FEED */
