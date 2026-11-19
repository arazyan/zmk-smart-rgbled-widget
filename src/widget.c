/*
 * Smart RGB LED status widget — K:04 status language.
 *
 * Persistent display, PER HALF (priority order):
 *   1. layer color — language layers (0 en / 1 ru) on BOTH halves (they
 *      change what keys on both halves type); MO layers 2+ only on the
 *      half whose key press activated the layer (attributed via
 *      position-event sources; the other half keeps the base display)
 *   2. charging color while USB is connected: yellow, green at >= 95%
 *      3. base layer color (white by default), subject to
 *      RGBLED_WIDGET_BASE_LAYER_TIMEOUT_MS (0 = always on) and the
 *      key-toggle; cleared when toggled off
 *   4. off
 *
 * Transient patterns (override the persistent display):
 *   - BT profile switch: profile color for 1 s
 *     (BT0 red, BT1 blue, BT2 yellow, BT3 green, BT4 magenta)
 *   - advertising, ready to pair: blue blink (500 ms cycle)
 *   - reconnecting to a known device: white flash (300 ms cycle)
 *   - looking for the other half (peripheral): yellow blink (500 ms cycle)
 *   - host / other half connected: green for 1 s
 *   - battery <= 10% and no USB: red flash every 60 s (every 15 s
 *     below 5%)
 *   - &ind_bat key: own charge 1 s (<=20% red, <=40% orange, <=74%
 *     yellow, else green), then each peripheral's charge 1 s
 *
 * Sleep: LED off unless connected via USB.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/rgb_underglow.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/usb.h>

#if __has_include(<zmk/split/central.h>)
#include <zmk/split/central.h>
#define ZMK_SPLIT_CENTRAL_BATTERY_GET zmk_split_central_get_peripheral_battery_level
#else
#include <zmk/split/bluetooth/central.h>
#define ZMK_SPLIT_CENTRAL_BATTERY_GET zmk_split_get_peripheral_battery_level
#endif

#include <zephyr/logging/log.h>

#include <zmk_rgbled_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "A zmk,underglow chosen node must be declared"
#endif

/* ZMK v0.3 compatibility: the underglow effect enum lives privately in
 * app/src/rgb_underglow.c — SOLID is its first member. */
#define UNDERGLOW_EFFECT_SOLID_COMPAT 0

/* ZMK v0.3 compatibility: USB_POWERED only exists in newer
 * ZMK; v0.3 exposes zmk_usb_get_conn_state(). "USB power present" is
 * the charging condition (POWERED or HID). */
#define USB_POWERED (zmk_usb_get_conn_state() != ZMK_USB_CONN_NONE)

#define IS_CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if IS_CENTRAL
#include <zmk/behavior.h>
#include <zmk/events/position_state_changed.h>

/* Targeted push to the right half only (peripheral slot 0): unlike the
 * zmk_behavior_invoke_binding GLOBAL broadcast, this reaches just one
 * half — layer colors now light only the half whose key caused them. */
static void push_right(uint32_t param1, uint32_t param2) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = DT_PROP(DT_NODELABEL(k04_layer_led), label),
        .param1 = param1,
        .param2 = param2,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .layer = 0,
        .timestamp = k_uptime_get(),
    };
    zmk_split_central_invoke_behavior(0, &binding, event, true);
}

/* Which half pressed the most recent key: SOURCE_LOCAL (left) or a
 * peripheral slot index (right). Layer activations are attributed to
 * this half in the poll. */
static uint8_t last_press_source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;

static int led_position_listener_cb(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev != NULL && ev->state) {
        last_press_source = ev->source;
    }
    return 0;
}

ZMK_LISTENER(led_position_listener, led_position_listener_cb);
ZMK_SUBSCRIPTION(led_position_listener, zmk_position_state_changed);
#endif

