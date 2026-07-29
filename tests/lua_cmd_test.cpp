#include <cstdio>

extern "C" {
#include "lauxlib.h"
#include "lualib.h"
}

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    const char* script =
        "local calls = {}\n"
        "local actor = {}\n"
        "function actor:x(v) calls[#calls+1] = {'x', v}; return self end\n"
        "function actor:y(v) calls[#calls+1] = {'y', v}; return self end\n"
        "actor['cmd'] = function(self, v) calls[#calls+1] = {'cmd', v}; return self end\n"
        "function actor:visible(v) calls[#calls+1] = {'visible', v}; return self end\n"
        "function actor:playcommand() calls[#calls+1] = {'playcommand'}; return self end\n"
        "local command = cmd(x,12;y,-4;visible,false;playcommand)\n"
        "assert(type(command) == 'function')\n"
        "command(actor)\n"
        "assert(#calls == 4)\n"
        "assert(calls[1][1] == 'x' and calls[1][2] == 12)\n"
        "assert(calls[2][1] == 'y' and calls[2][2] == -4)\n"
        "assert(calls[3][1] == 'visible' and calls[3][2] == false)\n"
        "assert(calls[4][1] == 'playcommand')\n";

    const char* methodScript =
        "local actor = {}\n"
        "actor['cmd'] = function(self, v) self.value = v; return self end\n"
        "actor:cmd('x,12')\n"
        "assert(actor.value == 'x,12')\n";

    const int rc = luaL_dostring(L, script);
    if (rc != 0) {
        std::fprintf(stderr, "%s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    const int methodRc = luaL_dostring(L, methodScript);
    if (methodRc != 0) {
        std::fprintf(stderr, "%s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    lua_close(L);
    return 0;
}
