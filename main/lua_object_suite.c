#include "lua_object_suite.h"
#include "lua_runtime.h"
#include "lua_allocator.h"
#include "gc_probe.h"
#include "lauxlib.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#define OBJECT_TRIALS 30
#define OBJECT_CASES 7

typedef struct {
    const char *name;
    uint32_t count;
    uint64_t allocated_sum, reclaimed_sum, gc_sum;
    uint64_t allocated_max, reclaimed_max, gc_max;
    int64_t gc_samples[OBJECT_TRIALS];
    int64_t survivors_sum;
    uint32_t survivor_count;
} object_case_stats_t;

static object_case_stats_t cases[OBJECT_CASES] = {
    { .name="simple_table" }, { .name="nested_table" },
    { .name="closure_upvalue" }, { .name="weak_values" },
    { .name="full_userdata" }, { .name="userdata_finalizer" },
    { .name="c_api_stack_root" }
};

static uint32_t finalizer_calls;

static int case_index(const char *name)
{
    for (int i = 0; i < OBJECT_CASES; ++i)
        if (strcmp(name, cases[i].name) == 0) return i;
    return -1;
}

static int finalizer(lua_State *L)
{
    (void)L;
    finalizer_calls++;
    return 0;
}

static int make_finalizable(lua_State *L)
{
    int n = (int)luaL_checkinteger(L, 1);
    for (int i = 0; i < n; ++i) {
        lua_newuserdata(L, 24);
        luaL_getmetatable(L, "experiment.finalizable");
        lua_setmetatable(L, -2);
        lua_pop(L, 1);
    }
    return 0;
}

static int make_plain_userdata(lua_State *L)
{
    int n = (int)luaL_checkinteger(L, 1);
    for (int i = 0; i < n; ++i) {
        (void)lua_newuserdata(L, 24);
        lua_pop(L, 1);
    }
    return 0;
}

static int get_finalizer_calls(lua_State *L)
{
    lua_pushinteger(L, finalizer_calls);
    return 1;
}

static int run_gc_case(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int n = (int)luaL_checkinteger(L, 3);
    const lua_heap_stats_t *heap = lua_allocator_stats();
    size_t before_alloc = heap->current_bytes;

    lua_gc(L, LUA_GCSTOP, 0);
    lua_pushvalue(L, 2);
    lua_pushinteger(L, n);
    if (lua_pcall(L, 1, 0, 0) != 0) return lua_error(L);
    size_t before_gc = heap->current_bytes;
    int64_t t0 = esp_timer_get_time();
    lua_gc(L, LUA_GCCOLLECT, 0);
    uint64_t gc_us = (uint64_t)(esp_timer_get_time() - t0);
    size_t after_gc = heap->current_bytes;
    size_t allocated = before_gc > before_alloc ? before_gc - before_alloc : 0;
    size_t reclaimed = before_gc > after_gc ? before_gc - after_gc : 0;

    int survivors = -1;
    if (lua_isfunction(L, 4)) {
        lua_pushvalue(L, 4);
        if (lua_pcall(L, 0, 1, 0) == 0) {
            survivors = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
        } else {
            lua_pop(L, 1);
        }
    }
    int index = case_index(name);
    if (index >= 0 && cases[index].count < OBJECT_TRIALS) {
        object_case_stats_t *s = &cases[index];
        s->gc_samples[s->count++] = (int64_t)gc_us;
        s->allocated_sum += allocated; s->reclaimed_sum += reclaimed; s->gc_sum += gc_us;
        if (allocated > s->allocated_max) s->allocated_max = allocated;
        if (reclaimed > s->reclaimed_max) s->reclaimed_max = reclaimed;
        if (gc_us > s->gc_max) s->gc_max = gc_us;
        if (survivors >= 0) { s->survivors_sum += survivors; s->survivor_count++; }
    }
    lua_gc(L, LUA_GCRESTART, 0);
    return 0;
}

