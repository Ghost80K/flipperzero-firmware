#include "app_common.h"

#include <core/check.h>
#include <core/thread.h>
#include <core/log.h>

#include <interface/patterns/ble_thread/tl/shci_tl.h>
#include <interface/patterns/ble_thread/tl/hci_tl.h>

#define TAG "BleEvt"

#define BLE_EVENT_THREAD_FLAG_SHCI_EVENT (1UL << 0)
#define BLE_EVENT_THREAD_FLAG_HCI_EVENT (1UL << 1)
#define BLE_EVENT_THREAD_FLAG_KILL_THREAD (1UL << 2)

#define BLE_EVENT_THREAD_FLAG_ALL                                         \
    (BLE_EVENT_THREAD_FLAG_SHCI_EVENT | BLE_EVENT_THREAD_FLAG_HCI_EVENT | \
     BLE_EVENT_THREAD_FLAG_KILL_THREAD)

static FuriThread* event_thread = NULL;

static int32_t ble_event_thread(void* context) {
    UNUSED(context);
    uint32_t flags = 0;

    while(true) {
        flags =
            furi_thread_flags_wait(BLE_EVENT_THREAD_FLAG_ALL, FuriFlagWaitAny, FuriWaitForever);
        if(flags & BLE_EVENT_THREAD_FLAG_SHCI_EVENT) {
            // FURI_LOG_W(TAG, "shci_user_evt_proc");
            shci_user_evt_proc();
        }
        if(flags & BLE_EVENT_THREAD_FLAG_HCI_EVENT) {
            // FURI_LOG_W(TAG, "hci_user_evt_proc");
            hci_user_evt_proc();
        }
        if(flags & BLE_EVENT_THREAD_FLAG_KILL_THREAD) {
            break;
        }
    }

    return 0;
}

void shci_notify_asynch_evt(void* pdata) {
    UNUSED(pdata);
    if(!event_thread) {
        return;
    }

    // FURI_LOG_W(TAG, "shci_notify_asynch_evt");
    FuriThreadId thread_id = furi_thread_get_id(event_thread);
    furi_assert(thread_id);
    furi_thread_flags_set(thread_id, BLE_EVENT_THREAD_FLAG_SHCI_EVENT);
}

void hci_notify_asynch_evt(void* pdata) {
    UNUSED(pdata);
    furi_check(event_thread);
    // FURI_LOG_W(TAG, "hci_notify_asynch_evt");
    FuriThreadId thread_id = furi_thread_get_id(event_thread);
    furi_assert(thread_id);
    furi_thread_flags_set(thread_id, BLE_EVENT_THREAD_FLAG_HCI_EVENT);
}

void ble_event_thread_stop() {
    if(!event_thread) return;

    FuriThreadId thread_id = furi_thread_get_id(event_thread);
    furi_assert(thread_id);
    furi_thread_flags_set(thread_id, BLE_EVENT_THREAD_FLAG_KILL_THREAD);
    furi_thread_join(event_thread);
    furi_thread_free(event_thread);
    event_thread = NULL;
}

void ble_event_thread_start() {
    furi_check(event_thread == NULL);

    event_thread = furi_thread_alloc_ex("BleEventWorker", 1024, ble_event_thread, NULL);
    furi_thread_set_priority(event_thread, FuriThreadPriorityHigh);
    furi_thread_start(event_thread);
}
