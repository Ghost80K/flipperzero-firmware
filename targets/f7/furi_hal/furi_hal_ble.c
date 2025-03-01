#include "ble_system.h"
#include <core/check.h>
#include <gap.h>
#include <furi_hal_ble.h>
#include <furi_ble/profile_interface.h>

#include <ble/ble.h>
#include <interface/patterns/ble_thread/shci/shci.h>

#include <stdbool.h>
#include <stm32wbxx.h>
#include <stm32wbxx_ll_hsem.h>

#include <hsem_map.h>

#include <furi_hal_version.h>
#include <furi_hal_power.h>
#include <furi_hal_bus.c>
#include <services/battery_service.h>
#include <furi.h>

#define TAG "FuriHalBt"

#define furi_hal_ble_DEFAULT_MAC_ADDR \
    { 0x6c, 0x7a, 0xd8, 0xac, 0x57, 0x72 }

/* Time, in ms, to wait for mode transition before crashing */
#define C2_MODE_SWITCH_TIMEOUT 10000

typedef struct {
    FuriMutex* core2_mtx;
    FuriHalBtStack stack;
} FuriHalBt;

static FuriHalBt furi_hal_ble = {
    .core2_mtx = NULL,
    .stack = FuriHalBtStackUnknown,
};

void furi_hal_ble_init() {
    FURI_LOG_I(TAG, "Start BT initialization");
    furi_hal_bus_enable(FuriHalBusHSEM);
    furi_hal_bus_enable(FuriHalBusIPCC);
    furi_hal_bus_enable(FuriHalBusAES2);
    furi_hal_bus_enable(FuriHalBusPKA);
    furi_hal_bus_enable(FuriHalBusCRC);

    if(!furi_hal_ble.core2_mtx) {
        furi_hal_ble.core2_mtx = furi_mutex_alloc(FuriMutexTypeNormal);
        furi_assert(furi_hal_ble.core2_mtx);
    }

    // Explicitly tell that we are in charge of CLK48 domain
    furi_check(LL_HSEM_1StepLock(HSEM, CFG_HW_CLK48_CONFIG_SEMID) == 0);

    // Start Core2
    ble_system_init();
}

void furi_hal_ble_lock_core2() {
    furi_assert(furi_hal_ble.core2_mtx);
    furi_check(furi_mutex_acquire(furi_hal_ble.core2_mtx, FuriWaitForever) == FuriStatusOk);
}

void furi_hal_ble_unlock_core2() {
    furi_assert(furi_hal_ble.core2_mtx);
    furi_check(furi_mutex_release(furi_hal_ble.core2_mtx) == FuriStatusOk);
}

static bool furi_hal_ble_radio_stack_is_supported(const BleGlueC2Info* info) {
    bool supported = false;
    if(info->StackType == INFO_STACK_TYPE_BLE_LIGHT) {
        if(info->VersionMajor >= FURI_HAL_BLE_STACK_VERSION_MAJOR &&
           info->VersionMinor >= FURI_HAL_BLE_STACK_VERSION_MINOR) {
            furi_hal_ble.stack = FuriHalBtStackLight;
            supported = true;
        }
    } else if(info->StackType == INFO_STACK_TYPE_BLE_FULL) {
        if(info->VersionMajor >= FURI_HAL_BLE_STACK_VERSION_MAJOR &&
           info->VersionMinor >= FURI_HAL_BLE_STACK_VERSION_MINOR) {
            furi_hal_ble.stack = FuriHalBtStackFull;
            supported = true;
        }
    } else {
        furi_hal_ble.stack = FuriHalBtStackUnknown;
    }
    return supported;
}

bool furi_hal_ble_start_radio_stack() {
    bool res = false;
    furi_assert(furi_hal_ble.core2_mtx);

    furi_mutex_acquire(furi_hal_ble.core2_mtx, FuriWaitForever);

    // Explicitly tell that we are in charge of CLK48 domain
    furi_check(LL_HSEM_1StepLock(HSEM, CFG_HW_CLK48_CONFIG_SEMID) == 0);

    do {
        // Wait until C2 is started or timeout
        if(!ble_system_wait_for_c2_start(FURI_HAL_BLE_C2_START_TIMEOUT)) {
            FURI_LOG_E(TAG, "Core2 start failed");
            break;
        }

        // If C2 is running, start radio stack fw
        if(!furi_hal_ble_ensure_c2_mode(BleGlueC2ModeStack)) {
            break;
        }

        // Check whether we support radio stack
        const BleGlueC2Info* c2_info = ble_system_get_c2_info();
        if(!furi_hal_ble_radio_stack_is_supported(c2_info)) {
            FURI_LOG_E(TAG, "Unsupported radio stack");
            // Don't stop SHCI for crypto enclave support
            break;
        }
        // Starting radio stack
        if(!ble_system_start()) {
            FURI_LOG_E(TAG, "Failed to start radio stack");
            ble_stack_deinit();
            ble_system_stop();
            break;
        }
        res = true;
    } while(false);
    furi_mutex_release(furi_hal_ble.core2_mtx);

    return res;
}

