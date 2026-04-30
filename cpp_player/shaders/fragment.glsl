// Fragment shader main function
// Note: #version, uniforms, and in/out declarations are in uniforms.glsl
// All helper functions are included from other files

in vec2 TexCoord;
out vec4 FragColor;

void main() {
    vec2 uv = TexCoord;
    
    if (uEquirectangularMode) {
        // Equirectangular mode: dual-lens fisheye to equirectangular conversion
        // Always compute non-stitched projection as fallback
        vec2 equirectUV = fisheyeToEquirectangular(uv, uFOV);
        vec4 fallbackColor = vec4(0.0, 0.0, 0.0, 1.0);
        if (equirectUV.x >= 0.0 && equirectUV.x <= 1.0 && equirectUV.y >= 0.0 && equirectUV.y <= 1.0) {
            fallbackColor = sampleTextureBGR(equirectUV);
        }
        
        // Only use stitching if parameters are valid
        if (isValidStitching()) {
            // Try equirectangular with stitching
            vec4 stitchedColor = fisheyeToEquirectangularWithStitching(uv, uFOV);
            // Use stitched result if alpha > 0.5 (valid mapping), otherwise use fallback
            // Alpha = 0.0 indicates invalid mapping (outside both circles or invalid UVs)
            if (stitchedColor.a > 0.5) {
                FragColor = stitchedColor;
            } else {
                // Fall back to non-stitched projection
                FragColor = fallbackColor;
            }
        } else {
            // Equirectangular without stitching: use fallback
            FragColor = fallbackColor;
        }
    } else if (uRectilinearMode) {
        // Rectilinear mode: dual-lens fisheye to rectilinear conversion (no stitching)
        vec2 rectUV = equirectToRectilinear(uv, uFOV);
        
        // Check for valid UV coordinates
        if (rectUV.x >= 0.0 && rectUV.x <= 1.0 && rectUV.y >= 0.0 && rectUV.y <= 1.0) {
            FragColor = sampleTextureBGR(rectUV);
        } else {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);  // Black outside FOV or invalid
        }
    } else {
        // Default mode: display raw equirectangular video
        FragColor = sampleTextureBGR(uv);
    }
}
