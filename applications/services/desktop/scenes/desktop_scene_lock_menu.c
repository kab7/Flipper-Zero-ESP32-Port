#include <furi.h>
#include <gui/scene_manager.h>

#include <btshim.h>
#include <wifi.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <bootloader_common.h>
#include <stdint.h>

#include "../desktop_i.h"
#include "../views/desktop_view_lock_menu.h"
#include "../helpers/qflipper_bridge.h"
#include "furi_hal_usb_tinyusb_composite.h"
#include "desktop_scene.h"

#include "sdkconfig.h"

/* qFlipper / USB-Storage need USB-OTG (ESP32-S3 / S2 only). */
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#define LOCK_MENU_USB_AVAILABLE true
#else
#define LOCK_MENU_USB_AVAILABLE false
#endif

void desktop_scene_lock_menu_callback(DesktopEvent event, void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

static bool desktop_lock_menu_bt_enabled(void) {
    Bt* bt = furi_record_open(RECORD_BT);
    BtSettings settings;
    bt_get_settings(bt, &settings);
    furi_record_close(RECORD_BT);
    return settings.enabled;
}

static void desktop_lock_menu_set_bt_enabled(bool enabled) {
    Bt* bt = furi_record_open(RECORD_BT);
    BtSettings settings;
    bt_get_settings(bt, &settings);
    settings.enabled = enabled;
    bt_set_settings(bt, &settings);
    furi_record_close(RECORD_BT);
}

static bool desktop_lock_menu_wifi_enabled(void) {
    Wifi* wifi = furi_record_open(RECORD_WIFI);
    bool enabled = wifi_is_enabled(wifi);
    furi_record_close(RECORD_WIFI);
    return enabled;
}

static const esp_partition_t* desktop_lock_menu_bruce_partition(void) {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "bruce");
    if(!partition) return NULL;
    esp_app_desc_t description;
    return esp_ota_get_partition_description(partition, &description) == ESP_OK ? partition : NULL;
}

/* IDF 5.4.1 verifies the *entire* target image in esp_ota_set_boot_partition().
 * Bruce's large mapped segment can trip the interrupt watchdog while the GUI
 * is running. Write only the OTA selection record here; the bootloader still
 * verifies the image before loading it and falls back to ota_0 if it is bad.
 * This mirrors esp_rewrite_ota_data() in IDF's esp_ota_ops.c. */
static esp_err_t desktop_lock_menu_select_bruce(const esp_partition_t* target) {
    if(!target || target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1 ||
       target->type != ESP_PARTITION_TYPE_APP) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t* data = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if(!data || data->size < 2 * data->erase_size) return ESP_ERR_NOT_FOUND;

    const uint8_t count = esp_ota_get_app_partition_count();
    if(count < 2) return ESP_ERR_INVALID_STATE;

    esp_ota_select_entry_t entries[2];
    for(size_t i = 0; i < 2; i++) {
        esp_err_t result = esp_partition_read(
            data, i * data->erase_size, &entries[i], sizeof(entries[i]));
        if(result != ESP_OK) return result;
    }

    const int active = bootloader_common_get_active_otadata(entries);
    const int next = active < 0 ? 0 : 1 - active;
    const uint32_t wanted = 2; /* ota_1 -> (ota_seq - 1) % count == 1 */
    uint32_t sequence = wanted;
    if(active >= 0) {
        const uint32_t previous = entries[active].ota_seq;
        uint32_t advance = (wanted + count - (previous % count)) % count;
        if(advance == 0) advance = count;
        if(previous > UINT32_MAX - advance) return ESP_ERR_INVALID_STATE;
        sequence = previous + advance;
    }

    entries[next].ota_seq = sequence;
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    entries[next].ota_state = ESP_OTA_IMG_NEW;
#else
    entries[next].ota_state = ESP_OTA_IMG_UNDEFINED;
#endif
    entries[next].crc = bootloader_common_ota_select_crc(&entries[next]);
    esp_err_t result =
        esp_partition_erase_range(data, next * data->erase_size, data->erase_size);
    if(result != ESP_OK) return result;
    return esp_partition_write(
        data, next * data->erase_size, &entries[next], sizeof(entries[next]));
}

