// Projection functions: rectilinear and equirectangular projections from dual-fisheye

// Convert dual-lens fisheye to rectilinear
// Based on equidistant fisheye projection model
// Maps left lens to left half of screen, right lens to right half of screen
// TODO: Right lens should face -Z (backward) for proper 360° dual-fisheye
vec2 equirectToRectilinear(vec2 uv, float fov) {
    vec2 texSize = uTextureSize;
    
    // Since this function is only called when uRectilinearMode is true,
    // assume dual-lens input. Use uIsHorizontal if available, otherwise use aspect ratio heuristic.
    bool layoutHorizontal;
    if (uStitchCalibrated) {
        // Use calibrated layout from stitching
        layoutHorizontal = uIsHorizontal;
    } else {
        // Use aspect ratio heuristic: horizontal if width > height
        layoutHorizontal = (texSize.x > texSize.y);
    }
    
    // Calculate lens dimensions and S (square side) based on layout
    float lensW, lensH, S;
    if (layoutHorizontal) {
        lensW = texSize.x * 0.5;
        lensH = texSize.y;
    } else {
        lensW = texSize.x;
        lensH = texSize.y * 0.5;
    }
    S = min(lensW, lensH);  // Use smaller dimension for square lens assumption
    
    // Determine which half of the OUTPUT screen we're rendering to
    // ALWAYS use left lens for left half, right lens for right half
    bool renderLeftHalf = uv.x < 0.5;
    
    // Output dimensions (normalized)
    float outW = 1.0;
    float outH = 1.0;
    
    // Rectilinear camera intrinsics for output
    float outFovRad = radians(fov);
    float fx = (outW * 0.5) / tan(outFovRad * 0.5);
    float fy = fx;  // Square pixels assumption
    float cx = outW * 0.5;
    float cy = outH * 0.5;
    
    // Fisheye equidistant model parameters
    float lensFovDeg = 195.0;  // Default lens FOV
    float thetaMax = radians(lensFovDeg * 0.5);
    
    // Fit f so that thetaMax maps to radius ~ S/2 (edge of lens circle)
    float fisheyeRadius = (S * 0.5) * 0.98;  // 0.98 leaves a margin
    float f_fish = fisheyeRadius / thetaMax;
    
    // Use calibrated lens centers if available, otherwise assume center
    // For rectilinear, we need to determine which lens we're using
    float u0, v0;
    if (renderLeftHalf) {
        u0 = uStitchCalibrated ? uLens1Center.x : 0.5;
        v0 = uStitchCalibrated ? uLens1Center.y : 0.5;
    } else {
        u0 = uStitchCalibrated ? uLens2Center.x : 0.5;
        v0 = uStitchCalibrated ? uLens2Center.y : 0.5;
    }
    
    // Define lens forward directions
    // TODO: For proper dual-fisheye, left lens should face +Z (forward) and right lens should face -Z (backward)
    // Currently both look forward, which may cause incorrect rectilinear views for the right half
    vec3 forward = vec3(0.0, 0.0, 1.0);
    
    // Current output pixel in normalized coordinates [0,1] for the half we're rendering
    float x_norm = renderLeftHalf ? uv.x * 2.0 : (uv.x - 0.5) * 2.0;
    float y_norm = uv.y;
    
    // Convert to rectilinear camera coordinates
    float x = (x_norm - cx) / fx;
    float y = (y_norm - cy) / fy;
    
    // Ray direction in virtual camera coords
    vec3 dirCam = vec3(x, y, 1.0);
    dirCam = normalize(dirCam);
    
    // Both lenses look forward, so use the same forward direction
    vec3 fwd = forward;
    
    // theta = angle between dir and optical axis
    float cosTheta = dot(dirCam, fwd);
    cosTheta = clamp(cosTheta, -1.0, 1.0);
    float theta = acos(cosTheta);
    
    // Outside lens FOV -> return invalid UV (will be black)
    if (theta > thetaMax) {
        return vec2(-1.0, -1.0);  // Invalid UV
    }
    
    // Build basis (right, up) for lens image plane
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 rightAxis = cross(up, fwd);
    float rnorm = length(rightAxis);
    if (rnorm < 1e-6) {
        return vec2(-1.0, -1.0);  // Invalid
    }
    rightAxis /= rnorm;
    vec3 upAxis = cross(fwd, rightAxis);  // Ensure orthonormal
    
    // Project dir onto lens plane basis to get azimuth
    float dx = dot(dirCam, rightAxis);
    float dy = dot(dirCam, upAxis);
    
    // Azimuth angle
    float phi = atan(dy, dx);
    
    // Equidistant projection: r = f * theta
    float r = f_fish * theta;
    
    // Map to pixel coords (normalized within lens)
    // Convert r from pixels to normalized coordinates
    float r_norm = r / S;
    
    // Map to lens UV coordinates
    // Both lenses use the same forward direction, but may need coordinate adjustment
    float u, v;
    if (renderLeftHalf) {
        // Left lens: standard mapping, but flip vertically (upside down)
        u = u0 + r_norm * cos(phi);
        v = v0 + r_norm * sin(phi);
        v = 1.0 - v;  // Flip vertically to correct orientation
    } else {
        // Right lens: adjust azimuth and flip to match lens orientation
        // Rotate azimuth by 180 degrees for opposite hemisphere
        float phi_right = phi + PI;
        u = u0 + r_norm * cos(phi_right);
        v = v0 + r_norm * sin(phi_right);
        // Flip horizontally to correct orientation
        u = 1.0 - u;
    }
    
    // Clamp to valid range for the lens
    u = clamp(u, 0.0, 1.0);
    v = clamp(v, 0.0, 1.0);
    
    // Map to source texture coordinates based on which lens
    vec2 sourceUV;
    if (renderLeftHalf) {
        // Left lens: map to left half of source texture [0, 0.5]
        sourceUV = vec2(u * 0.5, v);
    } else {
        // Right lens: map to right half of source texture [0.5, 1.0]
        sourceUV = vec2(0.5 + u * 0.5, v);
    }
    
    return sourceUV;
}

