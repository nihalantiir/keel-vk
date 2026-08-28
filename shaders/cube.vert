#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in float inBaseHue;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragUv;
layout(location = 2) flat out uint fragTextureIndex;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    float time;
    float phaseSpeed;
} pc;

// Matches renderer::GpuInstance (Renderer.cpp) field for field. scalarBlockLayout
// (required device contract) makes std430's usual vec3/vec4 padding rules moot
// here, but the layout is written to match exactly either way.
struct Instance {
    mat4 model;
    vec4 boundsCenterRadius;
    uint textureIndex;
    uint visible;
    uint generation;
    uint _pad0;
};

layout(set = 1, binding = 0, std430) readonly buffer InstanceBuffer {
    Instance instances[];
} instanceBuffer;

// Standard HSV to RGB, hue in degrees, saturation and value in [0,1].
vec3 hsv2rgb(float hueDegrees, float saturation, float value) {
    float h = mod(hueDegrees, 360.0) / 60.0;
    float c = value * saturation;
    float x = c * (1.0 - abs(mod(h, 2.0) - 1.0));
    vec3 rgb;
    if (h < 1.0) rgb = vec3(c, x, 0.0);
    else if (h < 2.0) rgb = vec3(x, c, 0.0);
    else if (h < 3.0) rgb = vec3(0.0, c, x);
    else if (h < 4.0) rgb = vec3(0.0, x, c);
    else if (h < 5.0) rgb = vec3(x, 0.0, c);
    else rgb = vec3(c, 0.0, x);
    return rgb + (value - c);
}

void main() {
    // gl_InstanceIndex, not a manually-added attribute: each indirect
    // command's firstInstance is the CPU cull loop's slot index for the
    // instance that survived (see Renderer::recordWorldPass), and
    // instanceCount is always 1 per command, so this always lands on the
    // right slot without needing shaderDrawParameters' gl_BaseInstance.
    Instance inst = instanceBuffer.instances[gl_InstanceIndex];

    gl_Position = pc.viewProj * inst.model * vec4(inPosition, 1.0);
    fragColor = hsv2rgb(inBaseHue + pc.time * pc.phaseSpeed, 0.75, 1.0);
    fragUv = inUv;
    fragTextureIndex = inst.textureIndex;
}
