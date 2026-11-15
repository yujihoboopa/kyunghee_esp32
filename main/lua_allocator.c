#include "lua_allocator.h"
#include "esp_heap_caps.h"

static lua_heap_stats_t stats;
static lua_heap_stats_t *active_stats;

void lua_allocator_reset(void)
{
    stats = (lua_heap_stats_t){0};
}
const lua_heap_stats_t *lua_allocator_stats(void) { return active_stats ? active_stats : &stats; }

void *lua_allocator(
    void *ud,
    void *ptr,
    size_t osize,
    size_t nsize
) {
    lua_heap_stats_t *s = ud;
    active_stats = s;

    const uint32_t caps =
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    if (nsize == 0) {
        if (ptr != NULL) {
            heap_caps_free(ptr);
            if (s->current_bytes >= osize) s->current_bytes -= osize;
            s->free_count++;
        }
        return NULL;
    }

    void *new_ptr;

    if (ptr == NULL) {
        new_ptr = heap_caps_malloc(nsize, caps);

        if (new_ptr != NULL)
            s->alloc_count++;
    }
    else {
        new_ptr = heap_caps_realloc(ptr, nsize, caps);

        if (new_ptr != NULL)
            s->realloc_count++;
    }

    if (new_ptr == NULL) {
        s->failed_count++;
        return NULL;
    }

    s->current_bytes = (ptr == NULL ? s->current_bytes : s->current_bytes - osize) + nsize;

    if (s->current_bytes > s->peak_bytes)
        s->peak_bytes = s->current_bytes;

    return new_ptr;
}
