static const char* cubemap_vertex =
R"(#version 100

attribute highp vec3 position;
varying highp vec3 direction;

uniform mat4 cubeMapMatrix;

void main()
{
    gl_Position = cubeMapMatrix * vec4(position, 1.0);
    direction = position;
})";

// Cube renders into an EXT_LINEAR-tagged intermediate, so we have to write linear values here.
// The cubemap's six faces are loaded by image_io::load_from_file (sRGB-encoded RGBA8), so apply
// sRGB -> linear after sampling.
static const char* cubemap_fragment =
R"(#version 100
varying highp vec3 direction;
uniform samplerCube smp;

highp vec3 srgb_to_linear(highp vec3 c) {
    highp vec3 lo = c / 12.92;
    highp vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    return mix(hi, lo, vec3(cutoff));
}

void main()
{
    highp vec3 srgb = textureCube(smp, direction).xyz;
    gl_FragColor = vec4(srgb_to_linear(srgb), 1);
})";
