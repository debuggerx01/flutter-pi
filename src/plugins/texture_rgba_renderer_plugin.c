#include "texture_rgba_renderer_plugin.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"

#ifndef GL_RGBA8_OES
    #define GL_RGBA8_OES 0x8058
#endif

// 保存 engine 引用及方法指针表
static FlutterEngine global_engine = NULL;
static FlutterEngineProcTable *g_procs = NULL;

struct std_value_hack {
    int type;
    union {
        bool bool_value;
        int32_t int32_value;
        int64_t int64_value;
        double float64_value;
        char *string_value;
        struct {
            const uint8_t *data;
            size_t size;
        } array_data;
    };
};
#define AS_HACK(v) ((struct std_value_hack *) (v))

typedef struct texture_data {
    int64_t key;
    int width;
    int height;
    uint8_t *buffer;
    size_t buffer_size;
    GLuint gl_texture_id;
    bool is_dirty;
    struct texture_data *next;
} texture_data_t;

static texture_data_t *textures_head = NULL;
static pthread_mutex_t texture_mutex = PTHREAD_MUTEX_INITIALIZER;

static texture_data_t *find_texture(const int64_t key) {
    texture_data_t *curr = textures_head;
    while (curr != NULL) {
        if (curr->key == key)
            return curr;
        curr = curr->next;
    }
    return NULL;
}

static int64_t create_texture(const int64_t key) {
    pthread_mutex_lock(&texture_mutex);
    if (find_texture(key) == NULL) {
        texture_data_t *tex = calloc(1, sizeof(texture_data_t));
        tex->key = key;
        tex->next = textures_head;
        textures_head = tex;
    }
    pthread_mutex_unlock(&texture_mutex);
    return key;
}

static bool close_texture(const int64_t key) {
    pthread_mutex_lock(&texture_mutex);
    texture_data_t **curr = &textures_head;
    while (*curr != NULL) {
        if ((*curr)->key == key) {
            texture_data_t *to_delete = *curr;
            *curr = to_delete->next;

            if (to_delete->gl_texture_id != 0) {
                glDeleteTextures(1, &to_delete->gl_texture_id);
            }
            if (to_delete->buffer != NULL) {
                free(to_delete->buffer);
            }
            free(to_delete);
            pthread_mutex_unlock(&texture_mutex);
            return true;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&texture_mutex);
    return false;
}

static bool on_rgba(const int64_t key, const uint8_t *data, const size_t length, const int width, const int height) {
    pthread_mutex_lock(&texture_mutex);
    texture_data_t *tex = find_texture(key);
    if (tex != NULL) {
        tex->width = width;
        tex->height = height;
        if (tex->buffer_size < length) {
            tex->buffer = (uint8_t *) realloc(tex->buffer, length);
            tex->buffer_size = length;
        }
        if (tex->buffer != NULL && data != NULL) {
            memcpy(tex->buffer, data, length);
            tex->is_dirty = true;
        }
        pthread_mutex_unlock(&texture_mutex);
        return true;
    }
    pthread_mutex_unlock(&texture_mutex);
    return false;
}

bool rgba_renderer_texture_callback(
    const void *user_data,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
) {
    (void) user_data;
    (void) width;
    (void) height;

    pthread_mutex_lock(&texture_mutex);
    texture_data_t *tex = find_texture(texture_id);
    if (tex == NULL) {
        pthread_mutex_unlock(&texture_mutex);
        return false;
    }

    if (tex->gl_texture_id == 0) {
        glGenTextures(1, &tex->gl_texture_id);
        glBindTexture(GL_TEXTURE_2D, tex->gl_texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex->gl_texture_id);
    }

    if (tex->is_dirty && tex->buffer != NULL) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->width, tex->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex->buffer);
        tex->is_dirty = false;
    }

    texture_out->target = GL_TEXTURE_2D;
    texture_out->name = tex->gl_texture_id;
    texture_out->format = GL_RGBA8_OES;

    pthread_mutex_unlock(&texture_mutex);
    return true;
}

// 暴露出 FFI 供 Dart 侧的高性能 Native.onRgba 调用
#ifdef __cplusplus
extern "C" {
#endif

// 通过 __attribute__((visibility("default"))) __attribute__((used)) 保证符号被导出，不被优化掉
__attribute__((visibility("default"))) __attribute__((used)) void
FlutterRgbaRendererPluginOnRgba(void *texture_rgba_ptr, const uint8_t *buffer, int len, int width, int height, int stride_align) {
    (void) stride_align;
    // 我们将 key 直接当作 pointer 地址传给 Dart，这里再将其转回 key
    const int64_t key = (intptr_t) texture_rgba_ptr;

    const bool result = on_rgba(key, buffer, (size_t) len, width, height);
    if (result && global_engine && g_procs) {
        g_procs->MarkExternalTextureFrameAvailable(global_engine, key);
    }
}

#ifdef __cplusplus
}
#endif

