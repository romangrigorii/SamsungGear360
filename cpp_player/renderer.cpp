#include "renderer.h"
#include "config.h"
#include "shaders/shaders.h"
#include <iostream>
#include <vector>

GLuint g_texture = 0;
GLuint g_VAO = 0;
GLuint g_VBO = 0;
GLuint g_EBO = 0;

void setupQuad() {
    float vertices[] = {
        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 1.0f
    };
    
    unsigned int indices[] = {
        0, 1, 2,
        2, 3, 0
    };
    
    glGenVertexArrays(1, &g_VAO);
    glGenBuffers(1, &g_VBO);
    glGenBuffers(1, &g_EBO);
    
    glBindVertexArray(g_VAO);
    
    glBindBuffer(GL_ARRAY_BUFFER, g_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
}

void updateTexture(const std::vector<uint8_t>& data, int width, int height) {
    if (g_texture == 0) {
        glGenTextures(1, &g_texture);
        glBindTexture(GL_TEXTURE_2D, g_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    
    glBindTexture(GL_TEXTURE_2D, g_texture);
    
    // Upload BGR data directly - conversion to RGB is done in the shader
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data.data());
}

void renderFrame() {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    if (g_texture != 0) {
        glUseProgram(g_shaderProgram);
        
        // Set shader uniforms
        GLint rectilinearLoc = glGetUniformLocation(g_shaderProgram, "uRectilinearMode");
        GLint equirectangularLoc = glGetUniformLocation(g_shaderProgram, "uEquirectangularMode");
        GLint fovLoc = glGetUniformLocation(g_shaderProgram, "uFOV");
        GLint stitchLoc = glGetUniformLocation(g_shaderProgram, "uStitchMode");
        GLint stitchCalibratedLoc = glGetUniformLocation(g_shaderProgram, "uStitchCalibrated");
        GLint lens1CenterLoc = glGetUniformLocation(g_shaderProgram, "uLens1Center");
        GLint lens2CenterLoc = glGetUniformLocation(g_shaderProgram, "uLens2Center");
        GLint lensRadiusLoc = glGetUniformLocation(g_shaderProgram, "uLensRadius");
        GLint innerRadiusRatioLoc = glGetUniformLocation(g_shaderProgram, "uInnerRadiusRatio");
        GLint isHorizontalLoc = glGetUniformLocation(g_shaderProgram, "uIsHorizontal");
        GLint textureSizeLoc = glGetUniformLocation(g_shaderProgram, "uTextureSize");
        GLint alignmentOffset1Loc = glGetUniformLocation(g_shaderProgram, "uAlignmentOffset1");
        GLint alignmentOffset2Loc = glGetUniformLocation(g_shaderProgram, "uAlignmentOffset2");
        GLint enableCircularCropLoc = glGetUniformLocation(g_shaderProgram, "uEnableCircularCrop");
        GLint enableLightFalloffLoc = glGetUniformLocation(g_shaderProgram, "uEnableLightFalloff");
        
        // New calibration uniforms
        GLint lens1FOVLoc = glGetUniformLocation(g_shaderProgram, "uLens1FOV");
        GLint lens2FOVLoc = glGetUniformLocation(g_shaderProgram, "uLens2FOV");
        GLint lens1DistortionLoc = glGetUniformLocation(g_shaderProgram, "uLens1Distortion");
        GLint lens2DistortionLoc = glGetUniformLocation(g_shaderProgram, "uLens2Distortion");
        GLint lens1RotationLoc = glGetUniformLocation(g_shaderProgram, "uLens1Rotation");
        GLint lens2RotationLoc = glGetUniformLocation(g_shaderProgram, "uLens2Rotation");
        GLint rotationLoc = glGetUniformLocation(g_shaderProgram, "uRotation");
        GLint fromCalibrationFileLoc = glGetUniformLocation(g_shaderProgram, "uFromCalibrationFile");
        
        glUniform1i(rectilinearLoc, g_options.rectilinearMode ? 1 : 0);
        glUniform1i(equirectangularLoc, g_options.equirectangularMode ? 1 : 0);
        glUniform1f(fovLoc, g_options.fov);
        glUniform1i(stitchLoc, g_options.stitchMode ? 1 : 0);
        glUniform1i(stitchCalibratedLoc, g_stitchParams.calibrated ? 1 : 0);
        glUniform2f(lens1CenterLoc, g_stitchParams.lens1CenterX, g_stitchParams.lens1CenterY);
        glUniform2f(lens2CenterLoc, g_stitchParams.lens2CenterX, g_stitchParams.lens2CenterY);
        glUniform1f(lensRadiusLoc, g_stitchParams.lensRadius);
        glUniform1f(innerRadiusRatioLoc, g_stitchParams.innerRadiusRatio);
        glUniform1i(isHorizontalLoc, g_stitchParams.isHorizontal ? 1 : 0);
        glUniform2f(textureSizeLoc, (float)g_frameData.width, (float)g_frameData.height);
        glUniform2f(alignmentOffset1Loc, g_stitchParams.alignmentOffset1X, g_stitchParams.alignmentOffset1Y);
        glUniform2f(alignmentOffset2Loc, g_stitchParams.alignmentOffset2X, g_stitchParams.alignmentOffset2Y);
        // Enable circular crop and light falloff when stitching is enabled
        glUniform1i(enableCircularCropLoc, (g_options.stitchMode && g_stitchParams.calibrated) ? 1 : 0);
        glUniform1i(enableLightFalloffLoc, g_options.enableLightFalloffCompensation ? 1 : 0);
        
        // Set new calibration uniforms
        glUniform1f(lens1FOVLoc, g_stitchParams.lens1FOV);
        glUniform1f(lens2FOVLoc, g_stitchParams.lens2FOV);
        glUniform4f(lens1DistortionLoc, g_stitchParams.lens1P1, g_stitchParams.lens1P2, 
                    g_stitchParams.lens1P3, g_stitchParams.lens1P4);
        glUniform4f(lens2DistortionLoc, g_stitchParams.lens2P1, g_stitchParams.lens2P2, 
                    g_stitchParams.lens2P3, g_stitchParams.lens2P4);
        glUniform3f(lens1RotationLoc, g_stitchParams.lens1RotationYaw, g_stitchParams.lens1RotationPitch, 
                    g_stitchParams.lens1RotationRoll);
        glUniform3f(lens2RotationLoc, g_stitchParams.lens2RotationYaw, g_stitchParams.lens2RotationPitch, 
                    g_stitchParams.lens2RotationRoll);
        glUniform3f(rotationLoc, g_stitchParams.rotationYaw, g_stitchParams.rotationPitch, 
                    g_stitchParams.rotationRoll);
        glUniform1i(fromCalibrationFileLoc, g_stitchParams.fromCalibrationFile ? 1 : 0);
        
        // Log if stitching was requested but not applied (check once per render)
        static bool stitchWarningLogged = false;
        if (g_options.equirectangularMode && g_options.stitchMode && !stitchWarningLogged) {
            if (!g_stitchParams.calibrated || g_stitchParams.lensRadius <= 0.0f ||
                g_stitchParams.lens1CenterX < 0.0f || g_stitchParams.lens1CenterX > 1.0f ||
                g_stitchParams.lens1CenterY < 0.0f || g_stitchParams.lens1CenterY > 1.0f ||
                g_stitchParams.lens2CenterX < 0.0f || g_stitchParams.lens2CenterX > 1.0f ||
                g_stitchParams.lens2CenterY < 0.0f || g_stitchParams.lens2CenterY > 1.0f) {
                std::cerr << "Warning: Stitching was requested but could not be applied. "
                          << "Falling back to non-stitched equirectangular projection." << std::endl;
                stitchWarningLogged = true;
            }
        }
        
        glBindTexture(GL_TEXTURE_2D, g_texture);
        glBindVertexArray(g_VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
}