// Apply 3D rotation matrix to a direction vector (matches Python implementation)
// Order: Roll(Y) -> Pitch(X) -> Yaw(Z)
vec3 applyRotation(vec3 dir, vec3 rotation) {
    float yaw = rotation.x;
    float pitch = rotation.y;
    float roll = rotation.z;
    
    float x_sphere = dir.x;
    float y_sphere = dir.y;
    float z_sphere = dir.z;
    
    // Roll (rotation around Y axis - the lens forward direction)
    float cosR = cos(roll);
    float sinR = sin(roll);
    float x_rot = cosR * x_sphere + sinR * z_sphere;
    float z_rot = -sinR * x_sphere + cosR * z_sphere;
    x_sphere = x_rot;
    z_sphere = z_rot;
    
    // Pitch (rotation around X axis)
    float cosP = cos(pitch);
    float sinP = sin(pitch);
    float y_rot = cosP * y_sphere - sinP * z_sphere;
    z_rot = sinP * y_sphere + cosP * z_sphere;
    y_sphere = y_rot;
    z_sphere = z_rot;
    
    // Yaw (rotation around Z axis)
    float cosY = cos(yaw);
    float sinY = sin(yaw);
    x_rot = cosY * x_sphere - sinY * y_sphere;
    y_rot = sinY * x_sphere + cosY * y_sphere;
    x_sphere = x_rot;
    y_sphere = y_rot;
    
    return vec3(x_sphere, y_sphere, z_sphere);
}

