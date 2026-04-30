// Mask functions: ring mask, circular crop, and light falloff compensation

// Helper function to convert UV to lens-local pixel coordinates
// Returns (pixel, centerPixel) in lens-local pixel space
void convertUVToLensLocal(vec2 uv, vec2 lensCenter, bool lens1, out vec2 pixel, out vec2 centerPixel) {
    vec2 texSize = uTextureSize;
    
    if (uIsHorizontal) {
        // Horizontal layout: each lens is half width
        float lensW = texSize.x * 0.5;
        float lensH = texSize.y;
        
        if (lens1) {
            // Left lens: convert to lens-local coordinates
            vec2 lensUV = vec2(uv.x * 2.0, uv.y);
            pixel = vec2(lensUV.x * lensW, lensUV.y * lensH);
        } else {
            // Right lens: convert to lens-local coordinates
            vec2 lensUV = vec2((uv.x - 0.5) * 2.0, uv.y);
            pixel = vec2(lensUV.x * lensW, lensUV.y * lensH);
        }
        centerPixel = vec2(lensCenter.x * lensW, lensCenter.y * lensH);
    } else {
        // Vertical layout: each lens is half height
        float lensW = texSize.x;
        float lensH = texSize.y * 0.5;
        
        if (lens1) {
            // Top lens: convert to lens-local coordinates
            vec2 lensUV = vec2(uv.x, uv.y * 2.0);
            pixel = vec2(lensUV.x * lensW, lensUV.y * lensH);
        } else {
            // Bottom lens: convert to lens-local coordinates
            vec2 lensUV = vec2(uv.x, (uv.y - 0.5) * 2.0);
            pixel = vec2(lensUV.x * lensW, lensUV.y * lensH);
        }
        centerPixel = vec2(lensCenter.x * lensW, lensCenter.y * lensH);
    }
}

// Ring mask: returns 1.0 in the ring between inner and outer radii, 0.0 otherwise
// This defines the blending region (white ring on black canvas)
// uv is the source texture coordinate [0,1], lensCenter is lens-local normalized [0,1]
// innerRadius and outerRadius are in pixels (per-lens)
// lens1: true for lens 1 (left/top), false for lens 2 (right/bottom)
float ringMask(vec2 uv, vec2 lensCenter, float innerRadius, float outerRadius, bool lens1) {
    vec2 pixel, centerPixel;
    convertUVToLensLocal(uv, lensCenter, lens1, pixel, centerPixel);
    
    // Distance in lens-local pixel space
    float dist = distance(pixel, centerPixel);
    
    // Ring mask: white ring on black canvas
    // Inside inner circle: 0.0 (black)
    // Between inner and outer: 1.0 (white ring)
    // Outside outer circle: 0.0 (black)
    
    float ringWidth = outerRadius - innerRadius;
    float feather = max(2.0, ringWidth * 0.1);  // 10% of ring width for feathering
    
    float mask = 0.0;  // Start with black
    
    // Set ring region to white (1.0) between inner and outer radii
    if (dist >= innerRadius && dist <= outerRadius) {
        mask = 1.0;
    }
    
    // Add smooth feathering at inner edge
    // Transition from 0.0 to 1.0 as we move from (innerRadius - feather) to innerRadius
    if (dist >= innerRadius - feather && dist < innerRadius) {
        float t = (dist - (innerRadius - feather)) / feather;  // 0 at inner-feather, 1 at inner
        mask = clamp(t, 0.0, 1.0);
    }
    
    // Add smooth feathering at outer edge
    // Transition from 1.0 to 0.0 as we move from outerRadius to (outerRadius + feather)
    if (dist > outerRadius && dist <= outerRadius + feather) {
        float t = (outerRadius + feather - dist) / feather;  // 1 at outer, 0 at outer+feather
        mask = clamp(t, 0.0, 1.0);
    }
    
    // Ensure inside inner circle (before feather) is always 0.0 (black)
    if (dist < innerRadius - feather) {
        mask = 0.0;
    }
    
    // Ensure outside outer circle (after feather) is always 0.0 (black)
    if (dist > outerRadius + feather) {
        mask = 0.0;
    }
    
    return mask;
}

// Circular crop: returns 1.0 if pixel is within fisheye circle, 0.0 otherwise
// uv is the source texture coordinate [0,1], lensCenter is lens-local normalized [0,1]
// lensRadius is in pixels (per-lens)
// lens1: true for lens 1 (left/top), false for lens 2 (right/bottom)
float circularCropMask(vec2 uv, vec2 lensCenter, float lensRadius, bool lens1) {
    vec2 pixel, centerPixel;
    convertUVToLensLocal(uv, lensCenter, lens1, pixel, centerPixel);
    
    // Distance in lens-local pixel space
    float dist = distance(pixel, centerPixel);
    
    // Smooth falloff at boundary (feather edge)
    // Use radius-based feathering to match seam feathering
    float feather = max(2.0, lensRadius * 0.15);  // 15% of radius, minimum 2px
    return smoothstep(lensRadius + feather, lensRadius - feather, dist);
}

// Light falloff compensation (vignetting correction)
// Applies inverse vignetting profile to compensate for light falloff
// lens1: true for lens 1 (left/top), false for lens 2 (right/bottom)
vec4 applyLightFalloffCompensation(vec4 color, vec2 uv, vec2 lensCenter, float lensRadius, bool lens1) {
    if (!uEnableLightFalloff) {
        return color;
    }
    
    vec2 pixel, centerPixel;
    convertUVToLensLocal(uv, lensCenter, lens1, pixel, centerPixel);
    
    float dist = distance(pixel, centerPixel);
    float r = clamp(dist / lensRadius, 0.0, 1.0);  // Normalized radius [0, 1]
    
    // Polynomial vignetting model (6th order, similar to FisheyeStitcher)
    // Inverse profile: 1 / (P1*r^5 + P2*r^4 + P3*r^3 + P4*r^2 + P5*r + P6)
    float r2 = r * r;
    float r3 = r2 * r;
    float r4 = r3 * r;
    float r5 = r4 * r;
    
    // Coefficients for inverse vignetting (empirically tuned for fisheye lenses)
    // TODO: Allow per-lens coefficients via uniforms for better calibration
    float P1 = 0.0;
    float P2 = 0.0;
    float P3 = 0.15;
    float P4 = 0.35;
    float P5 = 0.3;
    float P6 = 1.0;
    
    float vignettingProfile = P1 * r5 + P2 * r4 + P3 * r3 + P4 * r2 + P5 * r + P6;
    float compensation = 1.0 / max(vignettingProfile, 0.1);  // Avoid division by zero
    
    // Clamp compensation more tightly to avoid over-brightening seams
    compensation = clamp(compensation, 0.8, 1.5);
    
    return color * compensation;
}
