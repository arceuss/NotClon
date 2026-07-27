// Definitions for the entry points declared extern in gl.h.
//
// This lives in the core library rather than in a main() so that the encoder
// and the editor resolve the same pointers exactly once each.
#include "gl.h"

#define X(type, name) type name = nullptr;
NC_GL_FUNCS
#undef X

bool nc_load_gl() {
    bool ok = true;
#define X(type, name)                                                      \
    name = (type)wglGetProcAddress(#name);                                 \
    if (!name) { fprintf(stderr, "missing GL entry point: %s\n", #name); ok = false; }
    NC_GL_FUNCS
#undef X
    return ok;
}
