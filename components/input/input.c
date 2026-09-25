/**
 * @file input.c
 * Input service — delegates to target-specific input driver.
 *
 * Each target provides its own target_input.c with hardware-specific
 * input handling (touch, encoder, buttons, etc.).
 */

#include "input.h"
#include "target_input.h"

#include <furi.h>
#include <furi_hal_power.h>
#include <furi_hal_display.h>

#define TAG "Input"
#define INPUT_POLL_MS 4U
#define INPUT_IDLE_POLL_MS 25U
#define INPUT_BATTERY_CHECK_MS 1000U

const char* input_get_key_name(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return "Up";
    case InputKeyDown:
        return "Down";
    case InputKeyRight:
        return "Right";
    case InputKeyLeft:
        return "Left";
    case InputKeyOk:
        return "Ok";
    case InputKeyBack:
        return "Back";
    default:
        return "Unknown";
    }
}

const char* input_get_type_name(InputType type) {
    switch(type) {
    case InputTypePress:
        return "Press";
    case InputTypeRelease:
        return "Release";
    case InputTypeShort:
        return "Short";
    case InputTypeLong:
        return "Long";
    case InputTypeRepeat:
        return "Repeat";
    default:
        return "Unknown";
    }
}

int32_t input_srv(void* p) {
    UNUSED(p);

    FuriPubSub* event_pubsub = furi_pubsub_alloc();
    furi_record_create(RECORD_INPUT_EVENTS, event_pubsub);

    target_input_init();

    FURI_LOG_I(TAG, "Input service started");

    uint32_t sequence_counter = 0;
    uint32_t last_battery_check = 0;
    bool on_battery = false;

    while(true) {
        const bool screen_asleep = furi_hal_display_is_asleep();
        if(screen_asleep) {
            /* VBUS detection is an I2C transaction. Polling it every 4 ms
             * prevented meaningful idle sleep and wasted battery. */
            const uint32_t now = furi_get_tick();
            if(!last_battery_check ||
               now - last_battery_check >= furi_ms_to_ticks(INPUT_BATTERY_CHECK_MS)) {
                on_battery = furi_hal_power_is_running_on_battery();
                last_battery_check = now;
            }
        } else {
            last_battery_check = 0;
        }
        furi_hal_power_allow_light_sleep(screen_asleep && on_battery);
        /* The display-off path needs button response, not 250 Hz encoder
         * sampling. This leaves useful idle windows between BLE events. */
        furi_delay_ms(screen_asleep ? INPUT_IDLE_POLL_MS : INPUT_POLL_MS);
        target_input_poll(event_pubsub, &sequence_counter);
    }

    return 0;
}