// Platform Channel Method Call 处理
static int on_method_call(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) channel;

    const char *method_name = object->method;
    struct std_value *args = &object->std_arg;

    if (strcmp(method_name, "createTexture") == 0) {
        struct std_value *key_val = stdmap_get_str(args, (char *) "key");
        int64_t key = 0;
        if (key_val)
            key = AS_HACK(key_val)->type == 3 ? AS_HACK(key_val)->int32_value : AS_HACK(key_val)->int64_value;

        const int64_t tex_id = create_texture(key);

        if (global_engine && g_procs) {
            g_procs->RegisterExternalTexture(global_engine, tex_id);
        }

        uint8_t payload[10] = { 0x00, 0x04 };
        memcpy(payload + 2, &tex_id, 8);

        if (global_engine && g_procs) {
            g_procs->SendPlatformMessageResponse(global_engine, responsehandle, payload, 10);
        }
        return 0;
    }
    if (strcmp(method_name, "closeTexture") == 0) {
        struct std_value *key_val = stdmap_get_str(args, (char *) "key");
        int64_t key = 0;
        if (key_val)
            key = (AS_HACK(key_val)->type == 3) ? AS_HACK(key_val)->int32_value : AS_HACK(key_val)->int64_value;

        if (global_engine && g_procs) {
            g_procs->UnregisterExternalTexture(global_engine, key);
        }
        const bool result = close_texture(key);

        const uint8_t payload[2] = { 0x00, result ? 0x01 : 0x02 };
        if (global_engine && g_procs) {
            g_procs->SendPlatformMessageResponse(global_engine, responsehandle, payload, 2);
        }
        return 0;

        // 【新增】处理 Dart 侧请求底层指针的需求
    }
    if (strcmp(method_name, "getTexturePtr") == 0) {
        struct std_value *key_val = stdmap_get_str(args, (char *) "key");
        int64_t key = 0;
        if (key_val)
            key = AS_HACK(key_val)->type == 3 ? AS_HACK(key_val)->int32_value : AS_HACK(key_val)->int64_value;

        // 直接将 key 当作指针返回 (安全，因为 key 是唯一的且我们在上面做了映射)
        uint8_t payload[10] = { 0x00, 0x04 };
        memcpy(payload + 2, &key, 8);

        if (global_engine && g_procs) {
            g_procs->SendPlatformMessageResponse(global_engine, responsehandle, payload, 10);
        }
        return 0;
    }
    if (strcmp(method_name, "onRgba") == 0) {
        struct std_value *key_val = stdmap_get_str(args, (char *) "key");
        int64_t key = 0;
        if (key_val)
            key = AS_HACK(key_val)->type == 3 ? AS_HACK(key_val)->int32_value : AS_HACK(key_val)->int64_value;

        struct std_value *width_val = stdmap_get_str(args, (char *) "width");
        int width = 0;
        if (width_val)
            width = AS_HACK(width_val)->int32_value;

        struct std_value *height_val = stdmap_get_str(args, (char *) "height");
        int height = 0;
        if (height_val)
            height = AS_HACK(height_val)->int32_value;

        struct std_value *data_val = stdmap_get_str(args, (char *) "data");
        const uint8_t *data = NULL;
        size_t data_len = 0;
        if (data_val) {
            data = AS_HACK(data_val)->array_data.data;
            data_len = AS_HACK(data_val)->array_data.size;
        }

        const bool result = on_rgba(key, data, data_len, width, height);

        if (result && global_engine && g_procs) {
            g_procs->MarkExternalTextureFrameAvailable(global_engine, key);
        }

        const uint8_t payload[2] = { 0x00, result ? 0x01 : 0x02 };
        if (global_engine && g_procs) {
            g_procs->SendPlatformMessageResponse(global_engine, responsehandle, payload, 2);
        }
        return 0;
    }

    // 找不到 Method 时，发送 NULL, size 0（等于 MethodChannel 的 Not Implemented 信号）
    if (global_engine && g_procs) {
        g_procs->SendPlatformMessageResponse(global_engine, responsehandle, NULL, 0);
    }
    return 0;
}

int texture_rgba_renderer_plugin_init(const FlutterEngine engine, FlutterEngineProcTable *procs) {
    global_engine = engine;
    g_procs = procs;

    plugin_registry_set_receiver_locked("texture_rgba_renderer", kStandardMethodCall, on_method_call);

    return 0;
}