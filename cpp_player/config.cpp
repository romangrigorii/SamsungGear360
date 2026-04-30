#include "config.h"
#include "toml_parser.h"
#include <iostream>
#include <csignal>
#include <cmath>
#include <sstream>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

Options g_options;
FrameData g_frameData;
std::atomic<bool> g_running(true);
std::atomic<AVFormatContext*> g_formatContext(nullptr);
std::atomic<bool> g_streamOpenFailed(false);
StitchParams g_stitchParams;

// Store calibration file path for reload
static std::string g_calibrationFilePath;
// Persistent stream URL when loaded from viewer.toml (g_options.url points into this)
static std::string g_streamUrl;
// Optional ffplay path from viewer.toml [external_player] (g_options.ffplayPath points into this)
static std::string g_ffplayPath;
// Optional GStreamer gst-launch path and params (g_options.gstLaunchPath points into this)
static std::string g_gstLaunchPath;

void setCalibrationFilePath(const std::string& path) {
    g_calibrationFilePath = path;
}

const std::string& getCalibrationFilePath() {
    return g_calibrationFilePath;
}

bool reloadCalibration() {
    if (g_calibrationFilePath.empty()) {
        std::cerr << "Cannot reload calibration: no file path is set. "
                     "Start with --calibration <file> or use calibration.toml in the current working directory."
                  << std::endl;
        return false;
    }
    if (!g_stitchParams.fromCalibrationFile) {
        std::cerr << "No calibration is currently loaded. Trying file: " << g_calibrationFilePath
                  << std::endl;
    } else {
        std::cout << "\n=== Reloading calibration ===" << std::endl;
    }
    return loadCalibrationFromFile(g_calibrationFilePath);
}

// Return first path that exists: exe-relative (cpp folder), then cwd
static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::string resolveConfigPath(const std::string& filename) {
    if (filename.empty()) return std::string();
    if (fileExists(filename))
        return filename;
#ifdef _WIN32
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0) {
        std::string dir(exePath);
        size_t last = dir.find_last_of("\\/");
        if (last != std::string::npos)
            dir.resize(last);
        const char* suffixes[] = { "\\..\\..\\..", "\\..\\..", "\\..", "" };
        for (const char* suf : suffixes) {
            std::string candidate = dir + suf + "\\" + filename;
            if (fileExists(candidate))
                return candidate;
        }
    }
#else
    (void)filename;
#endif
    return std::string();
}

// Load stream config from viewer.toml: [stream] ip, port -> sets g_options.url
bool loadStreamConfig(const std::string& filename) {
    std::string path = resolveConfigPath(filename);
    if (path.empty()) {
        if (!fileExists(filename))
            return false;  // optional config missing, no error message
        path = filename;
    }
    TomlParser toml;
    if (!toml.parse(path)) {
        return false;
    }
    if (!toml.hasSection("stream")) {
        return false;
    }
    std::string ip = toml.getString("stream", "ip", "10.0.0.210");
    int port = toml.getInt("stream", "port", 7679);
    std::string pathPart = toml.getString("stream", "path", "/livestream_high.avi");
    if (pathPart.empty() || pathPart[0] != '/') {
        pathPart = "/" + pathPart;
    }
    std::ostringstream oss;
    oss << "http://" << ip << ":" << port << pathPart;
    g_streamUrl = oss.str();
    g_options.url = g_streamUrl.c_str();
    if (toml.hasSection("external_player")) {
        g_ffplayPath = toml.getString("external_player", "ffplay", "");
        g_options.ffplayPath = g_ffplayPath.empty() ? nullptr : g_ffplayPath.c_str();
        g_gstLaunchPath = toml.getString("external_player", "gst_launch", "");
        g_options.gstLaunchPath = g_gstLaunchPath.empty() ? nullptr : g_gstLaunchPath.c_str();
        g_options.gstQueueMaxBuffers = toml.getInt("external_player", "gst_queue_max_buffers", 1);
        g_options.gstQueueMaxTimeMs = toml.getInt("external_player", "gst_queue_max_time_ms", 0);
        g_options.gstSync = toml.getInt("external_player", "gst_sync", 0) != 0;
    } else {
        g_ffplayPath.clear();
        g_options.ffplayPath = nullptr;
        g_gstLaunchPath.clear();
        g_options.gstLaunchPath = nullptr;
        g_options.gstQueueMaxBuffers = 1;
        g_options.gstQueueMaxTimeMs = 0;
        g_options.gstSync = false;
    }
    std::cout << "Stream URL from " << path << ": " << g_streamUrl << std::endl;
    return true;
}

