idf_component_register(
    SRCS
        "main.c"
        "lua_runtime.c"
        "lua_allocator.c"
        "lua_object_suite.c"
        "experiment.c"
        "ble_service.c"

    INCLUDE_DIRS "."

    REQUIRES
        lua51
        bt
        nvs_flash
        esp_timer
)
