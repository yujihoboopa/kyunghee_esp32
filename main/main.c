#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lua_runtime.h"
#include "experiment.h"
#include "ble_queue.h"
#include "lua_object_suite.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "SETUP";

static void experiment_setup_task(void *arg)
{
    (void)arg;
    lua_runtime_init();
    ble_queue_init();

    lua_runtime_execute(
        "slots = {}\n"

        "for i = 1, 64 do\n"
        "    slots[i] = {x=0, y=0}\n"
        "end\n"

        "function sensor_tick(seq, mode, force_full)\n"
        "    if mode > 0 then\n"
        "        local garbage={}\n"
        "        local count = mode == 1 and 16 or (mode == 2 and 64 or 128)\n"
        "        for i=1,count do garbage[i]={value=seq*count+i, child={i,i+1}} end\n"
        "    end\n"
        "    for i=1,64 do local n=seq*64+i; slots[i].x=n; slots[i].y=n*2 end\n"
        "    if force_full and seq % 20 == 19 then collectgarbage('collect') end\n"
        "end\n"
    );

    lua_object_suite_run();

    BaseType_t result = xTaskCreatePinnedToCore(
        experiment_task,
        "profile_controller",
        8192,
        NULL,
        4,
        NULL,
        1
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "failed to create profile controller task");
        abort();
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    BaseType_t result = xTaskCreatePinnedToCore(
        experiment_setup_task,
        "experiment_setup",
        16384,
        NULL,
        5,
        NULL,
        1
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "failed to create setup task");
        abort();
    }
}
