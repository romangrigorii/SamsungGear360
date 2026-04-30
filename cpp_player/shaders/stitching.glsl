// Stitching function: dual-fisheye to equirectangular with blending

// Convert dual-lens fisheye to equirectangular projection with stitching
// Uses stitching to blend at lens boundaries
vec4 fisheyeToEquirectangularWithStitching(vec2 uv, float fov) {
    vec2 texSize = uTextureSize;
    
    // Use uIsHorizontal from stitching calibration to determine layout
    // Calculate lens dimensions based on layout
    float lensW, lensH, S;
    if (uIsHorizontal) {
        lensW = texSize.x * 0.5;
        lensH = texSize.y;
    } else {
        lensW = texSize.x;
        lensH = texSize.y * 0.5;
    }
    S = min(lensW, lensH);  // Use smaller dimension for square lens assumption
    
    // Equirectangular mapping:
    // uv.x (0 to 1) maps to longitude (-180° to +180°)
    // uv.y (0 to 1) maps to latitude (-90° to +90°)
    float longitude = (uv.x - 0.5) * 2.0 * PI;  // -PI to +PI
    float latitude = (0.5 - uv.y) * PI;  // -PI/2 to +PI/2 (inverted Y)
    
    // Apply vertical FOV limit
    float maxLatitude = radians(fov * 0.5);
    if (abs(latitude) > maxLatitude) {
        return vec4(0.0, 0.0, 0.0, 0.0);  // Outside FOV - return transparent
    }
    
    // Convert to 3D direction on unit sphere
    float cosLat = cos(latitude);
    vec3 dir = vec3(
        cosLat * sin(longitude),
        sin(latitude),
        cosLat * cos(longitude)
    );
    
    // Fisheye parameters
    float lensFovDeg = 195.0;
    float thetaMax = radians(lensFovDeg * 0.5);
    float fisheyeRadius = (S * 0.5) * 0.98;
    float f_fish = fisheyeRadius / thetaMax;
    
    // Use calibrated lens centers (lens-local normalized [0,1])
    // These are detected from the actual image, not assumed
    
    // Determine which lens(s) to use based on direction and stitching
    vec3 forwardL = vec3(0.0, 0.0, 1.0);
    vec3 forwardR = vec3(0.0, 0.0, -1.0);
    
    // Calculate angles from both lens optical axes
    float cosThetaL = dot(dir, forwardL);
    cosThetaL = clamp(cosThetaL, -1.0, 1.0);
    float thetaL = acos(cosThetaL);
    
    float cosThetaR = dot(dir, forwardR);
    cosThetaR = clamp(cosThetaR, -1.0, 1.0);
    float thetaR = acos(cosThetaR);
    
    // Check if direction is within FOV of either lens
    bool inLeftFOV = thetaL <= thetaMax;
    bool inRightFOV = thetaR <= thetaMax;
    
    if (!inLeftFOV && !inRightFOV) {
        return vec4(0.0, 0.0, 0.0, 0.0);  // Outside both lenses - return transparent
    }
    
    // Build basis for lens image planes
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 rightAxisL = cross(up, forwardL);
    vec3 rightAxisR = cross(up, forwardR);
    float rnormL = length(rightAxisL);
    float rnormR = length(rightAxisR);
    
    if (rnormL < 1e-6 || rnormR < 1e-6) {
        return vec4(0.0, 0.0, 0.0, 0.0);  // Invalid basis - return transparent
    }
    rightAxisL /= rnormL;
    rightAxisR /= rnormR;
    vec3 upAxisL = cross(forwardL, rightAxisL);
    vec3 upAxisR = cross(forwardR, rightAxisR);
    
    // Project direction onto both lens planes
    float dxL = dot(dir, rightAxisL);
    float dyL = dot(dir, upAxisL);
    float phiL = atan(dyL, dxL);
    
    float dxR = dot(dir, rightAxisR);
    float dyR = dot(dir, upAxisR);
    float phiR = atan(dyR, dxR);
    
    // Calculate UVs for both lenses
    vec2 uvL = vec2(-1.0, -1.0);
    vec2 uvR = vec2(-1.0, -1.0);
    
    if (inLeftFOV) {
        float rL = f_fish * thetaL;
        float r_normL = rL / S;
        // Use calibrated lens 1 center (lens-local normalized [0,1])
        float uL = uLens1Center.x + r_normL * cos(phiL);
        float vL = uLens1Center.y + r_normL * sin(phiL);
        vL = 1.0 - vL;  // Flip vertically for left lens
        uL = clamp(uL, 0.0, 1.0);
        vL = clamp(vL, 0.0, 1.0);
        
        // Apply alignment offset in lens-local normalized UV space [0,1]
        // This allows per-lens pixel shifts to be correctly scaled
        uL += uAlignmentOffset1.x;
        vL += uAlignmentOffset1.y;
        
        // Map to full-texture UV coordinates
        if (uIsHorizontal) {
            uvL = vec2(uL * 0.5, vL);
        } else {
            uvL = vec2(uL, vL * 0.5);
        }
        
        // Validate on unclamped UV to catch invalid offsets
        // Don't clamp - let invalid UVs return alpha=0 for proper fallback
    }
    
    if (inRightFOV) {
        float rR = f_fish * thetaR;
        float r_normR = rR / S;
        float phiR_adj = phiR + PI;
        // Use calibrated lens 2 center (lens-local normalized [0,1])
        float uR = uLens2Center.x + r_normR * cos(phiR_adj);
        float vR = uLens2Center.y + r_normR * sin(phiR_adj);
        uR = 1.0 - uR;  // Flip horizontally for right lens
        uR = clamp(uR, 0.0, 1.0);
        vR = clamp(vR, 0.0, 1.0);
        
        // Apply alignment offset in lens-local normalized UV space [0,1]
        // This allows per-lens pixel shifts to be correctly scaled
        uR += uAlignmentOffset2.x;
        vR += uAlignmentOffset2.y;
        
        // Map to full-texture UV coordinates
        if (uIsHorizontal) {
            uvR = vec2(0.5 + uR * 0.5, vR);
        } else {
            uvR = vec2(uR, 0.5 + vR * 0.5);
        }
        
        // Validate on unclamped UV to catch invalid offsets
        // Don't clamp - let invalid UVs return alpha=0 for proper fallback
    }
    
    // Sample and blend using stitching logic
    // Always return a color if within FOV, use lens radius only for blending decisions
    if (inLeftFOV && inRightFOV) {
        // Both lenses see this direction - blend using stitching logic
        // Validate UVs before any processing (check lens-half validity)
        bool validL = validLensUV(uvL, true);   // lens 1 (left/top)
        bool validR = validLensUV(uvR, false);  // lens 2 (right/bottom)
        
        if (!validL && !validR) {
            // Both UVs invalid - return transparent
            return vec4(0.0, 0.0, 0.0, 0.0);
        }
        
        // Sample textures only if UVs are valid
        vec4 colorL = vec4(0.0);
        vec4 colorR = vec4(0.0);
        
        if (validL) {
            colorL = sampleTextureBGR(uvL);
        }
        if (validR) {
            colorR = sampleTextureBGR(uvR);
        }
        
        // Calculate distances from lens centers in lens-local pixel space
        // Only calculate if UV is valid, otherwise use large distance
        vec2 pixelL, pixelR;  // lens-local pixel coordinates
        vec2 centerL, centerR;  // lens-local pixel coordinates
        
        if (uIsHorizontal) {
            float lensW = texSize.x * 0.5;
            float lensH = texSize.y;
            
            if (validL) {
                // Left lens: convert uvL to lens-local
                vec2 lensUVL = vec2(uvL.x * 2.0, uvL.y);
                pixelL = vec2(lensUVL.x * lensW, lensUVL.y * lensH);
            } else {
                pixelL = vec2(1e6, 1e6);  // Invalid - use large distance
            }
            centerL = vec2(uLens1Center.x * lensW, uLens1Center.y * lensH);
            
            if (validR) {
                // Right lens: convert uvR to lens-local
                vec2 lensUVR = vec2((uvR.x - 0.5) * 2.0, uvR.y);
                pixelR = vec2(lensUVR.x * lensW, lensUVR.y * lensH);
            } else {
                pixelR = vec2(1e6, 1e6);  // Invalid - use large distance
            }
            centerR = vec2(uLens2Center.x * lensW, uLens2Center.y * lensH);
        } else {
            float lensW = texSize.x;
            float lensH = texSize.y * 0.5;
            
            if (validL) {
                // Top lens: convert uvL to lens-local
                vec2 lensUVL = vec2(uvL.x, uvL.y * 2.0);
                pixelL = vec2(lensUVL.x * lensW, lensUVL.y * lensH);
            } else {
                pixelL = vec2(1e6, 1e6);  // Invalid - use large distance
            }
            centerL = vec2(uLens1Center.x * lensW, uLens1Center.y * lensH);
            
            if (validR) {
                // Bottom lens: convert uvR to lens-local
                vec2 lensUVR = vec2(uvR.x, (uvR.y - 0.5) * 2.0);
                pixelR = vec2(lensUVR.x * lensW, lensUVR.y * lensH);
            } else {
                pixelR = vec2(1e6, 1e6);  // Invalid - use large distance
            }
            centerR = vec2(uLens2Center.x * lensW, uLens2Center.y * lensH);
        }
        
        // Distance in lens-local pixel space (uLensRadius is in pixels)
        float distL = distance(pixelL, centerL);
        float distR = distance(pixelR, centerR);
        
        // Ring-based blending: separate inner (safe) and outer (usable) radii
        // This prevents blending in extreme periphery where lens quality degrades
        float innerRadius = uLensRadius * uInnerRadiusRatio;  // Safe zone radius
        float outerRadius = uLensRadius;  // Usable fisheye radius
        
        // Compute ring-based weights:
        // - Inside inner radius: weight = 1.0 (safe zone, full weight)
        // - Between inner and outer: smooth transition from 1.0 to 0.0 (blending zone)
        // - Outside outer radius: weight = 0.0 (no weight)
        
        float ringWidth = outerRadius - innerRadius;
        float featherWidth = max(2.0, ringWidth * 0.1);  // 10% of ring width for feathering
        
        // Compute distance to inner and outer boundaries
        float distToInnerL = innerRadius - distL;  // Positive = inside inner circle
        float distToOuterL = outerRadius - distL;   // Positive = inside outer circle
        float distToInnerR = innerRadius - distR;
        float distToOuterR = outerRadius - distR;
        
        // Weight calculation for lens L:
        // - If inside inner radius: weight = 1.0
        // - If in ring (between inner and outer): smooth transition
        // - If outside outer: weight = 0.0
        float weightL = 0.0;
        if (validL) {
            if (distL <= innerRadius) {
                // Inside safe zone: full weight
                weightL = 1.0;
            } else if (distL <= outerRadius) {
                // In blending ring: smooth transition from 1.0 to 0.0
                // Map distance from [innerRadius, outerRadius] to [1.0, 0.0]
                float t = (distL - innerRadius) / ringWidth;  // 0 at inner, 1 at outer
                weightL = 1.0 - smoothstep(0.0, 1.0, t);
            } else {
                // Outside outer radius: no weight
                weightL = 0.0;
            }
        }
        
        // Weight calculation for lens R (same logic)
        float weightR = 0.0;
        if (validR) {
            if (distR <= innerRadius) {
                weightR = 1.0;
            } else if (distR <= outerRadius) {
                float t = (distR - innerRadius) / ringWidth;
                weightR = 1.0 - smoothstep(0.0, 1.0, t);
            } else {
                weightR = 0.0;
            }
        }
        
        float distWeightL = weightL;
        float distWeightR = weightR;
        
        // Apply ring mask to constrain blending to the ring region
        // This prevents blending in extreme periphery where lens quality degrades
        if (validL) {
            float ringMaskL = ringMask(uvL, uLens1Center, innerRadius, outerRadius, true);  // lens1
            distWeightL *= ringMaskL;  // Only blend in the ring region
        }
        if (validR) {
            float ringMaskR = ringMask(uvR, uLens2Center, innerRadius, outerRadius, false);  // lens2
            distWeightR *= ringMaskR;  // Only blend in the ring region
        }
        
        // Apply circular crop if enabled (only if UV is valid)
        if (uEnableCircularCrop) {
            if (validL) {
                float maskL = circularCropMask(uvL, uLens1Center, uLensRadius, true);  // lens1
                colorL *= maskL;
                distWeightL *= maskL;  // Also reduce weight if outside crop circle
            }
            if (validR) {
                float maskR = circularCropMask(uvR, uLens2Center, uLensRadius, false);  // lens2
                colorR *= maskR;
                distWeightR *= maskR;  // Also reduce weight if outside crop circle
            }
        }
        
        // Apply light falloff compensation if enabled (only if UV is valid)
        if (uEnableLightFalloff) {
            if (validL) {
                colorL = applyLightFalloffCompensation(colorL, uvL, uLens1Center, uLensRadius, true);  // lens1
            }
            if (validR) {
                colorR = applyLightFalloffCompensation(colorR, uvR, uLens2Center, uLensRadius, false);  // lens2
            }
        }
        
        // Normalize weights for blending
        float totalWeight = distWeightL + distWeightR;
        if (totalWeight > 0.001) {
            // Both lenses contribute - blend based on normalized weights
            float blend = distWeightR / totalWeight;
            vec4 result = mix(colorL, colorR, blend);
            // Return with alpha = 1.0 to indicate valid mapping
            return vec4(result.rgb, 1.0);
        } else {
            // Both lenses are invalid or outside their circles - return transparent
            return vec4(0.0, 0.0, 0.0, 0.0);
        }
    } else if (inLeftFOV) {
        // Only left lens sees this direction
        if (!validLensUV(uvL, true)) {  // lens1
            return vec4(0.0, 0.0, 0.0, 0.0);  // Invalid UV - return transparent
        }
        
        vec4 colorL = sampleTextureBGR(uvL);
        
        // Apply circular crop if enabled
        if (uEnableCircularCrop) {
            float maskL = circularCropMask(uvL, uLens1Center, uLensRadius, true);  // lens1
            colorL *= maskL;
        }
        
        // Apply light falloff compensation if enabled
        if (uEnableLightFalloff) {
            colorL = applyLightFalloffCompensation(colorL, uvL, uLens1Center, uLensRadius, true);  // lens1
        }
        
        return vec4(colorL.rgb, 1.0);  // Valid mapping
    } else if (inRightFOV) {
        // Only right lens sees this direction
        if (!validLensUV(uvR, false)) {  // lens2
            return vec4(0.0, 0.0, 0.0, 0.0);  // Invalid UV - return transparent
        }
        
        vec4 colorR = sampleTextureBGR(uvR);
        
        // Apply circular crop if enabled
        if (uEnableCircularCrop) {
            float maskR = circularCropMask(uvR, uLens2Center, uLensRadius, false);  // lens2
            colorR *= maskR;
        }
        
        // Apply light falloff compensation if enabled
        if (uEnableLightFalloff) {
            colorR = applyLightFalloffCompensation(colorR, uvR, uLens2Center, uLensRadius, false);  // lens2
        }
        
        return vec4(colorR.rgb, 1.0);  // Valid mapping
    }
    
    // Outside both lens FOVs
    return vec4(0.0, 0.0, 0.0, 0.0);  // Invalid - return transparent
}
