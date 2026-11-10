in vec2 vNdc;
out vec4 outColor;

// Light meter for the eye adaptation: an 8 x 8 picture of the frame's brightness before exposure.
// Each cell is the log average of a 4 x 4 grid of scene texels, so one lamp does not swamp a cell.

uniform sampler2DMS uScene;
uniform vec2 uSize;     // scene size in pixels
uniform float uFloor;   // darkest luminance that still counts: pure black must not drag the average to nothing

void main() {
    vec2 cell = floor(gl_FragCoord.xy);
    float sum = 0.0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            vec2 uv = (cell + (vec2(i, j) + 0.5) * 0.25) / 8.0;
            sum += log(max(luma(texelFetch(uScene, ivec2(uv * uSize), 0).rgb), uFloor));
        }
    outColor = vec4(sum / 16.0, 0.0, 0.0, 1.0);
}