// color palette, indices: 0 black, 1 red, 2 green, 3 yellow, 4 blue,
// 5 magenta, 6 cyan, 7 white
static const struct zmk_led_hsb color_def[8] = {
    {0, 0, 0},
    {0, 100, 100},
    {120, 100, 100},
    {60, 100, 100},
    {240, 100, 100},
    {300, 100, 100},
    {180, 100, 100},
    {0, 0, 100},
};

/* orange for the 21-40% battery tier (not part of the 8-color enum) */
#define COLOR_ORANGE_H 30

#if IS_CENTRAL
static const uint8_t layer_color_idx[] = {
    CONFIG_RGBLED_WIDGET_LAYER_0_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_1_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_2_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_3_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_4_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_5_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_6_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_7_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_8_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_9_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_10_COLOR, CONFIG_RGBLED_WIDGET_LAYER_11_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_12_COLOR, CONFIG_RGBLED_WIDGET_LAYER_13_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_14_COLOR, CONFIG_RGBLED_WIDGET_LAYER_15_COLOR,
};

static const uint8_t profile_color_idx[] = {
    CONFIG_RGBLED_WIDGET_PROFILE_COLOR_0,
    CONFIG_RGBLED_WIDGET_PROFILE_COLOR_1,
    CONFIG_RGBLED_WIDGET_PROFILE_COLOR_2,
    CONFIG_RGBLED_WIDGET_PROFILE_COLOR_3,
    CONFIG_RGBLED_WIDGET_PROFILE_COLOR_4,
};
#endif

// ------------------------------------------------------------------ state

static bool initialized = false;
static bool asleep = false;

// host connection edge tracking (central: host over BT or USB;
// peripheral: split link to the central)
static bool prev_connected = false;

// override: show override_color until override_until
static int64_t override_until = 0;
static struct zmk_led_hsb override_color;

static int64_t last_low_battery_blink = 0;

#if IS_CENTRAL
static uint8_t prev_layer = 255;
static int64_t base_layer_since = 0;
static int32_t last_left_push = -2;  /* force first push */
static int32_t last_right_push = -2; /* force first push */
#endif

// persistent layer color pushed to every half (palette index, -1 = none)
static int8_t layer_led_color = -1;
static bool layer_color_is_base = false;

// key-toggled: whether the base-layer (white) display is allowed at all
static bool base_light_enabled = true;

void k04_status_led_show_color(int16_t param) {
    if (param == 0xFF) {
        layer_led_color = -1;
        layer_color_is_base = false;
    } else {
        layer_led_color = param & 0xFF;
        layer_color_is_base = (param & 0x100) != 0;
    }
}

void k04_status_led_toggle_base_light(void) {
    base_light_enabled = !base_light_enabled;
#if IS_CENTRAL
    /* force the central to recompute and re-push on the next tick */
    last_left_push = -2;
    last_right_push = -2;
#endif
}

// -------------------------------------------------------------- LED helpers

static struct zmk_led_hsb hsb_dim(struct zmk_led_hsb c) {
    c.b = (c.b * CONFIG_RGBLED_WIDGET_PERSISTENT_BRIGHTNESS) / 100;
    return c;
}

static void apply_hsb(struct zmk_led_hsb c) {
    zmk_rgb_underglow_select_effect(UNDERGLOW_EFFECT_SOLID_COMPAT);
    zmk_rgb_underglow_set_hsb(c);
    zmk_rgb_underglow_on();
}

static void apply_black(void) {
    /* WS2812-class LEDs latch their last frame; an explicit black write
     * is the deterministic way to go dark. */
    apply_hsb(color_def[0]);
}

static void override_for(struct zmk_led_hsb c, uint16_t ms) {
    override_color = c;
    override_until = k_uptime_get() + ms;
    apply_hsb(c);
}

void k04_status_led_blink_color(int8_t palette_idx) {
    if (palette_idx >= 0 && palette_idx < 8) {
        override_for(color_def[palette_idx], 1000);
    }
}

// ------------------------------------------------------------- battery

