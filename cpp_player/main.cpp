#include "config.h"
#include "logging.h"
#include "shaders/shaders.h"
#include "renderer.h"
#include "video_decoder.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <mutex>
#include <atomic>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

static void pauseBeforeExitIfError() {
#ifdef _WIN32
    std::cerr << "Press Enter to close..." << std::endl;
    std::cerr.flush();
    std::cout.flush();
    // Message box so user sees something even if console isn't visible
    MessageBoxA(nullptr, "An error occurred. See console for details.", "Gear360 Viewer", MB_OK | MB_ICONERROR);
    std::cin.get();
#endif
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [options] [url]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --ffplay            Use ffplay to display stream (default external player)" << std::endl;
    std::cout << "  --gstreamer         Use GStreamer to display stream (on Windows uses ffplay)" << std::endl;
    std::cout << "  --rectilinear       Enable rectilinear conversion (spherical to flat view)" << std::endl;
    std::cout << "  --equirectangular   Enable equirectangular projection (dual-lens to 360° view)" << std::endl;
    std::cout << "  --fov <degrees>     Field of view for conversions (default: 195 when --rectilinear or --equirectangular used)" << std::endl;
    std::cout << "  --stitch            Enable stitch mode for equirectangular projection" << std::endl;
    std::cout << "  --calibration <file>  Load calibration parameters from TOML file" << std::endl;
    std::cout << "  --light-falloff     Enable lens light falloff compensation (shader)" << std::endl;
    std::cout << "  --help, -h          Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Stream URL: from viewer.toml [stream] (ip, port, path), or pass [url] on command line." << std::endl;
    std::cout << "On Windows, set [external_player] ffplay in viewer.toml if ffplay is not in PATH." << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << programName << "                    # OpenGL viewer (URL from viewer.toml)" << std::endl;
    std::cout << "  " << programName << " --ffplay            # Play with ffplay (default external player)" << std::endl;
    std::cout << "  " << programName << " --gstreamer         # Play with GStreamer" << std::endl;
    std::cout << "  " << programName << " --rectilinear       # OpenGL rectilinear view" << std::endl;
}

static std::string buildGstPipeline(const std::string& url) {
    int maxBuf = g_options.gstQueueMaxBuffers > 0 ? g_options.gstQueueMaxBuffers : 1;
    int maxTimeMs = g_options.gstQueueMaxTimeMs >= 0 ? g_options.gstQueueMaxTimeMs : 0;
    std::ostringstream queueOpts;
    queueOpts << "max-size-buffers=" << maxBuf;
    if (maxTimeMs > 0)
        queueOpts << " max-size-time=" << (static_cast<long long>(maxTimeMs) * 1000000);
    std::ostringstream pipe;
    pipe << "uridecodebin uri=\"" << url << "\" ! queue " << queueOpts.str()
         << " ! videoconvert ! autovideosink sync=" << (g_options.gstSync ? "true" : "false");
    return pipe.str();
}

