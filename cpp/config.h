#ifndef CONFIG_H
#define CONFIG_H

#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>

extern "C" {
    #include <libavformat/avformat.h>
}

// Command-line options
struct Options {
    const char* url = "http://192.168.43.1:7679/livestream_high.avi";
    const char* ffplayPath = nullptr;   // optional path to ffplay.exe (from viewer.toml [external_player])
    const char* gstLaunchPath = nullptr; // optional path to gst-launch-1.0 (for --gstreamer)
    int gstQueueMaxBuffers = 1;         // queue max-size-buffers (1 = low latency)
    int gstQueueMaxTimeMs = 0;          // queue max-size-time in ms (0 = no limit)
    bool gstSync = false;                // autovideosink sync
    bool rectilinearMode = false;  // Enable rectilinear conversion
    bool equirectangularMode = false;  // Enable equirectangular projection
    float fov = 195.0f;  // FOV in degrees (used when rectilinear or equirectangular is enabled)
    bool stitchMode = false;
    bool enableLightFalloffCompensation = false;  // Enable vignetting compensation
};

// Global state
struct FrameData {
    std::vector<uint8_t> data;
    int width;
    int height;
    std::mutex mutex;
    bool updated = false;
};

// Stitching parameters (calculated in stitch mode or loaded from calibration file)
struct StitchParams {
    float lens1CenterX = 0.5f;  // Lens-local normalized [0,1] within lens 1 half
    float lens1CenterY = 0.5f;  // Lens-local normalized [0,1] within lens 1 half
    float lens2CenterX = 0.5f;  // Lens-local normalized [0,1] within lens 2 half
    float lens2CenterY = 0.5f;  // Lens-local normalized [0,1] within lens 2 half
    float lensRadius = 0.0f;    // Lens radius in pixels (per-lens, outer/usable radius)
    float innerRadiusRatio = 0.85f;  // Inner radius as ratio of outer radius (safe zone, default 0.85 = 85%)
    bool isHorizontal = true;  // true if side-by-side, false if top-bottom
    bool calibrated = false;
    
    // Lens dimensions in pixels (for resolution-independent processing)
    int lensWidth = 0;
    int lensHeight = 0;
    
    // Alignment refinement offsets (lens-local normalized UV [0,1])
    float alignmentOffset1X = 0.0f;  // Offset for lens 1
    float alignmentOffset1Y = 0.0f;
    float alignmentOffset2X = 0.0f;  // Offset for lens 2
    float alignmentOffset2Y = 0.0f;
    
    // FOV scaling per lens (1.0 = 180°, typical value ~1.08 for 195° lens)
    float lens1FOV = 1.0f;
    float lens2FOV = 1.0f;
    
    // Polynomial distortion coefficients: r = f*θ*(1 + p1*θ + p2*θ² + p3*θ³ + p4*θ⁴)
    float lens1P1 = 0.0f, lens1P2 = 0.0f, lens1P3 = 0.0f, lens1P4 = 0.0f;
    float lens2P1 = 0.0f, lens2P2 = 0.0f, lens2P3 = 0.0f, lens2P4 = 0.0f;
    
    // Per-lens rotation correction (radians)
    float lens1RotationYaw = 0.0f;
    float lens1RotationPitch = 0.0f;
    float lens1RotationRoll = 0.0f;
    float lens2RotationYaw = 0.0f;
    float lens2RotationPitch = 0.0f;
    float lens2RotationRoll = 0.0f;
    
    // Global rotation (used when per-lens values not specified)
    float rotationYaw = 0.0f;    // Rotation around vertical axis
    float rotationPitch = 0.0f;  // Rotation around horizontal axis
    float rotationRoll = 0.0f;   // Rotation around optical axis
    
    // Whether calibration was loaded from file
    bool fromCalibrationFile = false;
    
    // Track if first frame has been analyzed
    bool firstFrameCollected = false;
};

// Resolve config file path: try next to exe (../../.. for cpp), then cwd
std::string resolveConfigPath(const std::string& filename);
// Load stream URL from viewer.toml [stream] ip + port (sets default URL)
bool loadStreamConfig(const std::string& filename);

// Load calibration from TOML file
bool loadCalibrationFromFile(const std::string& filename);

// Reload calibration from the stored path
bool reloadCalibration();

// Set/get calibration file path
void setCalibrationFilePath(const std::string& path);
const std::string& getCalibrationFilePath();

// Global instances
extern Options g_options;
extern FrameData g_frameData;
extern std::atomic<bool> g_running;
extern std::atomic<AVFormatContext*> g_formatContext;
extern std::atomic<bool> g_streamOpenFailed;
extern StitchParams g_stitchParams;

// Signal handler
void signalHandler(int signal);

#endif // CONFIG_H
