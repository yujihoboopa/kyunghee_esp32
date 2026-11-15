#include "lua_runtime.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lua_allocator.h"
#include "esp_log.h"

static const char *TAG = "LUA";

lua_State *L;

static lua_heap_stats_t lua_stats;

void lua_runtime_init(void)
{
    L = lua_newstate(lua_allocator, &lua_stats);

    if (L == NULL) {
        ESP_LOGE(TAG, "Lua initialization failed");
        abort();
    }

    luaL_openlibs(L);

    lua_gc(L, LUA_GCSETPAUSE, 200);
    lua_gc(L, LUA_GCSETSTEPMUL, 200);

    ESP_LOGI(TAG, "Lua 5.1 initialized");
}

void lua_runtime_execute(const char *script)
{
    int status = luaL_dostring(L, script);

    if (status != 0) {
        ESP_LOGE(TAG, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}
