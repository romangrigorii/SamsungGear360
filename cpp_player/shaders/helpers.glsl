// Helper functions for UV validation and coordinate conversion

// Sample texture with BGR to RGB conversion (video source is BGR)
vec4 sampleTextureBGR(vec2 uv) {
    vec4 c = texture(ourTexture, uv);
    return vec4(c.b, c.g, c.r, c.a);  // Swap B and R channels
}

// Helper function to validate UV coordinates in full texture space
bool validUV(vec2 u) {
    return all(greaterThanEqual(u, vec2(0.0))) && all(lessThanEqual(u, vec2(1.0)));
}

// Helper function to validate UV coordinates for a specific lens half
bool validLensUV(vec2 u, bool lens1) {
    if (!validUV(u)) return false;
    if (uIsHorizontal) {
        return lens1 ? (u.x <= 0.5) : (u.x >= 0.5);
    } else {
        return lens1 ? (u.y <= 0.5) : (u.y >= 0.5);
    }
}
