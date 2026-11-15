#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t current_bytes;
    size_t peak_bytes;

    uint64_t alloc_count;
    uint64_t realloc_count;
    uint64_t free_count;
    uint64_t failed_count;
} lua_heap_stats_t;

void lua_allocator_reset(void);
const lua_heap_stats_t *lua_allocator_stats(void);

void *lua_allocator(
    void *ud,
    void *ptr,
    size_t osize,
    size_t nsize
);