static struct zmk_led_hsb battery_tier_color(uint8_t level) {
    if (level == 0) {
        return color_def[0]; // unknown
    }
    if (level <= 20) {
        return color_def[1]; // red
    }
    if (level <= 40) {
        return (struct zmk_led_hsb){COLOR_ORANGE_H, 100, 100};
    }
    if (level <= 74) {
        return color_def[3]; // yellow
    }
    return color_def[2]; // green
}

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
void indicate_battery(void) {
    /* One symmetric second of this half's own charge tier. The halves
     * blink in parallel (GLOBAL-locality keymap behavior), so a second
     * stage showing the peripheral's level on the central made the left
     * LED visibly outlast the right one — dropped. */
    uint8_t level = zmk_battery_state_of_charge();

    override_color = battery_tier_color(level);
    override_until = k_uptime_get() + 1000;
    apply_hsb(override_color);
}
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
void indicate_connectivity(void) {
    /* state machine picks connection state up within a poll tick */
}
#endif

#if IS_CENTRAL
void indicate_layer(void) {
    /* state machine picks layer changes up within a poll tick */
}
#endif

// ------------------------------------------------------------- events

#if IS_CENTRAL && IS_ENABLED(CONFIG_ZMK_BLE)
static int led_profile_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }
    // BT profile switched: profile color for one second on BOTH halves —
    // locally on the central, and via a targeted push to the right half.
    uint8_t profile = zmk_ble_active_profile_index();
    uint8_t color_idx = (profile < 5) ? profile_color_idx[profile] : 0;
    LOG_INF("Profile %d selected, blinking color %d for 1s on all halves", profile, color_idx);

    k04_status_led_blink_color((int8_t)color_idx);
    push_right(color_idx, 1);
    return 0;
}

ZMK_LISTENER(led_profile_listener, led_profile_listener_cb);
ZMK_SUBSCRIPTION(led_profile_listener, zmk_ble_active_profile_changed);
#endif

static int led_activity_listener_cb(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }
    switch (ev->state) {
    case ZMK_ACTIVITY_SLEEP:
        asleep = true;
        // LED off in sleep unless the half is powered from USB
        if (!USB_POWERED) {
            apply_black();
        }
        break;
    default:
        asleep = false;
        break;
    }
    return 0;
}

ZMK_LISTENER(led_activity_listener, led_activity_listener_cb);
ZMK_SUBSCRIPTION(led_activity_listener, zmk_activity_state_changed);

// --------------------------------------------------------- state machine

#define STATUS_POLL_MS 30