static int stack_root_case(lua_State *L)
{
    const int root = lua_gettop(L) + 1;
    lua_newtable(L);
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 128; ++i) {
        lua_newtable(L);
        lua_pushinteger(L, i);
        lua_rawseti(L, -2, 1);
        lua_pop(L, 1);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    uint64_t elapsed = (uint64_t)(esp_timer_get_time() - t0);
    int survived = lua_istable(L, root);
    object_case_stats_t *s = &cases[case_index("c_api_stack_root")];
    if (s->count < OBJECT_TRIALS) {
        s->gc_samples[s->count++] = (int64_t)elapsed;
        s->gc_sum += elapsed;
        if (elapsed > s->gc_max) s->gc_max = elapsed;
        s->survivors_sum += survived;
        s->survivor_count++;
    }
    return 0;
}

static void register_function(const char *name, lua_CFunction fn)
{
    lua_pushcfunction(L, fn);
    lua_setglobal(L, name);
}

void lua_object_suite_run(void)
{
    static const char script[] =
        "local N=96; gc_finalizer_baseline=0\n"
        "for trial=1,30 do\n"
        " gc_run_case('simple_table', function(n) local t={} for i=1,n do t[i]=i end end, N)\n"
        " gc_run_case('nested_table', function(n) local t={} for i=1,n do t[i]={child={i,i+1}} end end, N)\n"
        " gc_run_case('closure_upvalue', function(n) local t={} for i=1,n do local v=i; t[i]=function() return v end end; gc_kept_closure=t[1] end, N, function() return type(gc_kept_closure)=='function' and gc_kept_closure()==1 and 1 or 0 end)\n"
        " gc_run_case('weak_values', function(n) local w=setmetatable({}, {__mode='v'}); gc_weak=w; for i=1,n do w[i]={i} end end, N, function() local c=0; for _ in pairs(gc_weak) do c=c+1 end; return c end)\n"
        " gc_run_case('full_userdata', function(n) make_plain_userdata(n) end, N)\n"
        " gc_run_case('userdata_finalizer', function(n) make_finalizable(n) end, N, function() local c=finalizer_count(); local d=c-gc_finalizer_baseline; gc_finalizer_baseline=c; return d end)\n"
        " stack_root_case()\n"
        "end\n";

    luaL_newmetatable(L, "experiment.finalizable");
    lua_pushcfunction(L, finalizer);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    register_function("make_finalizable", make_finalizable);
    register_function("make_plain_userdata", make_plain_userdata);
    register_function("finalizer_count", get_finalizer_calls);
    register_function("gc_run_case", run_gc_case);
    register_function("stack_root_case", stack_root_case);
    finalizer_calls = 0;
    for (int i = 0; i < OBJECT_CASES; ++i) cases[i].count = 0;
    int status = luaL_dostring(L, script);
    if (status != 0) {
        printf("object_suite_error,%s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    for (int i = 0; i < OBJECT_CASES; ++i) {
        object_case_stats_t *s = &cases[i];
        if (!s->count) continue;
        for (uint32_t a = 0; a < s->count; ++a)
            for (uint32_t b = a + 1; b < s->count; ++b)
                if (s->gc_samples[b] < s->gc_samples[a]) {
                    int64_t tmp = s->gc_samples[a]; s->gc_samples[a] = s->gc_samples[b]; s->gc_samples[b] = tmp;
                }
        uint32_t p95 = (95 * s->count + 99) / 100 - 1;
        printf("object_case_summary,%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%lld\n",
               s->name, (unsigned)s->count,
               (unsigned long long)(s->allocated_sum / s->count),
               (unsigned long long)s->allocated_max,
               (unsigned long long)(s->reclaimed_sum / s->count),
               (unsigned long long)s->reclaimed_max,
               (unsigned long long)(s->gc_sum / s->count),
               (unsigned long long)s->gc_max,
               (long long)(s->survivor_count ? s->survivors_sum / s->survivor_count : -1));
        printf("object_gc_p95,%s,%lld\n", s->name, (long long)s->gc_samples[p95]);
    }
    gc_probe_dump_csv();
    gc_probe_reset();
}
