// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

//? #version 430 core
layout(location = 0) out vec2 tex_coord;
layout(location = 1) out vec2 source_size;
layout(location = 2) out vec2 output_size;

layout(binding = 0) uniform sampler2D tex;

#ifdef VULKAN
layout(push_constant, std140) uniform XbrzInfo {
    lowp float scale;
};
#else
uniform lowp float scale;
#endif

const vec2 vertices[4] =
    vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
    gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);
    tex_coord = (vertices[gl_VertexID] + 1.0) / 2.0;
    source_size = vec2(textureSize(tex, 0));
    output_size = source_size * scale;
}