static void status_poll(void) {
    int64_t now = k_uptime_get();
    bool usb = USB_POWERED;

    if (asleep) {
        return; // parked black by the activity listener (or on USB power)
    }

    // active override?
    if (now < override_until) {
        apply_hsb(override_color);
        return;
    }

#if IS_CENTRAL
    uint8_t layer = zmk_keymap_highest_layer_active();
    if (layer != prev_layer) {
        prev_layer = layer;
        if (layer == 0) {
            base_layer_since = now;
        }
    }

    // Per-half targets. Language layers 0-1 are GLOBAL — they change
    // what keys on both halves type, so both LEDs show the color. MO
    // layers 2+ light only the half whose key press caused them
    // (last_press_source). Base: white while enabled (timeout 0 =
    // always).
    int32_t base_target =
        (base_light_enabled &&
         (CONFIG_RGBLED_WIDGET_BASE_LAYER_TIMEOUT_MS == 0 ||
          now - base_layer_since < CONFIG_RGBLED_WIDGET_BASE_LAYER_TIMEOUT_MS))
            ? (int32_t)(layer_color_idx[0] | 0x100)
            : -1;

    int32_t left_target = base_target;
    int32_t right_target = base_target;
    if (layer == 1) {
        // language layer: both halves
        left_target = right_target = layer_color_idx[1];
    } else if (layer > 1) {
        int32_t lc = layer_color_idx[layer & 0xF];
        if (last_press_source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
            left_target = lc; // left key press: left LED only, right stays white
        } else {
            right_target = lc; // right key press: right LED only, left stays white
        }
    }

    if (left_target != last_left_push) {
        last_left_push = left_target;
        k04_status_led_show_color(left_target < 0 ? (int16_t)0xFF : (int16_t)left_target);
    }
    if (right_target != last_right_push) {
        push_right(right_target < 0 ? 0xFF : (uint32_t)right_target, 0);
        last_right_push = right_target;
    }

    // host connection edge: computer (USB or BT profile) connected
    bool connected;
    if (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB) {
        connected = true;
    } else {
#if IS_ENABLED(CONFIG_ZMK_BLE)
        connected = zmk_ble_active_profile_is_connected();
#else
        connected = false;
#endif
    }

    if (connected != prev_connected) {
        prev_connected = connected;
        if (connected) {
            override_for(color_def[2], 1000); // green for one second
            last_right_push = -2;             // re-push after the peripheral reset itself
            return;
        }
    }

#if IS_ENABLED(CONFIG_ZMK_BLE)
    // pairing / reconnecting patterns (BLE transport only)
    if (zmk_endpoints_selected().transport != ZMK_TRANSPORT_USB &&
        !zmk_ble_active_profile_is_connected()) {
        if (zmk_ble_active_profile_is_open()) {
            // ready to pair with a new device: blue blink
            bool on = (now / 500) % 2 == 0;
            if (on) {
                apply_hsb(color_def[4]);
            } else {
                apply_black();
            }
            return;
        }
        // reconnecting to a known device: white flash
        bool on = (now / 300) % 2 == 0;
        if (on) {
            apply_hsb(color_def[7]);
        } else {
            apply_black();
        }
        return;
    }
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

#else // peripheral half
    bool connected = zmk_split_bt_peripheral_is_connected();

    if (connected != prev_connected) {
        prev_connected = connected;
        if (connected) {
            override_for(color_def[2], 1000); // green for one second
            return;
        }
        // link lost: drop the stale layer color pushed by the central
        k04_status_led_show_color(0xFF);
    }

    if (!connected) {
        // looking for the other half: yellow blink
        bool on = (now / 500) % 2 == 0;
        if (on) {
            apply_hsb(color_def[3]);
        } else {
            apply_black();
        }
        return;
    }
#endif // IS_CENTRAL / peripheral

    // low battery: red flash while under 10%, more often under 5%
    uint8_t level = 0;
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    level = zmk_battery_state_of_charge();
    if (level > 0 && level <= 10 && !usb) {
        int64_t interval = level <= 5
                               ? CONFIG_RGBLED_WIDGET_LOW_BATTERY_CRITICAL_INTERVAL_MS
                               : CONFIG_RGBLED_WIDGET_LOW_BATTERY_BLINK_INTERVAL_MS;
        if (now - last_low_battery_blink >= interval) {
            last_low_battery_blink = now;
            override_for(color_def[1], 200); // visible red flash
            return;
        }
    }
#endif

    // ------------------------------------------------ persistent display

    /* Charging takes priority over the base-layer color, but never over
     * an active MO layer color. */
    if (usb && (layer_led_color < 0 || layer_color_is_base)) {
        apply_hsb(hsb_dim(level >= 95 ? color_def[2] : color_def[3]));
        return;
    }

    if (layer_led_color >= 0) {
        /* color pushed to this half (layer colors work on peripherals,
         * which have no layer state of their own) */
        apply_hsb(hsb_dim(color_def[layer_led_color]));
        return;
    }

    apply_black();
}

extern void led_status_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    while (true) {
        if (initialized) {
            status_poll();
        }
        k_msleep(STATUS_POLL_MS);
    }
}

K_THREAD_DEFINE(led_status_tid, 1024, led_status_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

static int led_widget_init(const struct device *dev) {
    ARG_UNUSED(dev);
    initialized = true;
    LOG_INF("K:04 status LED widget initialized");
    return 0;
}

SYS_INIT(led_widget_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
