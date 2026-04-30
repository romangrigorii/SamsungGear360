#ifndef STITCHING_H
#define STITCHING_H

#include "config.h"
#include <vector>

// Analyze a single frame to extract stitching parameters (immediate analysis)
void analyzeFrameForStitchingImmediate(const std::vector<uint8_t>& data, int width, int height);

// Compute frame alignment offsets using template matching
bool computeFrameAlignment(const std::vector<uint8_t>& data, int width, int height,
                          float& offset1X, float& offset1Y, float& offset2X, float& offset2Y);

#endif // STITCHING_H
