#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define BLE_DATA_SIZE 20
#define BLE_QUEUE_SIZE 16

typedef struct {
    uint32_t seq;

    int64_t scheduled_us;
    int64_t lua_call_us;

    uint16_t length;
    uint8_t data[BLE_DATA_SIZE];

} ble_request_t;

extern QueueHandle_t ble_queue;

void ble_queue_init(void);
