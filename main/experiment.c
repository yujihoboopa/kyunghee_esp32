#include "experiment.h"
#include "lua_runtime.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "gc_probe.h"
#include "lua_allocator.h"
#include "ble_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define LUA_PERIOD_MS 100
#define PROBE_PERIOD_MS 10
#define LUA_WINDOW 20
#define PROBE_WINDOW 100
#define LUA_SAMPLES_PER_PROFILE 100
#define PROBE_SAMPLES_PER_PROFILE 1000

static const char *TAG = "EXPERIMENT";
typedef struct {
    const char *name;
    uint8_t lua_core, probe_core, load;
    uint16_t pause, stepmul;
    bool forced_full_gc;
} run_profile_t;
typedef struct {
    const run_profile_t *profile;
    SemaphoreHandle_t done;
} worker_context_t;

static const run_profile_t profiles[] = {
    {"baseline_same_core0", 0, 0, 0, 200, 200, false},
    {"baseline_same_core1", 1, 1, 0, 200, 200, false},
    {"baseline_lua0_probe1", 0, 1, 0, 200, 200, false},
    {"baseline_lua1_probe0", 1, 0, 0, 200, 200, false},
    {"auto_gc_same_core0", 0, 0, 2, 200, 200, false},
    {"auto_gc_same_core1", 1, 1, 2, 200, 200, false},
    {"auto_gc_lua0_probe1", 0, 1, 2, 200, 200, false},
    {"auto_gc_lua1_probe0", 1, 0, 2, 200, 200, false},
    {"auto_gc_high_load_default", 1, 1, 3, 200, 200, false},
    {"pause100_high_load", 1, 1, 3, 100, 200, false},
    {"stepmul400_high_load", 1, 1, 3, 200, 400, false},
    {"forced_full_gc_high_load", 1, 1, 3, 200, 200, true},
};

static int compare_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static void print_window(const char *name, int64_t *samples, size_t n)
{
    if (!n) return;
    int64_t sorted[PROBE_WINDOW];
    int64_t min = samples[0], max = samples[0], sum = 0;
    for (size_t i = 0; i < n; ++i) {
        sorted[i] = samples[i]; sum += samples[i];
        if (samples[i] < min) min = samples[i];
        if (samples[i] > max) max = samples[i];
    }
    qsort(sorted, n, sizeof(sorted[0]), compare_i64);
    size_t p95_index = (95 * n + 99) / 100 - 1;
    printf("%s,%u,%lld,%lld,%lld,%lld\n", name, (unsigned)n,
           (long long)min, (long long)(sum / (int64_t)n),
           (long long)sorted[p95_index], (long long)max);
}

