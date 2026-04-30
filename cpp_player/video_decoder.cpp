#include "video_decoder.h"
#include "config.h"
#include "stitching.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/error.h>
}

static void logAvError(const char* what, int err)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, errbuf, sizeof(errbuf));
    std::cerr << what << ": " << errbuf << " (" << err << ")" << std::endl;
}

void videoDecodeThread(const char* url) {
    avformat_network_init();
    
    AVFormatContext* fmt = nullptr;
    AVDictionary* opts = nullptr;
    
    // Store format context for signal handler access
    g_formatContext.store(nullptr);
    
    // Low latency, but probesize must be large enough for MJPEG to read a full JPEG
    // header (tiny probe leaves width/height at 0 and breaks sws_getContext).
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);
    av_dict_set(&opts, "analyzeduration", "2000000", 0);
    av_dict_set(&opts, "probesize", "1048576", 0);
    
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
            av_dict_set(&opts, "analyzeduration", "2000000", 0);
            av_dict_set(&opts, "probesize", "1048576", 0);
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
    
    int fsret = avformat_find_stream_info(fmt, nullptr);
    if (fsret < 0) {
        logAvError("avformat_find_stream_info", fsret);
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
    
    int openRet = avcodec_open2(dec, codec, nullptr);
    if (openRet < 0) {
        logAvError("avcodec_open2", openRet);
        g_running = false;
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        g_formatContext.store(nullptr);
        avformat_network_deinit();
        return;
    }
    
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    // MJPEG often has width/height == 0 in the codec context until the first frame is
    // decoded. Creating SwsContext with 0x0 hits libswscale assertions — init lazily.
    SwsContext* sws = nullptr;
    std::vector<uint8_t> bgr;
    int bgrStride[4] = { 0, 0, 0, 0 };
    uint8_t* bgrData[4] = { nullptr, nullptr, nullptr, nullptr };
    int swsFrameW = 0;
    int swsFrameH = 0;
    AVPixelFormat swsInFmt = AV_PIX_FMT_NONE;

    {
        std::string sizeHint;
        if (dec->width > 0 && dec->height > 0)
            sizeHint = " (" + std::to_string(dec->width) + "x" + std::to_string(dec->height) + " from probe)";
        else
            sizeHint = " (size from first frame)";
        std::cout << "Video decoder ready" << sizeHint << std::endl;
    }

    long long frameCount = 0;
    bool stitchLogPrinted = false;

    // One place for frame output + texture upload prep (also used when flushing decoder at EOF).
    const auto processDecodedFrame = [&]() {
        if (frame->width <= 0 || frame->height <= 0)
            return;
        const AVPixelFormat inFmt = static_cast<AVPixelFormat>(frame->format);
        if (inFmt == AV_PIX_FMT_NONE)
            return;

        if (!sws || swsFrameW != frame->width || swsFrameH != frame->height || swsInFmt != inFmt) {
            sws_freeContext(sws);
            sws = nullptr;
            sws = sws_getContext(
                frame->width, frame->height, inFmt,
                frame->width, frame->height, AV_PIX_FMT_BGR24,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            if (!sws) {
                std::cerr << "sws_getContext failed for " << frame->width << "x" << frame->height
                          << " format=" << inFmt << std::endl;
                g_streamOpenFailed = true;
                g_running = false;
                return;
            }
            bgr.assign(static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 3u, 0);
            bgrStride[0] = frame->width * 3;
            bgrData[0] = bgr.data();
            swsFrameW = frame->width;
            swsFrameH = frame->height;
            swsInFmt = inFmt;
            std::cout << "SwsContext: " << frame->width << "x" << frame->height << " -> BGR24" << std::endl;
        }

        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, bgrData, bgrStride);

        // Analyze stitching on first frame if in equirectangular mode with stitch enabled
        if (g_options.equirectangularMode && g_options.stitchMode && !g_stitchParams.calibrated) {
            if (!g_stitchParams.firstFrameCollected) {
                g_stitchParams.firstFrameCollected = true;

                std::cout << "Stitch analysis: Analyzing first frame..." << std::endl;

                analyzeFrameForStitchingImmediate(bgr, frame->width, frame->height);

                std::cout << "Stitch calibration complete" << std::endl;
            }
        }

        if (g_options.equirectangularMode && g_options.stitchMode && !stitchLogPrinted) {
            if (g_stitchParams.firstFrameCollected) {
                if (!g_stitchParams.calibrated || g_stitchParams.lensRadius <= 0.0f ||
                    g_stitchParams.lens1CenterX < 0.0f || g_stitchParams.lens1CenterX > 1.0f) {
                    std::cerr << "Warning: Stitching was requested but could not be applied. "
                              << "Falling back to non-stitched equirectangular projection." << std::endl;
                    stitchLogPrinted = true;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_frameData.mutex);
            g_frameData.data = bgr;
            g_frameData.width = frame->width;
            g_frameData.height = frame->height;
            g_frameData.updated = true;
        }

        frameCount++;
        if (frameCount % 30 == 0) {
            std::cout << "Decoded frames: " << frameCount << std::endl;
        }
    };

    auto drainDecodedFrames = [&]() {
        while (g_running) {
            int recvRet = avcodec_receive_frame(dec, frame);
            if (recvRet == AVERROR(EAGAIN) || recvRet == AVERROR_EOF)
                break;
            if (recvRet < 0) {
                logAvError("avcodec_receive_frame", recvRet);
                break;
            }
            processDecodedFrame();
        }
    };

    while (g_running && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != videoStream) {
            av_packet_unref(pkt);
            continue;
        }

        int sendRet = avcodec_send_packet(dec, pkt);
        // AVERROR(EAGAIN): decoder needs output drained before accepting more input — still receive frames.
        if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
            logAvError("avcodec_send_packet", sendRet);
        }

        drainDecodedFrames();

        av_packet_unref(pkt);
    }

    // Flush decoder after demuxer EOF (libavcodec needs a NULL packet to emit buffered frames).
    if (g_running) {
        int flushSend = avcodec_send_packet(dec, nullptr);
        if (flushSend < 0 && flushSend != AVERROR_EOF && flushSend != AVERROR(EAGAIN))
            logAvError("avcodec_send_packet (flush)", flushSend);
        for (;;) {
            int recvRet = avcodec_receive_frame(dec, frame);
            if (recvRet == AVERROR_EOF)
                break;
            if (recvRet == AVERROR(EAGAIN))
                break;
            if (recvRet < 0) {
                logAvError("avcodec_receive_frame (flush)", recvRet);
                break;
            }
            processDecodedFrame();
        }
    }
    
    // Cleanup resources
    if (sws)
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
