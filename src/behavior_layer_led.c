/*
 * k04,led-layer-color — push a persistent status LED palette color to all
 * split halves.
 *
 * Locality GLOBAL: an invocation issued on the central runs the behavior
 * locally and on every connected peripheral
 * (zmk_behavior_invoke_binding() routes GLOBAL-locality behaviors through
 * the split transport in ZMK v0.3). The central's status widget uses this
 * to mirror layer colors onto the peripheral's LED, which has no layer
 * state of its own.
 *
 * param1: palette index 0-7 to show, 0xFF to clear (fall back to
 *         charging/base display).
 */

#define DT_DRV_COMPAT k04_led_layer_color

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <zmk_rgbled_widget/widget.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int behavior_layer_led_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    switch (binding->param2) {
    case 1: /* transient: one full-brightness second */
        k04_status_led_blink_color((int8_t)(binding->param1 & 0xFF));
        break;
    case 2: /* settings: toggle the base-layer light */
        k04_status_led_toggle_base_light();
        break;
    default: /* persistent: (possibly base-flagged) layer color */
        k04_status_led_show_color((int16_t)binding->param1);
        break;
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_layer_led_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

#define LAYER_LED_INST(n)                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_layer_led_init, NULL, NULL, NULL, POST_KERNEL,               \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_layer_led_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LAYER_LED_INST)