FuriHalBtStack furi_hal_ble_get_radio_stack() {
    return furi_hal_ble.stack;
}

bool furi_hal_ble_is_gatt_gap_supported() {
    if(furi_hal_ble.stack == FuriHalBtStackLight || furi_hal_ble.stack == FuriHalBtStackFull) {
        return true;
    } else {
        return false;
    }
}

bool furi_hal_ble_is_testing_supported() {
    if(furi_hal_ble.stack == FuriHalBtStackFull) {
        return true;
    } else {
        return false;
    }
}

static FuriHalBleProfileBase* current_profile = NULL;
static GapConfig current_config = {0};

bool furi_hal_ble_check_profile_type(
    FuriHalBleProfileBase* profile,
    const FuriHalBleProfileConfig* config) {
    if(!profile || !config) {
        return false;
    }

    return profile->config == config;
}

FuriHalBleProfileBase* furi_hal_ble_start_app(
    const FuriHalBleProfileConfig* profile_config,
    GapEventCallback event_cb,
    void* context) {
    furi_assert(event_cb);
    furi_check(profile_config);
    furi_check(current_profile == NULL);

    do {
        if(!ble_system_is_radio_stack_ready()) {
            FURI_LOG_E(TAG, "Can't start BLE App - radio stack did not start");
            break;
        }
        if(!furi_hal_ble_is_gatt_gap_supported()) {
            FURI_LOG_E(TAG, "Can't start Ble App - unsupported radio stack");
            break;
        }

        profile_config->get_gap_config(&current_config);

        if(!gap_init(&current_config, event_cb, context)) {
            gap_thread_stop();
            FURI_LOG_E(TAG, "Failed to init GAP");
            break;
        }
        // Start selected profile services
        if(furi_hal_ble_is_gatt_gap_supported()) {
            current_profile = profile_config->start();
        }
    } while(false);

    return current_profile;
}

void furi_hal_ble_reinit() {
    furi_hal_power_insomnia_enter();
    FURI_LOG_I(TAG, "Disconnect and stop advertising");
    furi_hal_ble_stop_advertising();

    if(current_profile) {
        FURI_LOG_I(TAG, "Stop current profile services");
        current_profile->config->stop(current_profile);
        current_profile = NULL;
    }

    // Magic happens here
    hci_reset();

    FURI_LOG_I(TAG, "Stop BLE related RTOS threads");
    gap_thread_stop();
    ble_stack_deinit();

    FURI_LOG_I(TAG, "Reset SHCI");
    furi_check(ble_system_reinit_c2());
    ble_system_stop();

    // enterprise delay
    furi_delay_ms(100);

    furi_hal_bus_disable(FuriHalBusHSEM);
    furi_hal_bus_disable(FuriHalBusIPCC);
    furi_hal_bus_disable(FuriHalBusAES2);
    furi_hal_bus_disable(FuriHalBusPKA);
    furi_hal_bus_disable(FuriHalBusCRC);

    furi_hal_ble_init();
    furi_hal_ble_start_radio_stack();
    furi_hal_power_insomnia_exit();
}

FuriHalBleProfileBase* furi_hal_ble_change_app(
    const FuriHalBleProfileConfig* profile_config,
    GapEventCallback event_cb,
    void* context) {
    furi_assert(event_cb);

    furi_hal_ble_reinit();
    return furi_hal_ble_start_app(profile_config, event_cb, context);
}

bool furi_hal_ble_is_active() {
    return gap_get_state() > GapStateIdle;
}

void furi_hal_ble_start_advertising() {
    if(gap_get_state() == GapStateIdle) {
        gap_start_advertising();
    }
}

void furi_hal_ble_stop_advertising() {
    if(furi_hal_ble_is_active()) {
        gap_stop_advertising();
        while(furi_hal_ble_is_active()) {
            furi_delay_tick(1);
        }
    }
}

void furi_hal_ble_update_battery_level(uint8_t battery_level) {
    ble_svc_battery_state_update(&battery_level, NULL);
}

void furi_hal_ble_update_power_state(bool charging) {
    ble_svc_battery_state_update(NULL, &charging);
}

void furi_hal_ble_get_key_storage_buff(uint8_t** key_buff_addr, uint16_t* key_buff_size) {
    ble_stack_get_key_storage_buff(key_buff_addr, key_buff_size);
}

