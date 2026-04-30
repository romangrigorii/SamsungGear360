#ifndef SHADERS_H
#define SHADERS_H

#include <GL/glew.h>

// Shader compilation
GLuint compileShader(GLenum type, const char* source);
bool createShaders();

// Shader source code
const char* getVertexShaderSource();
const char* getFragmentShaderSource();

// OpenGL resources
extern GLuint g_shaderProgram;

#endif // SHADERS_H
