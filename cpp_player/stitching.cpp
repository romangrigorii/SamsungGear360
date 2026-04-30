#include "stitching.h"
#include "config.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <vector>

// Try to use OpenCV for lens center detection if available
// OpenCV provides better lens center detection and alignment computation
#ifdef HAVE_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#endif

// Simple center detection using image statistics (no OpenCV required)
// Finds the center of mass of bright pixels, which should approximate the lens center
bool detectLensCenterSimple(const std::vector<uint8_t>& data, int width, int height,
                           int lensX, int lensY, int lensW, int lensH,
                           float& centerX, float& centerY, float& radius) {
    // Extract lens region
    int totalBrightness = 0;
    int sumX = 0, sumY = 0;
    int pixelCount = 0;
    
    // Threshold for "bright" pixels (fisheye images are usually brighter in center)
    const int brightnessThreshold = 128;
    
    for (int y = 0; y < lensH; y++) {
        for (int x = 0; x < lensW; x++) {
            int globalX = lensX + x;
            int globalY = lensY + y;
            int idx = (globalY * width + globalX) * 3;
            
            if (idx + 2 < (int)data.size()) {
                // BGR format
                int b = data[idx];
                int g = data[idx + 1];
                int r = data[idx + 2];
                int brightness = (r + g + b) / 3;
                
                if (brightness > brightnessThreshold) {
                    sumX += x;
                    sumY += y;
                    totalBrightness += brightness;
                    pixelCount++;
                }
            }
        }
    }
    
    if (pixelCount > lensW * lensH * 0.1) {  // At least 10% bright pixels
        centerX = (float)sumX / pixelCount / lensW;
        centerY = (float)sumY / pixelCount / lensH;
        radius = std::min(lensW, lensH) * 0.48f;
        return true;
    }
    
    return false;
}

bool detectLensCenter(const std::vector<uint8_t>& data, int width, int height,
                      int lensX, int lensY, int lensW, int lensH,
                      float& centerX, float& centerY, float& radius) {
#ifdef HAVE_OPENCV
    try {
        // Extract lens region from full image
        cv::Mat fullImage(height, width, CV_8UC3, (void*)data.data());
        cv::Rect lensRect(lensX, lensY, lensW, lensH);
        cv::Mat lensImage = fullImage(lensRect).clone();
        
        // Convert to grayscale
        cv::Mat gray;
        cv::cvtColor(lensImage, gray, cv::COLOR_BGR2GRAY);
        
        // Apply Gaussian blur to reduce noise
        cv::Mat blurred;
        cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 1.5);
        
        // Try multiple HoughCircles parameter sets for robustness
        std::vector<cv::Vec3f> circles;
        
        // First attempt: standard parameters
        cv::HoughCircles(blurred, circles, cv::HOUGH_GRADIENT, 1,
                         lensH / 8,  // min distance between centers
                         100,        // upper threshold for edge detection
                         30,         // accumulator threshold
                         lensH / 4,  // min radius
                         lensH / 2); // max radius
        
        // Second attempt: more lenient parameters if first failed
        if (circles.empty()) {
            cv::HoughCircles(blurred, circles, cv::HOUGH_GRADIENT, 2,
                             lensH / 16,  // smaller min distance
                             80,          // lower threshold
                             20,          // lower accumulator threshold
                             lensH / 6,   // smaller min radius
                             lensH * 0.6); // larger max radius
        }
        
        if (!circles.empty()) {
            // Use the largest circle (most likely the fisheye disc)
            auto largestCircle = std::max_element(circles.begin(), circles.end(),
                [](const cv::Vec3f& a, const cv::Vec3f& b) {
                    return a[2] < b[2];  // Compare radius
                });
            
            if (largestCircle != circles.end()) {
                // Convert to lens-local normalized coordinates [0,1]
                centerX = (*largestCircle)[0] / lensW;
                centerY = (*largestCircle)[1] / lensH;
                radius = (*largestCircle)[2];
                std::cout << "  OpenCV HoughCircles detected center: (" << centerX << ", " << centerY 
                          << "), radius: " << radius << "px" << std::endl;
                return true;
            }
        }
        
        // Fallback: try to find the largest circle using contour detection
        cv::Mat edges;
        cv::Canny(blurred, edges, 50, 150);
        
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(edges, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        
        if (!contours.empty()) {
            // Find the largest contour (likely the fisheye circle)
            auto largestContour = std::max_element(contours.begin(), contours.end(),
                [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
                    return cv::contourArea(a) < cv::contourArea(b);
                });
            
            if (largestContour != contours.end() && largestContour->size() >= 5) {
                cv::Point2f center;
                float r;
                cv::minEnclosingCircle(*largestContour, center, r);
                
                // Convert to lens-local normalized coordinates
                centerX = center.x / lensW;
                centerY = center.y / lensH;
                radius = r;
                std::cout << "  OpenCV contour detection found center: (" << centerX << ", " << centerY 
                          << "), radius: " << radius << "px" << std::endl;
                return true;
            }
        }
        
        std::cout << "  OpenCV detection failed, trying simple method..." << std::endl;
    } catch (const cv::Exception& e) {
        std::cerr << "  OpenCV error in lens center detection: " << e.what() << std::endl;
    }
#endif
    
    // Fallback: use simple brightness-based detection
    if (detectLensCenterSimple(data, width, height, lensX, lensY, lensW, lensH, 
                               centerX, centerY, radius)) {
        std::cout << "  Simple brightness-based detection found center: (" << centerX << ", " 
                  << centerY << "), radius: " << radius << "px" << std::endl;
        return true;
    }
    
    // Final fallback: assume center
    centerX = 0.5f;
    centerY = 0.5f;
    radius = std::min(lensW, lensH) * 0.48f;
    return false;
}