// Convert dual-lens fisheye to equirectangular projection
// Matches Python fisheye_to_equirect_half / fisheye_to_equirect_dual exactly
vec2 fisheyeToEquirectangular(vec2 uv, float fov) {
    vec2 texSize = uTextureSize;
    
    bool layoutHorizontal;
    if (uStitchCalibrated) {
        layoutHorizontal = uIsHorizontal;
    } else {
        layoutHorizontal = (texSize.x > texSize.y);
    }
    
    // Calculate lens dimensions
    float lensW, lensH;
    if (layoutHorizontal) {
        lensW = texSize.x * 0.5;
        lensH = texSize.y;
    } else {
        lensW = texSize.x;
        lensH = texSize.y * 0.5;
    }
    float Dia = min(lensW, lensH);
    float Rad = Dia * 0.5;
    
    bool useLeft = uv.x < 0.5;
    
    float outX, outY;
    if (useLeft) {
        outX = uv.x * 2.0;
        outY = uv.y;
    } else {
        outX = 1.0 - (uv.x - 0.5) * 2.0;
        outY = uv.y;
    }
    
    float cx, cy, lensFOV;
    vec4 distortion;
    float x_offset_px, y_offset_px;
    vec3 lensRotation;
    
    if (useLeft) {
        cx = uStitchCalibrated ? uLens1Center.x : 0.5;
        cy = uStitchCalibrated ? uLens1Center.y : 0.5;
        lensFOV = uFromCalibrationFile ? uLens1FOV : 1.0;
        distortion = uFromCalibrationFile ? uLens1Distortion : vec4(0.0);
        x_offset_px = uFromCalibrationFile ? (uAlignmentOffset1.x * lensW) : 0.0;
        y_offset_px = uFromCalibrationFile ? (uAlignmentOffset1.y * lensH) : 0.0;
        lensRotation = uFromCalibrationFile ? uLens1Rotation : vec3(0.0);
    } else {
        cx = uStitchCalibrated ? (1.0 - uLens2Center.x) : 0.5;
        cy = uStitchCalibrated ? uLens2Center.y : 0.5;
        lensFOV = uFromCalibrationFile ? uLens2FOV : 1.0;
        distortion = uFromCalibrationFile ? uLens2Distortion : vec4(0.0);
        x_offset_px = uFromCalibrationFile ? (-uAlignmentOffset2.x * lensW) : 0.0;
        y_offset_px = uFromCalibrationFile ? (uAlignmentOffset2.y * lensH) : 0.0;
        lensRotation = uFromCalibrationFile ? uLens2Rotation : vec3(0.0);
    }
    
    // Principal point (optical center) in pixels — applied after radial arms, alignment last.
    float cx_px = cx * Dia;
    float cy_px = cy * Dia;
    
    float x_fish, y_fish;
    
    if (uFromCalibrationFile) {
        // Same pipeline order as fisheye_to_equirect_calibrated.py and as the forward rim
        // calibration in rectilinear_to_dual_fisheye.py (theta_max / fx_fish from FOV, then k):
        //   (1) incidence angle θ from lens optical axis
        //   (2) FOV: normalize θ by θ_max = lens_fov_deg/2 (dimensionless r ∈ [0,~1] at rim)
        //   (3) radial distortion polynomial on r (matches Python k1,k2,k3 on r^2,r^4,r^6)
        //   (4) convert to pixel offset from axis: ρ = r_distorted * radius
        //   (5) add principal point (cx_px, cy_px), then seam alignment offsets (last)
        float lensDeg = lensFOV * 180.0;
        float lon_range = radians(lensDeg);
        float lon = outX * lon_range - lon_range * 0.5;
        float lat = (0.5 - outY) * PI;
        
        float Xw = cos(lat) * cos(lon);
        float Yw = cos(lat) * sin(lon);
        float Zw = sin(lat);
        
        vec3 world = vec3(Xw, Yw, Zw);
        if (lensRotation.x != 0.0 || lensRotation.y != 0.0 || lensRotation.z != 0.0) {
            world = applyRotation(world, lensRotation);
        }
        
        float Px = -world.z;
        float Py = world.y;
        float Pz = world.x;
        
        float theta_incidence = acos(clamp(Pz, -1.0, 1.0));
        float theta_azimuth = atan(Py, Px) - PI * 0.5;
        
        float theta_max = radians(lensDeg * 0.5);
        float r_norm = theta_incidence / theta_max;
        
        float r2 = r_norm * r_norm;
        float r_distorted = r_norm * (1.0 + distortion.x * r2 + distortion.y * r2 * r2 + distortion.z * r2 * r2 * r2);
        
        float rho = r_distorted * Rad;
        float dx = rho * cos(theta_azimuth);
        float dy = -rho * sin(theta_azimuth);
        
        x_fish = cx_px + dx + x_offset_px;
        y_fish = cy_px + dy + y_offset_px;
    } else {
        // Legacy path (no calibration file): same conceptual order — incidence → FOV scale →
        // distortion → radial pixels → principal point → alignment offsets.
        float Y = 2.0 * (outY - 0.5);
        float X = 2.0 * (outX - 0.5);
        float offsetLon = PI * 0.5;
        float lon_legacy = X * PI * 0.5 + offsetLon;
        float lat_legacy = Y * PI * 0.5;
        
        float x_sphere = cos(lat_legacy) * cos(lon_legacy);
        float y_sphere = cos(lat_legacy) * sin(lon_legacy);
        float z_sphere = sin(lat_legacy);
        
        float theta_inc = atan(sqrt(x_sphere * x_sphere + z_sphere * z_sphere), y_sphere);
        float phi_ray = atan(z_sphere, x_sphere);
        
        float theta_norm = theta_inc / (PI * 0.5);
        float p1 = distortion.x;
        float p2 = distortion.y;
        float p3 = distortion.z;
        float p4 = distortion.w;
        float distortionFactor = 1.0 + p1*theta_norm + p2*theta_norm*theta_norm +
                                 p3*theta_norm*theta_norm*theta_norm +
                                 p4*theta_norm*theta_norm*theta_norm*theta_norm;
        float r_geom = theta_inc * 2.0 / PI * distortionFactor;
        
        float u = r_geom * cos(phi_ray) / lensFOV;
        float v = r_geom * sin(phi_ray) / lensFOV;
        
        x_fish = cx_px + u * Rad + x_offset_px;
        y_fish = cy_px + v * Rad + y_offset_px;
    }
    
    x_fish = clamp(x_fish, 0.0, Dia - 1.0);
    y_fish = clamp(y_fish, 0.0, Dia - 1.0);
    
    float u_lens = x_fish / Dia;
    float v_lens = y_fish / Dia;
    
    if (!useLeft) {
        u_lens = 1.0 - u_lens;
    }
    
    vec2 sourceUV;
    if (useLeft) {
        sourceUV = vec2(u_lens * 0.5, v_lens);
    } else {
        sourceUV = vec2(0.5 + u_lens * 0.5, v_lens);
    }
    
    return sourceUV;
}

// Validate stitching parameters
bool isValidStitching() {
    // Check if stitching is enabled and parameters are valid
    if (!uStitchMode || !uStitchCalibrated) {
        return false;
    }
    
    // Check lens radius is valid (now in pixels)
    // Calculate minimum lens dimension in pixels
    float lensMinDimPx = uIsHorizontal 
        ? min(uTextureSize.x * 0.5, uTextureSize.y)
        : min(uTextureSize.x, uTextureSize.y * 0.5);
    
    if (uLensRadius <= 1.0 || uLensRadius > 0.55 * lensMinDimPx) {
        return false;
    }
    
    // Check lens centers are within valid range [0, 1] (lens-local normalized)
    if (uLens1Center.x < 0.0 || uLens1Center.x > 1.0 ||
        uLens1Center.y < 0.0 || uLens1Center.y > 1.0 ||
        uLens2Center.x < 0.0 || uLens2Center.x > 1.0 ||
        uLens2Center.y < 0.0 || uLens2Center.y > 1.0) {
        return false;
    }
    
    return true;
}