static void lua_worker(void *arg)
{
    const worker_context_t *ctx = arg;
    const run_profile_t *p = ctx->profile;
    const TickType_t period = pdMS_TO_TICKS(LUA_PERIOD_MS);
    const TickType_t tick_base = xTaskGetTickCount();
    const int64_t time_base = esp_timer_get_time();
    TickType_t last_wake = tick_base;
    int64_t offset_us = 0;
    int64_t lateness[LUA_WINDOW], call_time[LUA_WINDOW];
    size_t n = 0;

    lua_gc(L, LUA_GCSETPAUSE, p->pause);
    lua_gc(L, LUA_GCSETSTEPMUL, p->stepmul);
    printf("condition,%s,%u,%u,%u,%u,%u,%u\n", p->name,
           (unsigned)p->lua_core, (unsigned)p->probe_core, (unsigned)p->load,
           (unsigned)p->pause, (unsigned)p->stepmul, (unsigned)p->forced_full_gc);

    for (uint32_t seq = 0; seq < LUA_SAMPLES_PER_PROFILE; ++seq) {
        vTaskDelayUntil(&last_wake, period);
        int64_t scheduled_us = time_base +
            (int64_t)(int32_t)(last_wake - tick_base) * portTICK_PERIOD_MS * 1000;
        int64_t actual_us = esp_timer_get_time();
        int64_t raw_lateness = actual_us - scheduled_us;
        if (seq == 0) offset_us = raw_lateness;
        lateness[n] = raw_lateness - offset_us;

        int64_t lua_start_us = esp_timer_get_time();
        lua_getglobal(L, "sensor_tick");
        lua_pushinteger(L, seq);
        lua_pushinteger(L, p->load);
        lua_pushboolean(L, p->forced_full_gc);
        int status = lua_pcall(L, 3, 0, 0);
        call_time[n] = esp_timer_get_time() - lua_start_us;
        if (status != 0) {
            ESP_LOGE(TAG, "Lua error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }

        ble_request_t req = {0};
        req.seq = seq;
        req.scheduled_us = scheduled_us;
        req.lua_call_us = esp_timer_get_time();
        req.data[0] = (uint8_t)(p - profiles);
        memcpy(&req.data[1], &req.seq, sizeof(req.seq));
        memcpy(&req.data[5], &req.lua_call_us, sizeof(req.lua_call_us));
        req.length = 13;
        if (xQueueSend(ble_queue, &req, 0) != pdPASS) printf("ble_queue_full,%u\n", (unsigned)seq);

        if (++n == LUA_WINDOW) {
            print_window("task_lateness_us", lateness, n);
            print_window("lua_call_us", call_time, n);
            n = 0;
            const lua_heap_stats_t *s = lua_allocator_stats();
            printf("heap_summary,%u,%u,%u,%u\n", (unsigned)s->current_bytes,
                   (unsigned)s->peak_bytes, (unsigned)s->alloc_count, (unsigned)s->failed_count);
            gc_probe_dump_csv();
            gc_probe_reset();
        }
    }
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static void probe_worker(void *arg)
{
    const worker_context_t *ctx = arg;
    const run_profile_t *p = ctx->profile;
    const TickType_t period = pdMS_TO_TICKS(PROBE_PERIOD_MS);
    const TickType_t tick_base = xTaskGetTickCount();
    const int64_t time_base = esp_timer_get_time();
    TickType_t last_wake = tick_base;
    int64_t offset_us = 0;
    int64_t samples[PROBE_WINDOW];
    size_t n = 0;

    for (uint32_t seq = 0; seq < PROBE_SAMPLES_PER_PROFILE; ++seq) {
        vTaskDelayUntil(&last_wake, period);
        int64_t scheduled_us = time_base +
            (int64_t)(int32_t)(last_wake - tick_base) * portTICK_PERIOD_MS * 1000;
        int64_t raw_lateness = esp_timer_get_time() - scheduled_us;
        if (seq == 0) offset_us = raw_lateness;
        samples[n++] = raw_lateness - offset_us;
        if (n == PROBE_WINDOW) {
            print_window("probe_lateness_us", samples, n);
            printf("probe_condition,%s\n", p->name);
            n = 0;
        }
    }
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

void experiment_task(void *arg)
{
    const size_t profile_count = sizeof(profiles) / sizeof(profiles[0]);
    for (;;) {
        for (size_t i = 0; i < profile_count; ++i) {
            SemaphoreHandle_t done = xSemaphoreCreateCounting(2, 0);
            if (!done) {
                ESP_LOGE(TAG, "failed to create profile semaphore");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            worker_context_t ctx = { .profile = &profiles[i], .done = done };
            TaskHandle_t lua_handle = NULL, probe_handle = NULL;
            BaseType_t a = xTaskCreatePinnedToCore(lua_worker, "lua_profile", 8192,
                &ctx, 4, &lua_handle, profiles[i].lua_core);
            BaseType_t b = xTaskCreatePinnedToCore(probe_worker, "probe_profile", 4096,
                &ctx, 6, &probe_handle, profiles[i].probe_core);
            if (a != pdPASS || b != pdPASS) {
                ESP_LOGE(TAG, "failed to start profile %s", profiles[i].name);
                if (a == pdPASS) vTaskDelete(lua_handle);
                if (b == pdPASS) vTaskDelete(probe_handle);
                vSemaphoreDelete(done);
                continue;
            }
            xSemaphoreTake(done, portMAX_DELAY);
            xSemaphoreTake(done, portMAX_DELAY);
            vSemaphoreDelete(done);
        }
    }
}