/* Rebuild the menu from the live toggle states (used on enter and after a
 * toggle, so the Enable/Disable labels track reality). */
void desktop_scene_lock_menu_refresh(Desktop* desktop) {
    desktop_lock_menu_set_states(
        desktop->lock_menu,
        LOCK_MENU_USB_AVAILABLE,
        qflipper_bridge_is_active(),
        desktop_lock_menu_bt_enabled(),
        desktop_lock_menu_wifi_enabled(),
        desktop_lock_menu_bruce_partition() != NULL);
}

void desktop_scene_lock_menu_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    desktop_lock_menu_set_callback(desktop->lock_menu, desktop_scene_lock_menu_callback, desktop);
    desktop_scene_lock_menu_refresh(desktop);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdLockMenu);
}

bool desktop_scene_lock_menu_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopLockMenuEventQflipperToggle:
            if(qflipper_bridge_is_active()) {
                qflipper_bridge_stop();
                /* Bridge detached — now fully tear the shared composite down so
                 * the internal USB PHY routes back to USB-Serial-JTAG. esptool
                 * can flash again immediately, no reboot / BOOT+RESET needed. */
                furi_hal_usb_composite_uninstall();
            } else {
                qflipper_bridge_start();
            }
            /* Stay in the menu; refresh so the label flips. */
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;

        case DesktopLockMenuEventUsbStorage:
            /* The USB-Storage scene stops the qFlipper bridge itself (shared
             * composite / mutual exclusion). */
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneUsbStorage);
            consumed = true;
            break;

        case DesktopLockMenuEventBluetoothToggle: {
            bool want_on = !desktop_lock_menu_bt_enabled();
            if(want_on) {
                /* Gegenseitiger Ausschluss (geteiltes Radio/RAM): erst WiFi global
                 * aus (gibt Radio + RAM frei), dann den BLE-Stack real hochfahren.
                 * bt_start_stack ist idempotent — bei bereits laufendem Stack ein
                 * No-op; nach vorheriger WiFi-Nutzung war er abgebaut. */
                Wifi* wifi = furi_record_open(RECORD_WIFI);
                if(wifi_is_enabled(wifi)) wifi_disable(wifi);
                furi_record_close(RECORD_WIFI);
                Bt* bt = furi_record_open(RECORD_BT);
                bt_start_stack(bt);
                furi_record_close(RECORD_BT);
            }
            desktop_lock_menu_set_bt_enabled(want_on);
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;
        }

        case DesktopLockMenuEventWifiToggle: {
            /* WiFi global toggeln. Enable schaltet BLE aus + persistiert, zieht das
             * Radio hoch und reconnectet zum zuletzt genutzten Netz (siehe
             * wifi_enable). Disable baut das Radio ab; BLE bleibt bewusst aus. */
            Wifi* wifi = furi_record_open(RECORD_WIFI);
            if(wifi_is_enabled(wifi)) {
                wifi_disable(wifi);
            } else {
                wifi_enable(wifi);
            }
            furi_record_close(RECORD_WIFI);
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;
        }

        case DesktopLockMenuEventMeshClients:
            /* T-Embed ist immer Master; der Master-Mesh-Service läuft on-demand in
             * der Mesh-Clients-Scene. */
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneMeshClients);
            consumed = true;
            break;

        case DesktopLockMenuEventWebFs:
            /* Web-Filesystem lives in the WiFi app; launch it into that flow. */
            loader_start_detached_with_gui_error(desktop->loader, "wlan", "webfs");
            consumed = true;
            break;

        case DesktopLockMenuEventBruce: {
            const esp_partition_t* bruce = desktop_lock_menu_bruce_partition();
            if(!bruce) {
                FURI_LOG_E("DesktopBruce", "Bruce image is missing or invalid");
            } else {
                esp_err_t result = desktop_lock_menu_select_bruce(bruce);
                if(result == ESP_OK) {
                    furi_hal_power_reset();
                } else {
                    FURI_LOG_E("DesktopBruce", "Cannot boot Bruce: %s", esp_err_to_name(result));
                }
            }
            consumed = true;
            break;
        }

        default:
            break;
        }
    }

    return consumed;
}

void desktop_scene_lock_menu_on_exit(void* context) {
    UNUSED(context);
}
