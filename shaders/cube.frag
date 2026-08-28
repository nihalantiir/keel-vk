#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUv;
layout(location = 2) flat in uint fragTextureKind;
layout(location = 3) flat in uint fragTextureIndex;
layout(location = 4) flat in vec4 fragAtlasUvRect;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D bindlessTextures[];
layout(set = 2, binding = 0) uniform sampler2DArray residencyArray;
layout(set = 2, binding = 1) uniform sampler2D residencyAtlas;

// Matches renderer::TextureKind (src/renderer/TextureRef.h).
const uint kTextureKindBindless = 0u;
const uint kTextureKindArray = 1u;

void main() {
    vec3 texel;
    if (fragTextureKind == kTextureKindBindless) {
        // fragTextureIndex is flat-interpolated, so it is the same value
        // for every invocation across a triangle but can still differ
        // between instances/draws; nonuniformEXT covers that case even
        // though today's single instance makes it dynamically uniform too.
        texel = texture(bindlessTextures[nonuniformEXT(fragTextureIndex)], fragUv).rgb;
    } else if (fragTextureKind == kTextureKindArray) {
        texel = texture(residencyArray, vec3(fragUv, float(fragTextureIndex))).rgb;
    } else {
        vec2 uv = mix(fragAtlasUvRect.xy, fragAtlasUvRect.zw, fragUv);
        texel = texture(residencyAtlas, uv).rgb;
    }
    outColor = vec4(fragColor * texel, 1.0);
}