// Signal handler for graceful shutdown
// Note: Only async-signal-safe operations should be performed here.
// We just set the flag; cleanup happens in the main thread.
void signalHandler(int /* signal */) {
    g_running = false;
}

// Load calibration parameters from TOML file
bool loadCalibrationFromFile(const std::string& filename) {
    TomlParser toml;
    
    if (!toml.parse(filename)) {
        std::cerr << "Failed to parse calibration file: " << filename << std::endl;
        return false;
    }
    
    std::cout << "Loading calibration from: " << filename << std::endl;

    // Single FOV in degrees under [metadata] sets shader scale (deg/180) for both lenses.
    // Legacy files omit metadata and use [lens1]/[lens2] fov as scale factors (1.0 = 180°).
    float sharedFovDeg = -1.0f;
    if (toml.hasSection("metadata")) {
        if (toml.hasKey("metadata", "fov_deg"))
            sharedFovDeg = toml.getFloat("metadata", "fov_deg", 180.0f);
        else if (toml.hasKey("metadata", "lens_fov_deg"))
            sharedFovDeg = toml.getFloat("metadata", "lens_fov_deg", 180.0f);
    }
    if (sharedFovDeg >= 0.0f) {
        float scale = sharedFovDeg / 180.0f;
        g_stitchParams.lens1FOV = scale;
        g_stitchParams.lens2FOV = scale;
        std::cout << "  FOV: " << sharedFovDeg << "° ([metadata]; viewer scale factor "
                  << scale << " for both lenses)" << std::endl;
    }
    
    // Lens 1 parameters
    if (toml.hasSection("lens1")) {
        g_stitchParams.lens1CenterX = toml.getFloat("lens1", "center_x", 0.5f);
        g_stitchParams.lens1CenterY = toml.getFloat("lens1", "center_y", 0.5f);
        g_stitchParams.lens1P1 = toml.getFloat("lens1", "p1", 0.0f);
        g_stitchParams.lens1P2 = toml.getFloat("lens1", "p2", 0.0f);
        g_stitchParams.lens1P3 = toml.getFloat("lens1", "p3", 0.0f);
        g_stitchParams.lens1P4 = toml.getFloat("lens1", "p4", 0.0f);
        if (sharedFovDeg < 0.0f)
            g_stitchParams.lens1FOV = toml.getFloat("lens1", "fov", 1.0f);
        
        std::cout << "  Lens 1: center=(" << g_stitchParams.lens1CenterX << ", " 
                  << g_stitchParams.lens1CenterY << ")";
        if (sharedFovDeg < 0.0f)
            std::cout << ", FOV scale=" << g_stitchParams.lens1FOV;
        std::cout << ", p2=" << g_stitchParams.lens1P2 << std::endl;
    }
    
    // Lens 2 parameters
    if (toml.hasSection("lens2")) {
        g_stitchParams.lens2CenterX = toml.getFloat("lens2", "center_x", 0.5f);
        g_stitchParams.lens2CenterY = toml.getFloat("lens2", "center_y", 0.5f);
        g_stitchParams.lens2P1 = toml.getFloat("lens2", "p1", 0.0f);
        g_stitchParams.lens2P2 = toml.getFloat("lens2", "p2", 0.0f);
        g_stitchParams.lens2P3 = toml.getFloat("lens2", "p3", 0.0f);
        g_stitchParams.lens2P4 = toml.getFloat("lens2", "p4", 0.0f);
        if (sharedFovDeg < 0.0f)
            g_stitchParams.lens2FOV = toml.getFloat("lens2", "fov", 1.0f);
        
        // Per-lens rotation (from v2 format)
        const float DEG_TO_RAD = M_PI / 180.0f;
        float yaw2 = toml.getFloat("lens2", "rotation_yaw", 0.0f);
        float pitch2 = toml.getFloat("lens2", "rotation_pitch", 0.0f);
        float roll2 = toml.getFloat("lens2", "rotation_roll", 0.0f);
        g_stitchParams.lens2RotationYaw = yaw2 * DEG_TO_RAD;
        g_stitchParams.lens2RotationPitch = pitch2 * DEG_TO_RAD;
        g_stitchParams.lens2RotationRoll = roll2 * DEG_TO_RAD;
        
        // Per-lens offsets (from v2 format)
        g_stitchParams.alignmentOffset2X = toml.getFloat("lens2", "offset_x", 0.0f);
        g_stitchParams.alignmentOffset2Y = toml.getFloat("lens2", "offset_y", 0.0f);
        
        std::cout << "  Lens 2: center=(" << g_stitchParams.lens2CenterX << ", " 
                  << g_stitchParams.lens2CenterY << ")";
        if (sharedFovDeg < 0.0f)
            std::cout << ", FOV scale=" << g_stitchParams.lens2FOV;
        std::cout << ", p2=" << g_stitchParams.lens2P2 << std::endl;
        std::cout << "          rotation=(" << yaw2 << "°, " << pitch2 << "°, " << roll2 << "°)"
                  << ", offset=(" << g_stitchParams.alignmentOffset2X << ", " 
                  << g_stitchParams.alignmentOffset2Y << ")" << std::endl;
    }
    
    // Load lens1 per-lens rotation and offsets (v2 format)
    if (toml.hasSection("lens1")) {
        const float DEG_TO_RAD = M_PI / 180.0f;
        float yaw1 = toml.getFloat("lens1", "rotation_yaw", 0.0f);
        float pitch1 = toml.getFloat("lens1", "rotation_pitch", 0.0f);
        float roll1 = toml.getFloat("lens1", "rotation_roll", 0.0f);
        g_stitchParams.lens1RotationYaw = yaw1 * DEG_TO_RAD;
        g_stitchParams.lens1RotationPitch = pitch1 * DEG_TO_RAD;
        g_stitchParams.lens1RotationRoll = roll1 * DEG_TO_RAD;
        
        g_stitchParams.alignmentOffset1X = toml.getFloat("lens1", "offset_x", 0.0f);
        g_stitchParams.alignmentOffset1Y = toml.getFloat("lens1", "offset_y", 0.0f);
        
        if (yaw1 != 0.0f || pitch1 != 0.0f || roll1 != 0.0f) {
            std::cout << "  Lens 1: rotation=(" << yaw1 << "°, " << pitch1 << "°, " << roll1 << "°)" << std::endl;
        }
    }
    
    // Legacy rotation section (for backward compatibility with v1 format)
    if (toml.hasSection("rotation")) {
        float yawDeg = toml.getFloat("rotation", "yaw", 0.0f);
        float pitchDeg = toml.getFloat("rotation", "pitch", 0.0f);
        float rollDeg = toml.getFloat("rotation", "roll", 0.0f);
        
        const float DEG_TO_RAD = M_PI / 180.0f;
        g_stitchParams.rotationYaw = yawDeg * DEG_TO_RAD;
        g_stitchParams.rotationPitch = pitchDeg * DEG_TO_RAD;
        g_stitchParams.rotationRoll = rollDeg * DEG_TO_RAD;
        
        // If per-lens rotations not set, use legacy values for lens2
        if (g_stitchParams.lens2RotationYaw == 0.0f && 
            g_stitchParams.lens2RotationPitch == 0.0f && 
            g_stitchParams.lens2RotationRoll == 0.0f) {
            g_stitchParams.lens2RotationYaw = g_stitchParams.rotationYaw;
            g_stitchParams.lens2RotationPitch = g_stitchParams.rotationPitch;
            g_stitchParams.lens2RotationRoll = g_stitchParams.rotationRoll;
        }
        
        std::cout << "  Rotation (legacy): yaw=" << yawDeg << "°, pitch=" << pitchDeg 
                  << "°, roll=" << rollDeg << "°" << std::endl;
    }
    
    // Legacy alignment section (for backward compatibility with v1 format)
    if (toml.hasSection("alignment")) {
        float y_off = toml.getFloat("alignment", "y_offset", 0.0f);
        float x_off = toml.getFloat("alignment", "x_offset", 0.0f);
        
        // Only use if per-lens offsets not already set
        if (g_stitchParams.alignmentOffset2X == 0.0f && g_stitchParams.alignmentOffset2Y == 0.0f) {
            g_stitchParams.alignmentOffset2Y = y_off;
            g_stitchParams.alignmentOffset2X = x_off;
        }
        
        std::cout << "  Alignment (legacy): y_offset=" << y_off 
                  << ", x_offset=" << x_off << std::endl;
    }
    
    // Mark as calibrated and from file
    g_stitchParams.calibrated = true;
    g_stitchParams.fromCalibrationFile = true;
    
    std::cout << "✓ Calibration loaded successfully" << std::endl;
    
    return true;
}