// Simple normalized cross-correlation for template matching (no OpenCV required)
// Returns correlation value in [0, 1] where 1.0 is perfect match
float computeNCC(const uint8_t* templateData, int templateW, int templateH,
                 const uint8_t* searchData, int searchW, int searchH,
                 int searchX, int searchY) {
    float sumTemplate = 0.0f, sumSearch = 0.0f;
    float sumTemplateSq = 0.0f, sumSearchSq = 0.0f;
    float sumProduct = 0.0f;
    int count = 0;
    
    for (int y = 0; y < templateH; y++) {
        for (int x = 0; x < templateW; x++) {
            int templateIdx = (y * templateW + x) * 3;
            int searchIdx = ((searchY + y) * searchW + (searchX + x)) * 3;
            
            if (searchX + x < searchW && searchY + y < searchH) {
                // Convert BGR to grayscale
                int templateGray = (int)templateData[templateIdx] + 
                                   (int)templateData[templateIdx + 1] + 
                                   (int)templateData[templateIdx + 2];
                int searchGray = (int)searchData[searchIdx] + 
                                (int)searchData[searchIdx + 1] + 
                                (int)searchData[searchIdx + 2];
                
                templateGray /= 3;
                searchGray /= 3;
                
                sumTemplate += templateGray;
                sumSearch += searchGray;
                sumTemplateSq += templateGray * templateGray;
                sumSearchSq += searchGray * searchGray;
                sumProduct += templateGray * searchGray;
                count++;
            }
        }
    }
    
    if (count == 0) return 0.0f;
    
    float meanTemplate = sumTemplate / count;
    float meanSearch = sumSearch / count;
    
    float varTemplate = sumTemplateSq / count - meanTemplate * meanTemplate;
    float varSearch = sumSearchSq / count - meanSearch * meanSearch;
    
    if (varTemplate < 1e-6f || varSearch < 1e-6f) return 0.0f;
    
    float covariance = (sumProduct / count) - (meanTemplate * meanSearch);
    float correlation = covariance / std::sqrt(varTemplate * varSearch);
    
    // Normalize to [0, 1] range (NCC is typically [-1, 1])
    return (correlation + 1.0f) * 0.5f;
}

