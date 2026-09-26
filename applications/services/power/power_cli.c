#include "power_cli.h"

#include <furi_hal.h>
#include <toolbox/cli/cli_command.h>
#include <cli/cli_main_commands.h>
#include <lib/toolbox/args.h>
#include <power/power_service/power.h>
#include <toolbox/pipe.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_sleep.h>

static const char* power_cli_reset_name(uint32_t reason) {
    switch(reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

static const char* power_cli_wake_name(uint32_t cause) {
    switch(cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "none";
    case ESP_SLEEP_WAKEUP_EXT0: return "EXT0 button";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_GPIO: return "GPIO";
    default: return "other";
    }
}

static const char* power_cli_shutdown_mode_name(FuriHalPowerShutdownMode mode) {
    switch(mode) {
    case FuriHalPowerShutdownModeDeepSleep: return "Deep Sleep";
    case FuriHalPowerShutdownModePowerOff: return "Power Off";
    default: return "none";
    }
}

static const char* power_cli_shutdown_stage_name(FuriHalPowerShutdownStage stage) {
    switch(stage) {
    case FuriHalPowerShutdownStageRequested: return "requested";
    case FuriHalPowerShutdownStagePrepared: return "display/peripherals off";
    case FuriHalPowerShutdownStageChargerCheck: return "charger checked";
    case FuriHalPowerShutdownStageShipCommand: return "ship command started";
    case FuriHalPowerShutdownStageShipReturned: return "ship command returned";
    case FuriHalPowerShutdownStageWakeConfigured: return "wake source configured";
    case FuriHalPowerShutdownStageEnteringDeepSleep: return "entering deep sleep";
    default: return "none";
    }
}

static void power_cli_diag(void) {
    FuriHalPowerShutdownDiagnostics diag;
    furi_hal_power_get_shutdown_diagnostics(&diag);
    printf(
        "Boot: reset=%lu (%s), wake=%lu (%s)\r\n",
        (unsigned long)diag.reset_reason,
        power_cli_reset_name(diag.reset_reason),
        (unsigned long)diag.wakeup_cause,
        power_cli_wake_name(diag.wakeup_cause));
    if(!diag.has_previous_attempt) {
        printf("No shutdown attempt recorded before this boot.\r\n");
        return;
    }

    printf(
        "Previous shutdown: mode=%s, last stage=%s\r\n",
        power_cli_shutdown_mode_name(diag.mode),
        power_cli_shutdown_stage_name(diag.stage));
    printf(
        "BOOT level=%ld, charger=%ld, USB VBUS=%ld, ship I2C=%ld\r\n",
        (long)diag.button_level,
        (long)diag.charger_present,
        (long)diag.vbus_present,
        (long)diag.ship_write_ok);
    if(diag.wake_config_error >= 0) {
        printf("Wake configuration: %s\r\n", esp_err_to_name(diag.wake_config_error));
    }
    printf("(-1 means the step was not reached.)\r\n");
}

static void power_cli_sleep(void) {
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    FuriHalPowerLightSleepStats stats;
    furi_hal_power_get_light_sleep_stats(&stats);
    printf(
        "Light sleep: allowed=%d, entries=%lu, total=%llu ms\r\n",
        stats.allowed,
        (unsigned long)stats.sleep_count,
        (unsigned long long)(stats.total_sleep_us / 1000ULL));
    printf(
        "Wake causes: timer=%lu, GPIO=%lu, other=%lu\r\n",
        (unsigned long)stats.wake_timer,
        (unsigned long)stats.wake_gpio,
        (unsigned long)stats.wake_other);
#else
    printf("Light-sleep counters are disabled in this build.\r\n");
#endif
}

void power_cli_off(PipeSide* pipe, FuriString* args) {
    UNUSED(pipe);
    UNUSED(args);
    Power* power = furi_record_open(RECORD_POWER);
    printf("It's now safe to disconnect USB from your flipper\r\n");
    furi_delay_ms(666);
    power_off(power);
}

void power_cli_reboot(PipeSide* pipe, FuriString* args) {
    UNUSED(pipe);
    UNUSED(args);
    Power* power = furi_record_open(RECORD_POWER);
    power_reboot(power, PowerBootModeNormal);
}

void power_cli_reboot2dfu(PipeSide* pipe, FuriString* args) {
    UNUSED(pipe);
    UNUSED(args);
    Power* power = furi_record_open(RECORD_POWER);
    power_reboot(power, PowerBootModeDfu);
}

void power_cli_5v(PipeSide* pipe, FuriString* args) {
    UNUSED(pipe);
    Power* power = furi_record_open(RECORD_POWER);
    if(!furi_string_cmp(args, "0")) {
        power_enable_otg(power, false);
    } else if(!furi_string_cmp(args, "1")) {
        power_enable_otg(power, true);
    } else {
        cli_print_usage("power_otg", "<1|0>", furi_string_get_cstr(args));
    }

    furi_record_close(RECORD_POWER);
}

void power_cli_3v3(PipeSide* pipe, FuriString* args) {
    UNUSED(pipe);
    if(!furi_string_cmp(args, "0")) {
        furi_hal_power_disable_external_3_3v();
    } else if(!furi_string_cmp(args, "1")) {
        furi_hal_power_enable_external_3_3v();
    } else {
        cli_print_usage("power_ext", "<1|0>", furi_string_get_cstr(args));
    }
}

static void power_cli_command_print_usage(void) {
    printf("Usage:\r\n");
    printf("power <cmd> <args>\r\n");
    printf("Cmd list:\r\n");

    printf("\toff\t - shutdown power\r\n");
    printf("\tdiag\t - show last shutdown and wake reason\r\n");
    printf("\tsleep\t - show light-sleep counters since boot\r\n");
    printf("\treboot\t - reboot\r\n");
    printf("\treboot2dfu\t - reboot to dfu bootloader\r\n");
    printf("\t5v <0 or 1>\t - enable or disable 5v ext\r\n");
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug)) {
        printf("\t3v3 <0 or 1>\t - enable or disable 3v3 ext\r\n");
    }
}

void power_cli(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    FuriString* cmd;
    cmd = furi_string_alloc();

    do {
        if(!args_read_string_and_trim(args, cmd)) {
            power_cli_command_print_usage();
            break;
        }

        if(furi_string_cmp_str(cmd, "off") == 0) {
            power_cli_off(pipe, args);
            break;
        }

        if(furi_string_cmp_str(cmd, "diag") == 0) {
            power_cli_diag();
            break;
        }

        if(furi_string_cmp_str(cmd, "sleep") == 0) {
            power_cli_sleep();
            break;
        }

        if(furi_string_cmp_str(cmd, "reboot") == 0) {
            power_cli_reboot(pipe, args);
            break;
        }

        if(furi_string_cmp_str(cmd, "reboot2dfu") == 0) {
            power_cli_reboot2dfu(pipe, args);
            break;
        }

        if(furi_string_cmp_str(cmd, "5v") == 0) {
            power_cli_5v(pipe, args);
            break;
        }

        if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug)) {
            if(furi_string_cmp_str(cmd, "3v3") == 0) {
                power_cli_3v3(pipe, args);
                break;
            }
        }

        power_cli_command_print_usage();
    } while(false);

    furi_string_free(cmd);
}

void power_on_system_start(void) {
#ifdef SRV_CLI
    CliRegistry* registry = furi_record_open(RECORD_CLI);
    cli_registry_add_command(registry, "power", CliCommandFlagParallelSafe, power_cli, NULL);
    furi_record_close(RECORD_CLI);
#else
    UNUSED(power_cli);
#endif
}
