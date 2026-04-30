#version 330 core

// Uniform declarations and constants
uniform sampler2D ourTexture;
uniform bool uRectilinearMode;
uniform bool uEquirectangularMode;
uniform float uFOV;
uniform bool uStitchMode;
uniform vec2 uLens1Center;  // Lens-local normalized [0,1] within lens 1 half
uniform vec2 uLens2Center;  // Lens-local normalized [0,1] within lens 2 half
uniform float uLensRadius;  // Lens radius in pixels (per-lens, outer/usable radius)
uniform float uInnerRadiusRatio;  // Inner radius as ratio of outer radius (safe zone, e.g., 0.85)
uniform bool uIsHorizontal;
uniform vec2 uTextureSize;
uniform bool uStitchCalibrated;
uniform vec2 uAlignmentOffset1;  // Alignment offset for lens 1 (lens-local normalized UV [0,1])
uniform vec2 uAlignmentOffset2;  // Alignment offset for lens 2 (lens-local normalized UV [0,1])
uniform bool uEnableCircularCrop;  // Enable circular cropping of fisheye discs
uniform bool uEnableLightFalloff;  // Enable light falloff (vignetting) compensation

// Per-lens FOV scaling (1.0 = 180°, typical value ~1.08 for 195° lens)
uniform float uLens1FOV;
uniform float uLens2FOV;

// Polynomial distortion coefficients: r = f*θ*(1 + p1*θ + p2*θ² + p3*θ³ + p4*θ⁴)
uniform vec4 uLens1Distortion;  // (p1, p2, p3, p4)
uniform vec4 uLens2Distortion;  // (p1, p2, p3, p4)

// Per-lens rotation correction (radians)
uniform vec3 uLens1Rotation;  // (yaw, pitch, roll) for lens 1
uniform vec3 uLens2Rotation;  // (yaw, pitch, roll) for lens 2

// Legacy single rotation (for backward compatibility)
uniform vec3 uRotation;  // (yaw, pitch, roll)

// Whether calibration was loaded from file
uniform bool uFromCalibrationFile;

const float PI = 3.14159265359;