// Simple template matching without OpenCV
bool computeAlignmentOffsetsSimple(const std::vector<uint8_t>& data, int width, int height,
                                  int lensW, int lensH, bool isHorizontal,
                                  float& offset1X, float& offset1Y, float& offset2X, float& offset2Y) {
    if (isHorizontal) {
        // Horizontal layout: match right edge of left lens with left edge of right lens
        int overlapWidth = lensW / 4;  // 25% overlap region
        int templateStartX = lensW - overlapWidth;  // Right edge of left lens
        int searchStartX = lensW;  // Left edge of right lens
        int searchWidth = overlapWidth * 2;  // Search window
        
        // Extract template from left lens (right edge)
        std::vector<uint8_t> templateData(overlapWidth * lensH * 3);
        for (int y = 0; y < lensH; y++) {
            for (int x = 0; x < overlapWidth; x++) {
                int srcIdx = (y * width + (templateStartX + x)) * 3;
                int dstIdx = (y * overlapWidth + x) * 3;
                if (srcIdx + 2 < (int)data.size() && dstIdx + 2 < (int)templateData.size()) {
                    templateData[dstIdx] = data[srcIdx];
                    templateData[dstIdx + 1] = data[srcIdx + 1];
                    templateData[dstIdx + 2] = data[srcIdx + 2];
                }
            }
        }
        
        // Search in right lens (left edge)
        float bestCorrelation = 0.0f;
        int bestOffsetX = 0;
        
        for (int offsetX = -overlapWidth / 2; offsetX <= overlapWidth / 2; offsetX++) {
            int searchX = searchStartX + offsetX;
            if (searchX < 0 || searchX + overlapWidth > width) continue;
            
            float correlation = computeNCC(
                templateData.data(), overlapWidth, lensH,
                data.data(), width, height,
                searchX, 0
            );
            
            if (correlation > bestCorrelation) {
                bestCorrelation = correlation;
                bestOffsetX = offsetX;
            }
        }
        
        if (bestCorrelation > 0.5f) {
            offset2X = bestOffsetX / (float)lensW;
            offset2Y = 0.0f;
            std::cout << "  Simple template matching found offset: (" << offset2X << ", " << offset2Y 
                      << ") with correlation: " << bestCorrelation << std::endl;
            return true;
        } else {
            std::cout << "  Simple template matching correlation too low: " << bestCorrelation 
                      << " (threshold: 0.5)" << std::endl;
        }
    } else {
        // Vertical layout: match bottom edge of top lens with top edge of bottom lens
        int overlapHeight = lensH / 4;
        int templateStartY = lensH - overlapHeight;
        int searchStartY = lensH;
        int searchHeight = overlapHeight * 2;
        
        // Extract template from top lens (bottom edge)
        std::vector<uint8_t> templateData(lensW * overlapHeight * 3);
        for (int y = 0; y < overlapHeight; y++) {
            for (int x = 0; x < lensW; x++) {
                int srcIdx = ((templateStartY + y) * width + x) * 3;
                int dstIdx = (y * lensW + x) * 3;
                if (srcIdx + 2 < (int)data.size() && dstIdx + 2 < (int)templateData.size()) {
                    templateData[dstIdx] = data[srcIdx];
                    templateData[dstIdx + 1] = data[srcIdx + 1];
                    templateData[dstIdx + 2] = data[srcIdx + 2];
                }
            }
        }
        
        // Search in bottom lens (top edge)
        float bestCorrelation = 0.0f;
        int bestOffsetY = 0;
        
        for (int offsetY = -overlapHeight / 2; offsetY <= overlapHeight / 2; offsetY++) {
            int searchY = searchStartY + offsetY;
            if (searchY < 0 || searchY + overlapHeight > height) continue;
            
            float correlation = computeNCC(
                templateData.data(), lensW, overlapHeight,
                data.data(), width, height,
                0, searchY
            );
            
            if (correlation > bestCorrelation) {
                bestCorrelation = correlation;
                bestOffsetY = offsetY;
            }
        }
        
        if (bestCorrelation > 0.5f) {
            offset2Y = bestOffsetY / (float)lensH;
            offset2X = 0.0f;
            std::cout << "  Simple template matching found offset: (" << offset2X << ", " << offset2Y 
                      << ") with correlation: " << bestCorrelation << std::endl;
            return true;
        } else {
            std::cout << "  Simple template matching correlation too low: " << bestCorrelation 
                      << " (threshold: 0.5)" << std::endl;
        }
    }
    
    return false;
}

