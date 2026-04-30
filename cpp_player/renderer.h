#ifndef RENDERER_H
#define RENDERER_H

#include <GL/glew.h>
#include <vector>

// OpenGL setup and rendering
void setupQuad();
void updateTexture(const std::vector<uint8_t>& data, int width, int height);
void renderFrame();

// OpenGL resources
extern GLuint g_texture;
extern GLuint g_VAO;
extern GLuint g_VBO;
extern GLuint g_EBO;

#endif // RENDERER_H
