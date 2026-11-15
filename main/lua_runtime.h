#pragma once

#include "lua.h"

extern lua_State *L;

void lua_runtime_init(void);
void lua_runtime_execute(const char *script);
