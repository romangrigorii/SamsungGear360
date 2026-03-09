#include "video_decoder.h"
#include "config.h"
#include "stitching.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
    #include <libavutil/imgutils.h>
}

void videoDecodeThread(const char* url) {
    avformat_network_init();
    
    AVFormatContext* fmt = nullptr;
    AVDictionary* opts = nullptr;
    
    // Store format context for signal handler access
    g_formatContext.store(nullptr);
    
    // Low-latency options (matching ffplay command)
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);
    av_dict_set(&opts, "analyzeduration", "0", 0);
    av_dict_set(&opts, "probesize", "32", 0);
    
    // For MJPEG streams, we may need to hint the format
    // Try opening with format hint
    const char* format_hint = nullptr;
    if (strstr(url, "livestream") != nullptr) {
        // Likely MJPEG stream, try mjpeg format
        format_hint = "mjpeg";
    }
    
    int ret = avformat_open_input(&fmt, url, format_hint ? av_find_input_format(format_hint) : nullptr, &opts);
    if (ret < 0) {
        // If format hint failed, try without it
        if (format_hint) {
            std::cout << "Retrying without format hint..." << std::endl;
            av_dict_free(&opts);
            opts = nullptr;
            av_dict_set(&opts, "fflags", "nobuffer", 0);
            av_dict_set(&opts, "flags", "low_delay", 0);
            av_dict_set(&opts, "analyzeduration", "0", 0);
            av_dict_set(&opts, "probesize", "32", 0);
            ret = avformat_open_input(&fmt, url, nullptr, &opts);
        }
        
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            std::cerr << "Failed to open input: " << url << std::endl;
            std::cerr << "Error: " << errbuf << std::endl;
            std::cerr << "\nTroubleshooting:" << std::endl;
            std::cerr << "1. Verify the URL is correct (try port 7679)" << std::endl;
            std::cerr << "2. Test with: ffplay -i \"" << url << "\"" << std::endl;
            std::cerr << "3. Check network connectivity" << std::endl;
            av_dict_free(&opts);
            g_streamOpenFailed = true;
            g_running = false;
            g_formatContext.store(nullptr);
            return;
        }
    }
    av_dict_free(&opts);
    
    // Store format context for signal handler
    g_formatContext.store(fmt);
    
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::cerr << "Failed to find stream info" << std::endl;
        g_streamOpenFailed = true;
        g_running = false;
        avformat_close_input(&fmt);
        g_formatContext.store(nullptr);
        avformat_network_deinit();
        return;
    }
    
    int videoStream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = (int)i;
            break;
        }
    }
    
    if (videoStream < 0) {
        std::cerr << "No video stream found" << std::endl;
        g_running = false;
        avformat_close_input(&fmt);
        g_formatContext.store(nullptr);
        avformat_network_deinit();
        return;
    }
    
    const AVCodecParameters* par = fmt->streams[videoStream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        std::cerr << "Decoder not found" << std::endl;
        g_running = false;
        avformat_close_input(&fmt);
        g_formatContext.store(nullptr);
        avformat_network_deinit();
        return;
    }
    
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, par);
    
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        std::cerr << "Failed to open decoder" << std::endl;
        g_running = false;
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        g_formatContext.store(nullptr);
        avformat_network_deinit();
        return;
    }
    
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    
    SwsContext* sws = sws_getContext(
        dec->width, dec->height, dec->pix_fmt,
        dec->width, dec->height, AV_PIX_FMT_BGR24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    int bgrStride[4] = { dec->width * 3, 0, 0, 0 };
    std::vector<uint8_t> bgr(dec->width * dec->height * 3);
    uint8_t* bgrData[4] = { bgr.data(), nullptr, nullptr, nullptr };
    
    std::cout << "Video stream opened: " << dec->width << "x" << dec->height << std::endl;
    
    long long frameCount = 0;
    auto startTime = std::chrono::steady_clock::now();
    bool stitchLogPrinted = false;
    
    while (g_running && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != videoStream) {
            av_packet_unref(pkt);
            continue;
        }
        
        if (avcodec_send_packet(dec, pkt) == 0) {
            while (avcodec_receive_frame(dec, frame) == 0) {
                sws_scale(sws, frame->data, frame->linesize, 0, dec->height, bgrData, bgrStride);
                
                auto currentTime = std::chrono::steady_clock::now();
                
                // Analyze stitching on first frame if in equirectangular mode with stitch enabled
                if (g_options.equirectangularMode && g_options.stitchMode && !g_stitchParams.calibrated) {
                    if (!g_stitchParams.firstFrameCollected) {
                        g_stitchParams.firstFrameCollected = true;
                        
                        std::cout << "Stitch analysis: Analyzing first frame..." << std::endl;
                        
                        // Analyze stitching parameters immediately using the current frame
                        analyzeFrameForStitchingImmediate(bgr, dec->width, dec->height);
                        
                        std::cout << "Stitch calibration complete" << std::endl;
                    }
                }
                
                // Log if stitching was requested but calibration failed
                if (g_options.equirectangularMode && g_options.stitchMode && !stitchLogPrinted) {
                    // Check immediately after first frame (calibration should be done)
                    if (g_stitchParams.firstFrameCollected) {
                        if (!g_stitchParams.calibrated || g_stitchParams.lensRadius <= 0.0f ||
                            g_stitchParams.lens1CenterX < 0.0f || g_stitchParams.lens1CenterX > 1.0f) {
                            std::cerr << "Warning: Stitching was requested but could not be applied. "
                                      << "Falling back to non-stitched equirectangular projection." << std::endl;
                            stitchLogPrinted = true;
                        }
                    }
                }
                
                // Update frame data
                {
                    std::lock_guard<std::mutex> lock(g_frameData.mutex);
                    g_frameData.data = bgr;
                    g_frameData.width = dec->width;
                    g_frameData.height = dec->height;
                    g_frameData.updated = true;
                }
                
                frameCount++;
                if (frameCount % 30 == 0) {
                    std::cout << "Decoded frames: " << frameCount << std::endl;
                }
            }
        }
        
        av_packet_unref(pkt);
    }
    
    // Cleanup resources
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    
    // Explicitly close the format context and clear network resources
    if (fmt) {
        avformat_close_input(&fmt);
        g_formatContext.store(nullptr);
    }
    
    // Give network stack time to close connections
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Cleanup network resources
    avformat_network_deinit();
    
    std::cout << "Video decode thread finished, stream closed" << std::endl;
}