// Compute alignment offsets using template matching in overlap region
// Returns true if alignment was computed successfully
bool computeAlignmentOffsets(const std::vector<uint8_t>& data, int width, int height,
                            int lensW, int lensH, bool isHorizontal,
                            float& offset1X, float& offset1Y, float& offset2X, float& offset2Y) {
#ifdef HAVE_OPENCV
    try {
        cv::Mat image(height, width, CV_8UC3, (void*)data.data());
        cv::Mat gray;
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
        
        if (isHorizontal) {
            // Horizontal layout: overlap is in the middle vertical strip
            int overlapWidth = lensW / 4;  // Use 25% of lens width as overlap region
            int leftStart = lensW - overlapWidth;
            int rightStart = lensW;
            
            // Extract template from left lens (right edge)
            cv::Rect leftTemplateRect(leftStart, 0, overlapWidth, lensH);
            cv::Mat leftTemplate = gray(leftTemplateRect).clone();
            
            // Search region in right lens (left edge)
            cv::Rect rightSearchRect(rightStart, 0, overlapWidth * 2, lensH);
            cv::Mat rightSearch = gray(rightSearchRect).clone();
            
            // Template matching
            cv::Mat result;
            cv::matchTemplate(rightSearch, leftTemplate, result, cv::TM_CCOEFF_NORMED);
            
            // Find best match
            double minVal, maxVal;
            cv::Point minLoc, maxLoc;
            cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
            
            if (maxVal > 0.5) {  // Good match threshold
                // Calculate offset in pixels
                int offsetPx = maxLoc.x - overlapWidth;
                // Convert to lens-local normalized UV
                offset2X = offsetPx / (float)lensW;
                offset2Y = 0.0f;  // Assume horizontal alignment only for now
                std::cout << "  Template matching found offset: (" << offset2X << ", " << offset2Y 
                          << ") with confidence: " << maxVal << std::endl;
                return true;
            } else {
                std::cout << "  Template matching confidence too low: " << maxVal 
                          << " (threshold: 0.5)" << std::endl;
            }
        } else {
            // Vertical layout: overlap is in the middle horizontal strip
            int overlapHeight = lensH / 4;
            int topStart = lensH - overlapHeight;
            int bottomStart = lensH;
            
            // Extract template from top lens (bottom edge)
            cv::Rect topTemplateRect(0, topStart, lensW, overlapHeight);
            cv::Mat topTemplate = gray(topTemplateRect).clone();
            
            // Search region in bottom lens (top edge)
            cv::Rect bottomSearchRect(0, bottomStart, lensW, overlapHeight * 2);
            cv::Mat bottomSearch = gray(bottomSearchRect).clone();
            
            // Template matching
            cv::Mat result;
            cv::matchTemplate(bottomSearch, topTemplate, result, cv::TM_CCOEFF_NORMED);
            
            // Find best match
            double minVal, maxVal;
            cv::Point minLoc, maxLoc;
            cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
            
            if (maxVal > 0.5) {
                int offsetPx = maxLoc.y - overlapHeight;
                offset2Y = offsetPx / (float)lensH;
                offset2X = 0.0f;
                std::cout << "  Template matching found offset: (" << offset2X << ", " << offset2Y 
                          << ") with confidence: " << maxVal << std::endl;
                return true;
            } else {
                std::cout << "  Template matching confidence too low: " << maxVal 
                          << " (threshold: 0.5)" << std::endl;
            }
        }
    } catch (const cv::Exception& e) {
        std::cerr << "  OpenCV error in alignment computation: " << e.what() << std::endl;
    }
#else
    // Use simple template matching without OpenCV
    std::cout << "  OpenCV not available, using simple template matching..." << std::endl;
    if (computeAlignmentOffsetsSimple(data, width, height, lensW, lensH, isHorizontal,
                                      offset1X, offset1Y, offset2X, offset2Y)) {
        return true;
    }
#endif

#ifdef HAVE_OPENCV
    // OpenCV template matching failed, try simple method as fallback
    std::cout << "  OpenCV matching failed, trying simple fallback..." << std::endl;
    if (computeAlignmentOffsetsSimple(data, width, height, lensW, lensH, isHorizontal,
                                     offset1X, offset1Y, offset2X, offset2Y)) {
        return true;
    }
#endif
    
    // No alignment computed
    offset1X = 0.0f;
    offset1Y = 0.0f;
    offset2X = 0.0f;
    offset2Y = 0.0f;
    return false;
}

