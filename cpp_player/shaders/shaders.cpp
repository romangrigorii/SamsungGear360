#include "shaders.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>

GLuint g_shaderProgram = 0;

// Helper function to read a file into a string
std::string readShaderFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open shader file: " << filepath << std::endl;
        return "";
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const char* getVertexShaderSource() {
    // Read vertex shader from file
    static std::string vertexSource;
    if (vertexSource.empty()) {
        std::filesystem::path shaderDir = std::filesystem::path(__FILE__).parent_path();
        std::string vertexPath = (shaderDir / "vertex.glsl").string();
        vertexSource = readShaderFile(vertexPath);
        if (vertexSource.empty()) {
            // Fallback to inline shader if file read fails
            vertexSource = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";
        }
    }
    return vertexSource.c_str();
}

const char* getFragmentShaderSource() {
    // Build fragment shader by combining multiple files in order
    static std::string fragmentSource;
    if (fragmentSource.empty()) {
        std::filesystem::path shaderDir = std::filesystem::path(__FILE__).parent_path();
        
        // Order matters: uniforms first, then helpers, masks, projections, stitching, finally main
        std::vector<std::string> shaderFiles = {
            "uniforms.glsl",
            "helpers.glsl",
            "masks.glsl",
            "projections.glsl",
            "stitching.glsl",
            "fragment.glsl"
        };
        
        std::stringstream combined;
        bool versionAdded = false;
        
        for (const auto& filename : shaderFiles) {
            std::string filepath = (shaderDir / filename).string();
            std::string content = readShaderFile(filepath);
            if (content.empty()) {
                std::cerr << "Warning: Could not read shader file: " << filepath << std::endl;
                continue;
            }
            
            // Remove #version directive from content if it exists (only first file should have it)
            if (!versionAdded && content.find("#version") != std::string::npos) {
                size_t versionPos = content.find("#version");
                size_t versionEnd = content.find('\n', versionPos);
                if (versionEnd != std::string::npos) {
                    content = content.substr(versionEnd + 1);
                }
                combined << "#version 330 core\n";
                versionAdded = true;
            } else if (content.find("#version") != std::string::npos) {
                // Remove #version from subsequent files
                size_t versionPos = content.find("#version");
                size_t versionEnd = content.find('\n', versionPos);
                if (versionEnd != std::string::npos) {
                    content = content.substr(versionEnd + 1);
                }
            }
            
            combined << "\n// === " << filename << " ===\n";
            combined << content;
            combined << "\n";
        }
        
        fragmentSource = combined.str();
        
        // If no version was found, add it at the beginning
        if (!versionAdded) {
            fragmentSource = "#version 330 core\n" + fragmentSource;
        }
        
        // Fallback to inline shader if all files fail
        if (fragmentSource.find("#version 330 core") == std::string::npos || 
            fragmentSource.length() < 100) {
            std::cerr << "Error: Failed to load shader files, using fallback" << std::endl;
            fragmentSource = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;
uniform sampler2D ourTexture;
void main() {
    vec4 c = texture(ourTexture, TexCoord);
    FragColor = vec4(c.b, c.g, c.r, c.a);  // BGR to RGB
}
)";
        }
    }
    return fragmentSource.c_str();
}

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "Shader compilation error: " << infoLog << std::endl;
        return 0;
    }
    return shader;
}

bool createShaders() {
    const char* vertexSource = getVertexShaderSource();
    const char* fragmentSource = getFragmentShaderSource();
    
    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    
    if (vertexShader == 0 || fragmentShader == 0) {
        return false;
    }
    
    g_shaderProgram = glCreateProgram();
    glAttachShader(g_shaderProgram, vertexShader);
    glAttachShader(g_shaderProgram, fragmentShader);
    glLinkProgram(g_shaderProgram);
    
    GLint success;
    glGetProgramiv(g_shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(g_shaderProgram, 512, nullptr, infoLog);
        std::cerr << "Shader linking error: " << infoLog << std::endl;
        return false;
    }
    
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    
    return true;
}
