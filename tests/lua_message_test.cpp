#include "actor.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <cstdio>
#include <string>

int main() {
    nc::LuaHost host;
    std::string err;
    if (!host.open(err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }

    lua_State* L = host.L();
    const char* source =
        "return function(self, params) "
        "got_player=params.Player; got_score=params.TapNoteScore end";
    if (luaL_loadstring(L, source) != 0 || lua_pcall(L, 0, 1, 0) != 0) {
        std::fprintf(stderr, "compile: %s\n", lua_tostring(L, -1));
        return 1;
    }
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    nc::Actor actor;
    nc::LuaMessageParams params;
    params.player = "PlayerNumber_P2";
    params.tapNoteScore = "TapNoteScore_W1";
    if (!host.callChunk(ref, actor, 12.5, "JudgmentMessageCommand", err,
                        &params)) {
        std::fprintf(stderr, "call: %s\n", err.c_str());
        return 1;
    }

    lua_getglobal(L, "got_player");
    const char* player = lua_tostring(L, -1);
    const bool playerOk = player && std::string(player) == "PlayerNumber_P2";
    lua_pop(L, 1);
    lua_getglobal(L, "got_score");
    const char* score = lua_tostring(L, -1);
    const bool scoreOk = score && std::string(score) == "TapNoteScore_W1";
    lua_pop(L, 1);
    if (!playerOk || !scoreOk) {
        std::fprintf(stderr, "message params were not delivered\n");
        return 1;
    }
    return 0;
}