// Analyze a single frame immediately to extract stitching parameters
// Resolution-independent: works for any input size (e.g., 1280x640, 3840x1920, etc.)
void analyzeFrameForStitchingImmediate(const std::vector<uint8_t>& data, int width, int height) {
    // Determine layout based on aspect ratio
    bool isHorizontal = width > height;
    
    // Calculate lens parameters based on image dimensions
    // For dual fisheye: each lens is half the width (horizontal) or height (vertical)
    int lensWidth, lensHeight;
    int lens1X, lens1Y, lens2X, lens2Y;
    
    if (isHorizontal) {
        // Horizontal layout: side-by-side lenses
        lensWidth = width / 2;   // Each lens is half the total width
        lensHeight = height;     // Each lens uses full height
        
        lens1X = 0;
        lens1Y = 0;
        lens2X = lensWidth;
        lens2Y = 0;
    } else {
        // Vertical layout: top-bottom lenses
        lensWidth = width;       // Each lens uses full width
        lensHeight = height / 2; // Each lens is half the total height
        
        lens1X = 0;
        lens1Y = 0;
        lens2X = 0;
        lens2Y = lensHeight;
    }
    
    // Detect lens centers from actual image
    std::cout << "Detecting lens centers..." << std::endl;
    float center1X, center1Y, radius1;
    float center2X, center2Y, radius2;
    
    std::cout << "  Lens 1 region: (" << lens1X << ", " << lens1Y << ") size " 
              << lensWidth << "x" << lensHeight << std::endl;
    bool detected1 = detectLensCenter(data, width, height, lens1X, lens1Y, lensWidth, lensHeight,
                                      center1X, center1Y, radius1);
    
    std::cout << "  Lens 2 region: (" << lens2X << ", " << lens2Y << ") size " 
              << lensWidth << "x" << lensHeight << std::endl;
    bool detected2 = detectLensCenter(data, width, height, lens2X, lens2Y, lensWidth, lensHeight,
                                      center2X, center2Y, radius2);
    
    if (detected1 && detected2) {
        // Use detected centers
        g_stitchParams.lens1CenterX = center1X;
        g_stitchParams.lens1CenterY = center1Y;
        g_stitchParams.lens2CenterX = center2X;
        g_stitchParams.lens2CenterY = center2Y;
        // Use average radius
        g_stitchParams.lensRadius = (radius1 + radius2) * 0.5f;
        std::cout << "✓ Lens centers successfully detected from image" << std::endl;
    } else if (detected1 || detected2) {
        // Partial detection - use detected one and assume center for the other
        if (detected1) {
            g_stitchParams.lens1CenterX = center1X;
            g_stitchParams.lens1CenterY = center1Y;
            g_stitchParams.lens2CenterX = 0.5f;
            g_stitchParams.lens2CenterY = 0.5f;
            g_stitchParams.lensRadius = radius1;
            std::cout << "✓ Lens 1 detected, using geometric assumption for lens 2" << std::endl;
        } else {
            g_stitchParams.lens1CenterX = 0.5f;
            g_stitchParams.lens1CenterY = 0.5f;
            g_stitchParams.lens2CenterX = center2X;
            g_stitchParams.lens2CenterY = center2Y;
            g_stitchParams.lensRadius = radius2;
            std::cout << "✓ Lens 2 detected, using geometric assumption for lens 1" << std::endl;
        }
    } else {
        // Fallback to geometric assumption
        g_stitchParams.lens1CenterX = 0.5f;
        g_stitchParams.lens1CenterY = 0.5f;
        g_stitchParams.lens2CenterX = 0.5f;
        g_stitchParams.lens2CenterY = 0.5f;
        g_stitchParams.lensRadius = std::min(lensWidth, lensHeight) * 0.48f;
        std::cout << "✗ Lens center detection failed, using geometric assumption (0.5, 0.5)" << std::endl;
    }
    
    // Inner radius ratio for ring-based blending (default 0.85 = 85%)
    g_stitchParams.innerRadiusRatio = 0.85f;
    
    g_stitchParams.isHorizontal = isHorizontal;
    g_stitchParams.lensWidth = lensWidth;
    g_stitchParams.lensHeight = lensHeight;
    
    // Compute alignment offsets using template matching
    std::cout << "Computing alignment offsets..." << std::endl;
    float offset1X, offset1Y, offset2X, offset2Y;
    bool alignmentComputed = computeAlignmentOffsets(data, width, height, lensWidth, lensHeight,
                                                     isHorizontal, offset1X, offset1Y, offset2X, offset2Y);
    
    if (alignmentComputed) {
        g_stitchParams.alignmentOffset1X = offset1X;
        g_stitchParams.alignmentOffset1Y = offset1Y;
        g_stitchParams.alignmentOffset2X = offset2X;
        g_stitchParams.alignmentOffset2Y = offset2Y;
        std::cout << "✓ Alignment offsets computed: Lens1=(" << offset1X << "," << offset1Y 
                  << "), Lens2=(" << offset2X << "," << offset2Y << ")" << std::endl;
    } else {
        g_stitchParams.alignmentOffset1X = 0.0f;
        g_stitchParams.alignmentOffset1Y = 0.0f;
        g_stitchParams.alignmentOffset2X = 0.0f;
        g_stitchParams.alignmentOffset2Y = 0.0f;
        std::cout << "✗ Alignment computation failed, using zero offsets" << std::endl;
    }
    
    std::cout << "Stitch analysis complete (resolution-independent):" << std::endl;
    std::cout << "  Input resolution: " << width << "x" << height << std::endl;
    std::cout << "  Layout: " << (g_stitchParams.isHorizontal ? "Horizontal" : "Vertical") << std::endl;
    std::cout << "  Lens dimensions: " << lensWidth << "x" << lensHeight << " pixels" << std::endl;
    std::cout << "  Lens 1 center: (" << g_stitchParams.lens1CenterX << ", " 
              << g_stitchParams.lens1CenterY << ")" << std::endl;
    std::cout << "  Lens 2 center: (" << g_stitchParams.lens2CenterX << ", " 
              << g_stitchParams.lens2CenterY << ")" << std::endl;
    std::cout << "  Lens radius (pixels, outer): " << g_stitchParams.lensRadius << std::endl;
    std::cout << "  Inner radius ratio: " << g_stitchParams.innerRadiusRatio 
              << " (inner radius: " << (g_stitchParams.lensRadius * g_stitchParams.innerRadiusRatio) << "px)" << std::endl;
    
    // Mark as calibrated
    g_stitchParams.calibrated = true;
}

// Compute frame alignment offsets - delegates to computeAlignmentOffsets
bool computeFrameAlignment(const std::vector<uint8_t>& data, int width, int height,
                          float& offset1X, float& offset1Y, float& offset2X, float& offset2Y) {
    // Determine layout
    bool isHorizontal = width > height;
    int lensW = isHorizontal ? (width / 2) : width;
    int lensH = isHorizontal ? height : (height / 2);
    
    return computeAlignmentOffsets(data, width, height, lensW, lensH, isHorizontal,
                                   offset1X, offset1Y, offset2X, offset2Y);
}