void furi_hal_ble_set_key_storage_change_callback(
    BleGlueKeyStorageChangedCallback callback,
    void* context) {
    furi_assert(callback);
    ble_system_set_key_storage_changed_callback(callback, context);
}

void furi_hal_ble_nvm_sram_sem_acquire() {
    while(LL_HSEM_1StepLock(HSEM, CFG_HW_BLE_NVM_SRAM_SEMID)) {
        furi_thread_yield();
    }
}

void furi_hal_ble_nvm_sram_sem_release() {
    LL_HSEM_ReleaseLock(HSEM, CFG_HW_BLE_NVM_SRAM_SEMID, 0);
}

bool furi_hal_ble_clear_white_list() {
    furi_hal_ble_nvm_sram_sem_acquire();
    tBleStatus status = aci_gap_clear_security_db();
    if(status) {
        FURI_LOG_E(TAG, "Clear while list failed with status %d", status);
    }
    furi_hal_ble_nvm_sram_sem_release();
    return status != BLE_STATUS_SUCCESS;
}

void furi_hal_ble_dump_state(FuriString* buffer) {
    if(furi_hal_ble_is_alive()) {
        uint8_t HCI_Version;
        uint16_t HCI_Revision;
        uint8_t LMP_PAL_Version;
        uint16_t Manufacturer_Name;
        uint16_t LMP_PAL_Subversion;

        tBleStatus ret = hci_read_local_version_information(
            &HCI_Version, &HCI_Revision, &LMP_PAL_Version, &Manufacturer_Name, &LMP_PAL_Subversion);

        furi_string_cat_printf(
            buffer,
            "Ret: %d, HCI_Version: %d, HCI_Revision: %d, LMP_PAL_Version: %d, Manufacturer_Name: %d, LMP_PAL_Subversion: %d",
            ret,
            HCI_Version,
            HCI_Revision,
            LMP_PAL_Version,
            Manufacturer_Name,
            LMP_PAL_Subversion);
    } else {
        furi_string_cat_printf(buffer, "BLE not ready");
    }
}

bool furi_hal_ble_is_alive() {
    return ble_system_is_alive();
}

void furi_hal_ble_start_tone_tx(uint8_t channel, uint8_t power) {
    aci_hal_set_tx_power_level(0, power);
    aci_hal_tone_start(channel, 0);
}

void furi_hal_ble_stop_tone_tx() {
    aci_hal_tone_stop();
}

void furi_hal_ble_start_packet_tx(uint8_t channel, uint8_t pattern, uint8_t datarate) {
    hci_le_enhanced_transmitter_test(channel, 0x25, pattern, datarate);
}

void furi_hal_ble_start_packet_rx(uint8_t channel, uint8_t datarate) {
    hci_le_enhanced_receiver_test(channel, datarate, 0);
}

uint16_t furi_hal_ble_stop_packet_test() {
    uint16_t num_of_packets = 0;
    hci_le_test_end(&num_of_packets);
    return num_of_packets;
}

void furi_hal_ble_start_rx(uint8_t channel) {
    aci_hal_rx_start(channel);
}

float furi_hal_ble_get_rssi() {
    float val;
    uint8_t rssi_raw[3];

    if(aci_hal_read_raw_rssi(rssi_raw) != BLE_STATUS_SUCCESS) {
        return 0.0f;
    }

    // Some ST magic with rssi
    uint8_t agc = rssi_raw[2] & 0xFF;
    int rssi = (((int)rssi_raw[1] << 8) & 0xFF00) + (rssi_raw[0] & 0xFF);
    if(rssi == 0 || agc > 11) {
        val = -127.0;
    } else {
        val = agc * 6.0f - 127.0f;
        while(rssi > 30) {
            val += 6.0;
            rssi >>= 1;
        }
        val += (float)((417 * rssi + 18080) >> 10);
    }
    return val;
}

uint32_t furi_hal_ble_get_transmitted_packets() {
    uint32_t packets = 0;
    aci_hal_le_tx_test_packet_number(&packets);
    return packets;
}

void furi_hal_ble_stop_rx() {
    aci_hal_rx_stop();
}

bool furi_hal_ble_ensure_c2_mode(BleGlueC2Mode mode) {
    BleGlueCommandResult fw_start_res = ble_system_force_c2_mode(mode);
    if(fw_start_res == BleGlueCommandResultOK) {
        return true;
    } else if(fw_start_res == BleGlueCommandResultRestartPending) {
        // Do nothing and wait for system reset
        furi_delay_ms(C2_MODE_SWITCH_TIMEOUT);
        furi_crash("Waiting for FUS->radio stack transition");
        return true;
    }

    FURI_LOG_E(TAG, "Failed to switch C2 mode: %d", fw_start_res);
    return false;
}
