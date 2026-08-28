#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D bindlessTextures[];

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    float time;
    float phaseSpeed;
    uint textureIndex;
} pc;

void main() {
    // textureIndex is a push constant, the same value for every invocation
    // in this draw, so it is already dynamically uniform; nonuniformEXT is
    // only required when an index can vary per-invocation (e.g. read from a
    // per-vertex or per-instance attribute), but is harmless here too.
    vec3 texel = texture(bindlessTextures[nonuniformEXT(pc.textureIndex)], fragUv).rgb;
    outColor = vec4(fragColor * texel, 1.0);
}
