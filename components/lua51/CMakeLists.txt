
file(GLOB LUA_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
)
idf_component_register(
    SRCS ${LUA_SOURCES} "${PROJECT_DIR}/main/gc_probe.c"
    INCLUDE_DIRS "src" "${PROJECT_DIR}/main"
    REQUIRES esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=misleading-indentation
    -Wno-error=implicit-fallthrough
    -Wno-error=address
)