#ifdef _WIN32
// Run process with argv[] on Windows without going through cmd.exe (avoids quoting issues)
static int runProcessWindows(const char* exe, const char* const* argv) {
    std::string cmdLine;
    for (const char* const* p = argv; *p; ++p) {
        if (cmdLine.size()) cmdLine += ' ';
        const char* arg = *p;
        bool needQuote = false;
        for (const char* s = arg; *s; ++s) if (*s == ' ' || *s == '"') { needQuote = true; break; }
        if (needQuote) {
            cmdLine += '"';
            for (; *arg; ++arg) {
                if (*arg == '"') cmdLine += "\\\"";
                else cmdLine += *arg;
            }
            cmdLine += '"';
        } else
            cmdLine += arg;
    }
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');
    if (!CreateProcessA(nullptr, cmdLineBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        std::cerr << "CreateProcess failed (error " << err << ") for: " << exe << std::endl;
        return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exitCode);
}
#endif

static int runExternalPlayer(bool useGstreamer, const std::string& url) {
    const char* ffplay = (g_options.ffplayPath && g_options.ffplayPath[0]) ? g_options.ffplayPath : "ffplay";

    if (useGstreamer) {
        const char* gstExe = (g_options.gstLaunchPath && g_options.gstLaunchPath[0])
            ? g_options.gstLaunchPath : "gst-launch-1.0";
        std::string pipeline = buildGstPipeline(url);
#ifdef _WIN32
        const char* gstArgv[] = { gstExe, "-e", pipeline.c_str(), nullptr };
        int ret = runProcessWindows(gstExe, gstArgv);
        if (ret != 0) {
            std::cerr << "GStreamer failed (exit " << ret << "). Falling back to ffplay." << std::endl;
            const char* ffargv[] = { ffplay, "-hide_banner", "-fflags", "nobuffer", "-flags", "low_delay", "-framedrop", "-i", url.c_str(), nullptr };
            return runProcessWindows(ffplay, ffargv);
        }
        return ret;
#else
        std::ostringstream cmd;
        cmd << gstExe << " -e '" << pipeline << "'";
        return std::system(cmd.str().c_str());
#endif
    }

#ifdef _WIN32
    const char* ffargv[] = { ffplay, "-hide_banner", "-fflags", "nobuffer", "-flags", "low_delay", "-framedrop", "-i", url.c_str(), nullptr };
    return runProcessWindows(ffplay, ffargv);
#else
    std::ostringstream cmd;
    cmd << ffplay << " -hide_banner -fflags nobuffer -flags low_delay -framedrop -i \"" << url << "\"";
    return std::system(cmd.str().c_str());
#endif
}

static bool s_useFfplay = false;
static bool s_useGstreamer = false;

static bool argvRequestsHelp(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return true;
    }
    return false;
}

int parseArguments(int argc, char* argv[]) {
    loadStreamConfig("viewer.toml");

    bool fovSpecified = false;
    std::string calibrationFile = "";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return -1;
        } else if (strcmp(argv[i], "--ffplay") == 0) {
            s_useFfplay = true;
        } else if (strcmp(argv[i], "--gstreamer") == 0) {
            s_useGstreamer = true;
        } else if (strcmp(argv[i], "--rectilinear") == 0) {
            g_options.rectilinearMode = true;
        } else if (strcmp(argv[i], "--equirectangular") == 0) {
            g_options.equirectangularMode = true;
        } else if (strcmp(argv[i], "--fov") == 0) {
            if (i + 1 < argc) {
                g_options.fov = std::stof(argv[++i]);
                if (g_options.fov <= 0 || g_options.fov > 360) {
                    std::cerr << "Error: FOV must be between 0 and 360 degrees" << std::endl;
                    return 1;
                }
                fovSpecified = true;
            } else {
                std::cerr << "Error: --fov requires a value" << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "--stitch") == 0) {
            g_options.stitchMode = true;
        } else if (strcmp(argv[i], "--calibration") == 0) {
            if (i + 1 < argc) {
                calibrationFile = argv[++i];
            } else {
                std::cerr << "Error: --calibration requires a file path" << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "--light-falloff") == 0) {
            g_options.enableLightFalloffCompensation = true;
        } else if (argv[i][0] != '-') {
            g_options.url = argv[i];
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (!calibrationFile.empty()) {
        setCalibrationFilePath(calibrationFile);
        if (loadCalibrationFromFile(calibrationFile)) {
            if (!g_options.equirectangularMode && !g_options.rectilinearMode) {
                g_options.equirectangularMode = true;
                std::cout << "Equirectangular mode auto-enabled due to calibration file" << std::endl;
            }
        } else {
            std::cerr << "Warning: Failed to load calibration file, using defaults" << std::endl;
        }
    } else {
        std::string calPath = resolveConfigPath("calibration.toml");
        setCalibrationFilePath(calPath.empty() ? "calibration.toml" : calPath);
    }

    if ((g_options.rectilinearMode || g_options.equirectangularMode) && !fovSpecified) {
        g_options.fov = 195.0f;
    }

    return 0;
}

