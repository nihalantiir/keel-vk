#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in float inBaseHue;

layout(location = 0) out vec3 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    float time;
    float phaseSpeed;
} pc;

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
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragColor = hsv2rgb(inBaseHue + pc.time * pc.phaseSpeed, 0.75, 1.0);
}
