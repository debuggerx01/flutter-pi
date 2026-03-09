#ifndef TEXTURE_RGBA_RENDERER_PLUGIN_H
#define TEXTURE_RGBA_RENDERER_PLUGIN_H

#include <flutter_embedder.h>
#include <stdbool.h>

// 初始化插件
int texture_rgba_renderer_plugin_init(FlutterEngine engine, FlutterEngineProcTable *procs);

// 供引擎提取 OpenGL 纹理的回调
bool rgba_renderer_texture_callback(
    const void * user_data,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture* texture_out
);

#endif