void printStartupInfo() {
    std::cout << "Gear360 Viewer - Connecting to: " << g_options.url << std::endl;
    std::cout << "Press 'R' to reload calibration from: " << getCalibrationFilePath() << std::endl;
    if (g_options.rectilinearMode) {
        std::cout << "Rectilinear mode: ENABLED (FOV: " << g_options.fov << " degrees)" << std::endl;
    } else if (g_options.equirectangularMode) {
        std::cout << "Equirectangular mode: ENABLED (FOV: " << g_options.fov << " degrees)";
        if (g_options.stitchMode) {
            std::cout << " with stitching";
        }
        std::cout << std::endl;
    } else {
        std::cout << "Display mode: Raw equirectangular video" << std::endl;
    }
    if (g_options.stitchMode && g_options.equirectangularMode) {
        std::cout << "Stitch mode: ENABLED" << std::endl;
    }
    if (g_options.enableLightFalloffCompensation) {
        std::cout << "Light falloff compensation: ENABLED" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Tee subsequent diagnostics to <exe_dir>/gear360_viewer.log (skip when printing --help only)
    if (!argvRequestsHelp(argc, argv))
        initLoggingToFile(argc, argv);
    
    // Parse command-line arguments
    int parseResult = parseArguments(argc, argv);
    if (parseResult != 0) {
        return (parseResult == -1) ? 0 : parseResult;  // -1 means --help, exit cleanly
    }
    
    if (s_useFfplay || s_useGstreamer) {
        std::string url(g_options.url ? g_options.url : "http://10.0.0.210:7679/livestream_high.avi");
        int ret = runExternalPlayer(s_useGstreamer, url);
        if (ret != 0) {
            std::cerr << "External player exited with code " << ret << "." << std::endl;
            std::cerr << "Install GStreamer (gst-launch-1.0) and/or FFmpeg (ffplay), or set [external_player] ffplay / gst_launch in viewer.toml." << std::endl;
            pauseBeforeExitIfError();
        }
        return ret;
    }

    printStartupInfo();

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        pauseBeforeExitIfError();
        return 1;
    }
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Gear360 Viewer", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        pauseBeforeExitIfError();
        return 1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::cerr << "Failed to initialize GLEW" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        pauseBeforeExitIfError();
        return 1;
    }
    
    while (glGetError() != GL_NO_ERROR) {}
    
    if (!createShaders()) {
        std::cerr << "Failed to create shaders" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        pauseBeforeExitIfError();
        return 1;
    }
    
    setupQuad();
    
    // Start video decode thread
    std::thread decodeThread(videoDecodeThread, g_options.url);
    
    // Main render loop
    while (!glfwWindowShouldClose(window) && g_running) {
        glfwPollEvents();
        
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        
        // Reload calibration on 'R' key press
        static bool rKeyWasPressed = false;
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            if (!rKeyWasPressed) {
                rKeyWasPressed = true;
                reloadCalibration();
            }
        } else {
            rKeyWasPressed = false;
        }
        
        // Update texture if new frame available
        {
            std::lock_guard<std::mutex> lock(g_frameData.mutex);
            if (g_frameData.updated && g_frameData.width > 0 && g_frameData.height > 0) {
                updateTexture(g_frameData.data, g_frameData.width, g_frameData.height);
                g_frameData.updated = false;
            }
        }
        
        // Render
        renderFrame();
        
        glfwSwapBuffers(window);
    }
    
    // Cleanup - stop running first to signal decode thread
    std::cout << "Shutting down..." << std::endl;
    g_running = false;
    
    // Wait for decode thread to finish and close stream
    if (decodeThread.joinable()) {
        decodeThread.join();
    }
    
    if (g_streamOpenFailed)
        pauseBeforeExitIfError();
    
    // Ensure format context is closed
    AVFormatContext* fmt = g_formatContext.load();
    if (fmt) {
        avformat_close_input(&fmt);
        g_formatContext.store(nullptr);
    }
    
    // Cleanup OpenGL resources
    if (g_texture != 0) {
        glDeleteTextures(1, &g_texture);
    }
    if (g_shaderProgram != 0) {
        glDeleteProgram(g_shaderProgram);
    }
    if (g_VAO != 0) {
        glDeleteVertexArrays(1, &g_VAO);
    }
    if (g_VBO != 0) {
        glDeleteBuffers(1, &g_VBO);
    }
    if (g_EBO != 0) {
        glDeleteBuffers(1, &g_EBO);
    }
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    // Network deinit is handled in video_decoder.cpp
    
    std::cout << "Application closed, stream disconnected" << std::endl;
    
    return 0;
}
