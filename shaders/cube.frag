#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUv;
layout(location = 2) flat in uint fragTextureIndex;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D bindlessTextures[];

void main() {
    // fragTextureIndex is flat-interpolated from cube.vert, so it is the
    // same value for every invocation across a triangle but can still
    // differ between instances/draws; nonuniformEXT covers that case even
    // though today's single instance makes it dynamically uniform too.
    vec3 texel = texture(bindlessTextures[nonuniformEXT(fragTextureIndex)], fragUv).rgb;
    outColor = vec4(fragColor * texel, 1.0);
}
