#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#include "ble_queue.h"

static const char *TAG = "BLE";
QueueHandle_t ble_queue;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t value_handle;
static uint8_t addr_type;
static uint32_t sent, failed;
static uint32_t processed;
static volatile bool notify_enabled;
static int gap_event(struct ble_gap_event *e, void *arg);

static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(0x91,0x72,0x53,0x34,0x15,0x26,0x47,0x38,0x89,0xaa,0xbb,0xcc,0xdd,0xee,0x00,0x01);
static const ble_uuid128_t char_uuid = BLE_UUID128_INIT(0x91,0x72,0x53,0x34,0x15,0x26,0x47,0x38,0x89,0xaa,0xbb,0xcc,0xdd,0xee,0x00,0x02);
static int access_cb(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)ah; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    return os_mbuf_append(ctxt->om, &now_us, sizeof(now_us)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
static const struct ble_gatt_chr_def chars[] = {{.uuid=&char_uuid.u,.access_cb=access_cb,.val_handle=&value_handle,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY},{0}};
static const struct ble_gatt_svc_def svcs[] = {{.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&service_uuid.u,.characteristics=chars},{0}};

static void advertise(void) {
    struct ble_hs_adv_fields f = {0};
    const char name[] = "LuaGC-ESP32";
    f.name = (uint8_t *)name; f.name_len = sizeof(name)-1; f.name_is_complete = 1;
    f.uuids128 = (ble_uuid128_t *)&service_uuid; f.num_uuids128 = 1; f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_gap_adv_params p = {0}; p.conn_mode = BLE_GAP_CONN_MODE_UND; p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}
static int gap_event(struct ble_gap_event *e, void *arg) {
    if (e->type == BLE_GAP_EVENT_CONNECT) {
        if (e->connect.status == 0) {
            conn_handle=e->connect.conn_handle;
            ESP_LOGI(TAG, "connected, handle=%u", (unsigned)conn_handle);
        } else {
            ESP_LOGW(TAG, "connection failed, status=%d", e->connect.status);
            advertise();
        }
    }
    else if (e->type == BLE_GAP_EVENT_DISCONNECT) {
        ESP_LOGI(TAG, "disconnected, reason=%d", e->disconnect.reason);
        conn_handle=BLE_HS_CONN_HANDLE_NONE;
        notify_enabled=false;
        advertise();
    }
    else if (e->type == BLE_GAP_EVENT_SUBSCRIBE) {
        notify_enabled = e->subscribe.cur_notify;
        ESP_LOGI(TAG, "notify subscription=%d", (int)notify_enabled);
    }
    return 0;
}
static void on_sync(void) { if (ble_hs_id_infer_auto(0,&addr_type)==0) advertise(); }
static void host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }

void ble_tx_task(void *arg) {
    ble_request_t req;
    for (;;) {
        if (xQueueReceive(ble_queue,&req,portMAX_DELAY)!=pdPASS) continue;
        int rc = (conn_handle == BLE_HS_CONN_HANDLE_NONE || !notify_enabled) ? BLE_HS_ENOTCONN : 0;
        struct os_mbuf *om = rc ? NULL : ble_hs_mbuf_from_flat(req.data,req.length);
        if (!rc && !om) rc = BLE_HS_ENOMEM;
        if (!rc) rc=ble_gatts_notify_custom(conn_handle,value_handle,om);
        if (rc) failed++; else sent++;
        processed++;
        if ((processed % 20) == 0)
            printf("ble_summary,%u,%u,%u,%u\n", (unsigned)processed,
                   (unsigned)sent, (unsigned)failed, (unsigned)notify_enabled);
    }
}
void ble_queue_init(void) {
    ble_queue=xQueueCreate(BLE_QUEUE_SIZE,sizeof(ble_request_t));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init(); ble_svc_gatt_init();
    ble_svc_gap_device_name_set("LuaGC-ESP32");
    ESP_ERROR_CHECK(ble_gatts_count_cfg(svcs)); ESP_ERROR_CHECK(ble_gatts_add_svcs(svcs));
    ble_hs_cfg.sync_cb=on_sync;
    nimble_port_freertos_init(host_task);
    xTaskCreatePinnedToCore(ble_tx_task,"ble_tx",4096,NULL,5,NULL,0);
}
