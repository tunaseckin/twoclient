#include "gl.h"

#include <EGL/egl.h>
#include <android/log.h>

#define TWOCLIENT_GL_DEFINE(type, name) type tc_##name;
TWOCLIENT_GL_FUNCS(TWOCLIENT_GL_DEFINE)

namespace gl {

bool load() {
    bool ok = true;
#define TWOCLIENT_GL_LOAD(type, name)                                                 \
    tc_##name = reinterpret_cast<type>(eglGetProcAddress(#name));                     \
    if (!tc_##name) {                                                                 \
        __android_log_print(ANDROID_LOG_ERROR, CLIENT_NAME, "missing GL function " #name); \
        ok = false;                                                                   \
    }
    TWOCLIENT_GL_FUNCS(TWOCLIENT_GL_LOAD)
    return ok;
}

}  // namespace gl
