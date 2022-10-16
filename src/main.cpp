extern "C" {
#include "../include/capture/nvfbc.h"
#include "../include/capture/xcomposite.h"
#include "../include/gl.h"
#include "../include/time.h"
}

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <map>
#include <signal.h>
#include <sys/stat.h>

#include <unistd.h>
#include <fcntl.h>

#include "../include/sound.hpp"

#include <X11/extensions/Xrandr.h>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

#include <deque>
#include <future>

// TODO: Remove LIBAVUTIL_VERSION_MAJOR checks in the future when ubuntu, pop os LTS etc update ffmpeg to >= 5.0

static const int VIDEO_STREAM_INDEX = 0;

static thread_local char av_error_buffer[AV_ERROR_MAX_STRING_SIZE];

static const XRRModeInfo* get_mode_info(const XRRScreenResources *sr, RRMode id) {
    for(int i = 0; i < sr->nmode; ++i) {
        if(sr->modes[i].id == id)
            return &sr->modes[i];
    }    
    return nullptr;
}

typedef void (*active_monitor_callback)(const XRROutputInfo *output_info, const XRRCrtcInfo *crt_info, const XRRModeInfo *mode_info, void *userdata);

static void for_each_active_monitor_output(Display *display, active_monitor_callback callback, void *userdata) {
    XRRScreenResources *screen_res = XRRGetScreenResources(display, DefaultRootWindow(display));
    if(!screen_res)
        return;

    for(int i = 0; i < screen_res->noutput; ++i) {
        XRROutputInfo *out_info = XRRGetOutputInfo(display, screen_res, screen_res->outputs[i]);
        if(out_info && out_info->crtc && out_info->connection == RR_Connected) {
            XRRCrtcInfo *crt_info = XRRGetCrtcInfo(display, screen_res, out_info->crtc);
            if(crt_info && crt_info->mode) {
                const XRRModeInfo *mode_info = get_mode_info(screen_res, crt_info->mode);
                if(mode_info)
                    callback(out_info, crt_info, mode_info, userdata);
            }
            if(crt_info)
                XRRFreeCrtcInfo(crt_info);
        }
        if(out_info)
            XRRFreeOutputInfo(out_info);
    }    

    XRRFreeScreenResources(screen_res);
}

typedef struct {
    vec2i pos;
    vec2i size;
} gsr_monitor;

typedef struct {
    const char *name;
    int name_len;
    gsr_monitor *monitor;
    bool found_monitor;
} get_monitor_by_name_userdata;

static void get_monitor_by_name_callback(const XRROutputInfo *output_info, const XRRCrtcInfo *crt_info, const XRRModeInfo *mode_info, void *userdata) {
    get_monitor_by_name_userdata *data = (get_monitor_by_name_userdata*)userdata;
    if(!data->found_monitor && data->name_len == output_info->nameLen && memcmp(data->name, output_info->name, data->name_len) == 0) {
        data->monitor->pos = { crt_info->x, crt_info->y };
        data->monitor->size = { (int)crt_info->width, (int)crt_info->height };
        data->found_monitor = true;
    }
}

static bool get_monitor_by_name(Display *display, const char *name, gsr_monitor *monitor) {
    get_monitor_by_name_userdata userdata;
    userdata.name = name;
    userdata.name_len = strlen(name);
    userdata.monitor = monitor;
    userdata.found_monitor = false;
    for_each_active_monitor_output(display, get_monitor_by_name_callback, &userdata);
    return userdata.found_monitor;
}

static void monitor_output_callback_print(const XRROutputInfo *output_info, const XRRCrtcInfo *crt_info, const XRRModeInfo *mode_info, void *userdata) {
    fprintf(stderr, "    \"%.*s\"    (%dx%d+%d+%d)\n", output_info->nameLen, output_info->name, (int)crt_info->width, (int)crt_info->height, crt_info->x, crt_info->y);
}

static char* av_error_to_string(int err) {
    if(av_strerror(err, av_error_buffer, sizeof(av_error_buffer)) < 0)
        strcpy(av_error_buffer, "Unknown error");
    return av_error_buffer;
}

enum class VideoQuality {
    MEDIUM,
    HIGH,
    VERY_HIGH,
    ULTRA
};

enum class VideoCodec {
    H264,
    H265
};

static int x11_error_handler(Display *dpy, XErrorEvent *ev) {
    return 0;
}

static int x11_io_error_handler(Display *dpy) {
    return 0;
}

// |stream| is only required for non-replay mode
static void receive_frames(AVCodecContext *av_codec_context, int stream_index, AVStream *stream, AVFrame *frame,
                           AVFormatContext *av_format_context,
                           double replay_start_time,
                           std::deque<AVPacket> &frame_data_queue,
                           int replay_buffer_size_secs,
                           bool &frames_erased,
						   std::mutex &write_output_mutex) {
    for (;;) {
        // TODO: Use av_packet_alloc instead because sizeof(av_packet) might not be future proof(?)
        AVPacket av_packet;
        memset(&av_packet, 0, sizeof(av_packet));
        av_packet.data = NULL;
        av_packet.size = 0;
        int res = avcodec_receive_packet(av_codec_context, &av_packet);
        if (res == 0) { // we have a packet, send the packet to the muxer
            av_packet.stream_index = stream_index;
            av_packet.pts = av_packet.dts = frame->pts;

			std::lock_guard<std::mutex> lock(write_output_mutex);
            if(replay_buffer_size_secs != -1) {
                double time_now = clock_get_monotonic_seconds();
                double replay_time_elapsed = time_now - replay_start_time;

                AVPacket new_pack;
                av_packet_move_ref(&new_pack, &av_packet);
                frame_data_queue.push_back(std::move(new_pack));
                if(replay_time_elapsed >= replay_buffer_size_secs) {
                    av_packet_unref(&frame_data_queue.front());
                    frame_data_queue.pop_front();
                    frames_erased = true;
                }
            } else {
                av_packet_rescale_ts(&av_packet, av_codec_context->time_base, stream->time_base);
                av_packet.stream_index = stream->index;
                int ret = av_write_frame(av_format_context, &av_packet);
                if(ret < 0) {
                    fprintf(stderr, "Error: Failed to write frame index %d to muxer, reason: %s (%d)\n", av_packet.stream_index, av_error_to_string(ret), ret);
                }
            }
            av_packet_unref(&av_packet);
        } else if (res == AVERROR(EAGAIN)) { // we have no packet
                                             // fprintf(stderr, "No packet!\n");
            av_packet_unref(&av_packet);
            break;
        } else if (res == AVERROR_EOF) { // this is the end of the stream
            fprintf(stderr, "End of stream!\n");
            av_packet_unref(&av_packet);
            break;
        } else {
            fprintf(stderr, "Unexpected error: %d\n", res);
            av_packet_unref(&av_packet);
            break;
        }
    }
}

static AVCodecContext* create_audio_codec_context(AVFormatContext *av_format_context, int fps) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        fprintf(
            stderr,
            "Error: Could not find aac encoder\n");
        exit(1);
    }

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    assert(codec->type == AVMEDIA_TYPE_AUDIO);
	codec_context->codec_id = AV_CODEC_ID_AAC;
    codec_context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    //codec_context->bit_rate = 64000;
    codec_context->sample_rate = 48000;
    codec_context->profile = FF_PROFILE_AAC_LOW;
#if LIBAVCODEC_VERSION_MAJOR < 60
    codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
    codec_context->channels = 2;
#else
    av_channel_layout_default(&codec_context->ch_layout, 2);
#endif

    codec_context->time_base.num = 1;
    codec_context->time_base.den = codec_context->sample_rate;
    codec_context->framerate.num = fps;
    codec_context->framerate.den = 1;

    av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static const AVCodec* find_h264_encoder() {
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if(!codec)
        codec = avcodec_find_encoder_by_name("nvenc_h264");
    return codec;
}

static const AVCodec* find_h265_encoder() {
    const AVCodec *codec = avcodec_find_encoder_by_name("hevc_nvenc");
    if(!codec)
        codec = avcodec_find_encoder_by_name("nvenc_hevc");
    return codec;
}

static AVCodecContext *create_video_codec_context(AVFormatContext *av_format_context, 
                            VideoQuality video_quality,
                            int fps, const AVCodec *codec, bool is_livestream) {

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    //double fps_ratio = (double)fps / 30.0;

    assert(codec->type == AVMEDIA_TYPE_VIDEO);
    codec_context->codec_id = codec->id;
    // Timebase: This is the fundamental unit of time (in seconds) in terms
    // of which frame timestamps are represented. For fixed-fps content,
    // timebase should be 1/framerate and timestamp increments should be
    // identical to 1
    codec_context->time_base.num = 1;
    codec_context->time_base.den = fps;
    codec_context->framerate.num = fps;
    codec_context->framerate.den = 1;
    codec_context->sample_aspect_ratio.num = 0;
    codec_context->sample_aspect_ratio.den = 0;
    // High values reeduce file size but increases time it takes to seek
    if(is_livestream) {
        codec_context->flags |= (AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY);
        codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
        //codec_context->gop_size = std::numeric_limits<int>::max();
        //codec_context->keyint_min = std::numeric_limits<int>::max();
        codec_context->gop_size = fps * 2;
    } else {
        codec_context->gop_size = fps * 2;
    }
    codec_context->max_b_frames = 0;
    codec_context->pix_fmt = AV_PIX_FMT_CUDA;
    codec_context->color_range = AVCOL_RANGE_JPEG;
    if(codec->id == AV_CODEC_ID_HEVC)
        codec_context->codec_tag = MKTAG('h', 'v', 'c', '1');
    switch(video_quality) {
        case VideoQuality::MEDIUM:
            //codec_context->qmin = 35;
            //codec_context->qmax = 35;
            codec_context->bit_rate = 100000;//4500000 + (codec_context->width * codec_context->height)*0.75;
            break;
        case VideoQuality::HIGH:
            //codec_context->qmin = 34;
            //codec_context->qmax = 34;
            codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
            break;
        case VideoQuality::VERY_HIGH:
            //codec_context->qmin = 28;
            //codec_context->qmax = 28;
            codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
            break;
        case VideoQuality::ULTRA:
            //codec_context->qmin = 22;
            //codec_context->qmax = 22;
            codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
            break;
    }
    //codec_context->profile = FF_PROFILE_H264_MAIN;
    if (codec_context->codec_id == AV_CODEC_ID_MPEG1VIDEO)
        codec_context->mb_decision = 2;

    if(codec_context->codec_id == AV_CODEC_ID_H264)
        codec_context->profile = FF_PROFILE_H264_HIGH;

    // stream->time_base = codec_context->time_base;
    // codec_context->ticks_per_frame = 30;
    //av_opt_set(codec_context->priv_data, "tune", "hq", 0);
    // TODO: Do this for better file size? also allows setting qmin, qmax per frame? which can then be used to dynamically set bitrate to reduce quality
    // if live streaming is slow or if the users harddrive is cant handle writing megabytes of data per second.
    #if 0
    char qmin_str[32];
    snprintf(qmin_str, sizeof(qmin_str), "%d", codec_context->qmin);

    char qmax_str[32];
    snprintf(qmax_str, sizeof(qmax_str), "%d", codec_context->qmax);

    av_opt_set(codec_context->priv_data, "cq", qmax_str, 0);
    av_opt_set(codec_context->priv_data, "rc", "vbr", 0);
    av_opt_set(codec_context->priv_data, "qmin", qmin_str, 0);
    av_opt_set(codec_context->priv_data, "qmax", qmax_str, 0);
    codec_context->bit_rate = 0;
    #endif

    //codec_context->rc_max_rate = codec_context->bit_rate;
    //codec_context->rc_min_rate = codec_context->bit_rate;
    //codec_context->rc_buffer_size = codec_context->bit_rate / 10;

    av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static AVFrame* open_audio(AVCodecContext *audio_codec_context) {
    int ret;
    ret = avcodec_open2(audio_codec_context, audio_codec_context->codec, nullptr);
    if(ret < 0) {
        fprintf(stderr, "failed to open codec, reason: %s\n", av_error_to_string(ret));
        exit(1);
    }

    AVFrame *frame = av_frame_alloc();
    if(!frame) {
        fprintf(stderr, "failed to allocate audio frame\n");
        exit(1);
    }

    frame->nb_samples = audio_codec_context->frame_size;
    frame->format = audio_codec_context->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR < 60
    frame->channels = audio_codec_context->channels;
    frame->channel_layout = audio_codec_context->channel_layout;
#else
    av_channel_layout_copy(&frame->ch_layout, &audio_codec_context->ch_layout);
#endif

    ret = av_frame_get_buffer(frame, 0);
    if(ret < 0) {
        fprintf(stderr, "failed to allocate audio data buffers, reason: %s\n", av_error_to_string(ret));
        exit(1);
    }

    return frame;
}

static void open_video(AVCodecContext *codec_context, VideoQuality video_quality, bool very_old_gpu) {
    bool supports_p4 = false;
    bool supports_p7 = false;

    const AVOption *opt = nullptr;
    while((opt = av_opt_next(codec_context->priv_data, opt))) {
        if(opt->type == AV_OPT_TYPE_CONST) {
            if(strcmp(opt->name, "p4") == 0)
                supports_p4 = true;
            else if(strcmp(opt->name, "p7") == 0)
                supports_p7 = true;
        }
    }

    AVDictionary *options = nullptr;
    if(very_old_gpu) {
        switch(video_quality) {
            case VideoQuality::MEDIUM:
                av_dict_set_int(&options, "qp", 37, 0);
                break;
            case VideoQuality::HIGH:
                av_dict_set_int(&options, "qp", 32, 0);
                break;
            case VideoQuality::VERY_HIGH:
                av_dict_set_int(&options, "qp", 27, 0);
                break;
            case VideoQuality::ULTRA:
                av_dict_set_int(&options, "qp", 21, 0);
                break;
        }
    } else {
        switch(video_quality) {
            case VideoQuality::MEDIUM:
                av_dict_set_int(&options, "qp", 40, 0);
                break;
            case VideoQuality::HIGH:
                av_dict_set_int(&options, "qp", 35, 0);
                break;
            case VideoQuality::VERY_HIGH:
                av_dict_set_int(&options, "qp", 30, 0);
                break;
            case VideoQuality::ULTRA:
                av_dict_set_int(&options, "qp", 24, 0);
                break;
        }
    }

    if(!supports_p4 && !supports_p7)
        fprintf(stderr, "Info: your ffmpeg version is outdated. It's recommended that you use the flatpak version of gpu-screen-recorder version instead, which you can find at https://flathub.org/apps/details/com.dec05eba.gpu_screen_recorder\n");

    //if(is_livestream) {
    //    av_dict_set_int(&options, "zerolatency", 1, 0);
    //    //av_dict_set(&options, "preset", "llhq", 0);
    //}

    // Fuck nvidia and ffmpeg, I want to use a good preset for the gpu but all gpus prefer different
    // presets. Nvidia and ffmpeg used to support "hq" preset that chose the best preset for the gpu
    // with pretty good performance but you now have to choose p1-p7, which are gpu agnostic and on
    // older gpus p5-p7 slow the gpu down to a crawl...
    // "hq" is now just an alias for p7 in ffmpeg :(
    if(very_old_gpu)
        av_dict_set(&options, "preset", supports_p4 ? "p4" : "medium", 0);
    else
        av_dict_set(&options, "preset", supports_p7 ? "p7" : "slow", 0);

    av_dict_set(&options, "tune", "hq", 0);
    av_dict_set(&options, "rc", "constqp", 0);

    if(codec_context->codec_id == AV_CODEC_ID_H264)
        av_dict_set(&options, "profile", "high", 0);

    int ret = avcodec_open2(codec_context, codec_context->codec, &options);
    if (ret < 0) {
        fprintf(stderr, "Error: Could not open video codec: %s\n", av_error_to_string(ret));
        exit(1);
    }
}

static void usage() {
    fprintf(stderr, "usage: gpu-screen-recorder -w <window_id> -c <container_format> -f <fps> [-a <audio_input>...] [-q <quality>] [-r <replay_buffer_size_sec>] [-o <output_file>]\n");
    fprintf(stderr, "OPTIONS:\n");
    fprintf(stderr, "  -w    Window to record or a display, \"screen\" or \"screen-direct\". The display is the display name in xrandr and if \"screen\" or \"screen-direct\" is selected then all displays are recorded and they are recorded in h265 (aka hevc)."
        "\"screen-direct\" skips one texture copy for fullscreen applications so it may lead to better performance and it works with VRR monitors when recording fullscreen application but may break some applications, such as mpv in fullscreen mode. Recording a display requires a gpu with NvFBC support.\n");
    fprintf(stderr, "  -c    Container format for output file, for example mp4, or flv.\n");
    fprintf(stderr, "  -f    Framerate to record at.\n");
    fprintf(stderr, "  -a    Audio device to record from (pulse audio device). Can be specified multiple times. Each time this is specified a new audio track is added for the specified audio device. A name can be given to the audio input device by prefixing the audio input with <name>/, for example \"dummy/alsa_output.pci-0000_00_1b.0.analog-stereo.monitor\". Optional, no audio track is added by default.\n");
    fprintf(stderr, "  -q    Video quality. Should be either 'medium', 'high', 'very_high' or 'ultra'. 'high' is the recommended option when live streaming or when you have a slower harddrive. Optional, set to 'very_high' be default.\n");
    fprintf(stderr, "  -r    Replay buffer size in seconds. If this is set, then only the last seconds as set by this option will be stored"
        " and the video will only be saved when the gpu-screen-recorder is closed. This feature is similar to Nvidia's instant replay feature."
        " This option has be between 5 and 1200. Note that the replay buffer size will not always be precise, because of keyframes. Optional, disabled by default.\n");
    fprintf(stderr, "  -k    Codec to use. Should be either 'auto', 'h264' or 'h265'. Defaults to 'auto' which defaults to 'h265' unless recording at a higher resolution than 60. Forcefully set to 'h264' if -c is 'flv'.\n");
    fprintf(stderr, "  -o    The output file path. If omitted then the encoded data is sent to stdout. Required in replay mode (when using -r). In replay mode this has to be an existing directory instead of a file.\n");
    fprintf(stderr, "NOTES:\n");
    fprintf(stderr, "  Send signal SIGINT (Ctrl+C) to gpu-screen-recorder to stop and save the recording (when not using replay mode).\n");
    fprintf(stderr, "  Send signal SIGUSR1 (killall -SIGUSR1 gpu-screen-recorder) to gpu-screen-recorder to save a replay.\n");
    exit(1);
}

static sig_atomic_t running = 1;
static sig_atomic_t save_replay = 0;

static void int_handler(int) {
    running = 0;
}

static void save_replay_handler(int) {
    save_replay = 1;
}

struct Arg {
    std::vector<const char*> values;
    bool optional = false;
    bool list = false;

    const char* value() const {
        if(values.empty())
            return nullptr;
        return values.front();
    }
};

static bool is_hex_num(char c) {
    return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
}

static bool contains_non_hex_number(const char *str) {
    size_t len = strlen(str);
    if(len >= 2 && memcmp(str, "0x", 2) == 0) {
        str += 2;
        len -= 2;
    }

    for(size_t i = 0; i < len; ++i) {
        char c = str[i];
        if(c == '\0')
            return false;
        if(!is_hex_num(c))
            return true;
    }
    return false;
}

static std::string get_date_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%Y-%m-%d_%H-%M-%S", t);
    return str; 
}

static AVStream* create_stream(AVFormatContext *av_format_context, AVCodecContext *codec_context) {
    AVStream *stream = avformat_new_stream(av_format_context, nullptr);
    if (!stream) {
        fprintf(stderr, "Error: Could not allocate stream\n");
        exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    stream->time_base = codec_context->time_base;
    stream->avg_frame_rate = codec_context->framerate;
    return stream;
}

struct AudioTrack {
    AVCodecContext *codec_context = nullptr;
    AVFrame *frame = nullptr;
    AVStream *stream = nullptr;

    SoundDevice sound_device;
    std::thread thread; // TODO: Instead of having a thread for each track, have one thread for all threads and read the data with non-blocking read

    int stream_index = 0;
    AudioInput audio_input;
};

static std::future<void> save_replay_thread;
static std::vector<AVPacket> save_replay_packets;
static std::string save_replay_output_filepath;

static void save_replay_async(AVCodecContext *video_codec_context, int video_stream_index, std::vector<AudioTrack> &audio_tracks, const std::deque<AVPacket> &frame_data_queue, bool frames_erased, std::string output_dir, std::string container_format, std::mutex &write_output_mutex) {
    if(save_replay_thread.valid())
        return;
    
    size_t start_index = (size_t)-1;
    int64_t video_pts_offset = 0;
    int64_t audio_pts_offset = 0;

    {
        std::lock_guard<std::mutex> lock(write_output_mutex);
        start_index = (size_t)-1;
        for(size_t i = 0; i < frame_data_queue.size(); ++i) {
            const AVPacket &av_packet = frame_data_queue[i];
            if((av_packet.flags & AV_PKT_FLAG_KEY) && av_packet.stream_index == video_stream_index) {
                start_index = i;
                break;
            }
        }

        if(start_index == (size_t)-1)
            return;

        if(frames_erased) {
            video_pts_offset = frame_data_queue[start_index].pts;
            
            // Find the next audio packet to use as audio pts offset
            for(size_t i = start_index; i < frame_data_queue.size(); ++i) {
                const AVPacket &av_packet = frame_data_queue[i];
                if(av_packet.stream_index != video_stream_index) {
                    audio_pts_offset = av_packet.pts;
                    break;
                }
            }
        } else {
            start_index = 0;
        }

        save_replay_packets.resize(frame_data_queue.size());
        for(size_t i = 0; i < frame_data_queue.size(); ++i) {
            av_packet_ref(&save_replay_packets[i], &frame_data_queue[i]);
        }
    }

    save_replay_output_filepath = output_dir + "/Replay_" + get_date_str() + "." + container_format;
    save_replay_thread = std::async(std::launch::async, [video_stream_index, container_format, start_index, video_pts_offset, audio_pts_offset, video_codec_context, &audio_tracks]() mutable {
        AVFormatContext *av_format_context;
        // The output format is automatically guessed from the file extension
        avformat_alloc_output_context2(&av_format_context, nullptr, container_format.c_str(), nullptr);

        av_format_context->flags |= AVFMT_FLAG_GENPTS;
        av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVStream *video_stream = create_stream(av_format_context, video_codec_context);
        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

        std::unordered_map<int, AudioTrack*> stream_index_to_audio_track_map;
        for(AudioTrack &audio_track : audio_tracks) {
            stream_index_to_audio_track_map[audio_track.stream_index] = &audio_track;
            AVStream *audio_stream = create_stream(av_format_context, audio_track.codec_context);
            avcodec_parameters_from_context(audio_stream->codecpar, audio_track.codec_context);
            audio_track.stream = audio_stream;
        }

        int ret = avio_open(&av_format_context->pb, save_replay_output_filepath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s. Make sure %s is an existing directory with write access\n", save_replay_output_filepath.c_str(), av_error_to_string(ret), save_replay_output_filepath.c_str());
            return;
        }

        ret = avformat_write_header(av_format_context, nullptr);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when writing header to output file: %s\n", av_error_to_string(ret));
            return;
        }

        for(size_t i = start_index; i < save_replay_packets.size(); ++i) {
            AVPacket &av_packet = save_replay_packets[i];

            AVStream *stream = video_stream;
            AVCodecContext *codec_context = video_codec_context;

            if(av_packet.stream_index == video_stream_index) {
                av_packet.pts -= video_pts_offset;
                av_packet.dts -= video_pts_offset;
            } else {
                AudioTrack *audio_track = stream_index_to_audio_track_map[av_packet.stream_index];
                stream = audio_track->stream;
                codec_context = audio_track->codec_context;

                av_packet.pts -= audio_pts_offset;
                av_packet.dts -= audio_pts_offset;
            }

            av_packet.stream_index = stream->index;
            av_packet_rescale_ts(&av_packet, codec_context->time_base, stream->time_base);

            int ret = av_write_frame(av_format_context, &av_packet);
            if(ret < 0)
                fprintf(stderr, "Error: Failed to write frame index %d to muxer, reason: %s (%d)\n", stream->index, av_error_to_string(ret), ret);
        }

        if (av_write_trailer(av_format_context) != 0)
            fprintf(stderr, "Failed to write trailer\n");

        avio_close(av_format_context->pb);
        avformat_free_context(av_format_context);

        for(AudioTrack &audio_track : audio_tracks) {
            audio_track.stream = nullptr;
        }
    });
}

static AudioInput parse_audio_input_arg(const char *str) {
    AudioInput audio_input;
    audio_input.name = str;
    const size_t index = audio_input.name.find('/');
    if(index != std::string::npos) {
        audio_input.description = audio_input.name.substr(0, index);
        audio_input.name.erase(audio_input.name.begin(), audio_input.name.begin() + index + 1);
    }
    return audio_input;
}

// TODO: Does this match all livestreaming cases?
static bool is_livestream_path(const char *str) {
    const int len = strlen(str);
    if((len >= 7 && memcmp(str, "http://", 7) == 0) || (len >= 8 && memcmp(str, "https://", 8) == 0))
        return true;
    else if((len >= 7 && memcmp(str, "rtmp://", 7) == 0) || (len >= 8 && memcmp(str, "rtmps://", 8) == 0))
        return true;
    else
        return false;
}

int main(int argc, char **argv) {
    signal(SIGINT, int_handler);
    signal(SIGUSR1, save_replay_handler);

    std::map<std::string, Arg> args = {
        { "-w", Arg { {}, false, false } },
        //{ "-s", Arg { nullptr, true } },
        { "-c", Arg { {}, false, false } },
        { "-f", Arg { {}, false, false } },
        //{ "-s", Arg { {}, true, false } },
        { "-a", Arg { {}, true, true } },
        { "-q", Arg { {}, true, false } },
        { "-o", Arg { {}, true, false } },
        { "-r", Arg { {}, true, false } },
        { "-k", Arg { {}, true, false } }
    };

    for(int i = 1; i < argc - 1; i += 2) {
        auto it = args.find(argv[i]);
        if(it == args.end()) {
            fprintf(stderr, "Invalid argument '%s'\n", argv[i]);
            usage();
        }

        if(!it->second.values.empty() && !it->second.list) {
            fprintf(stderr, "Expected argument '%s' to only be specified once\n", argv[i]);
            usage();
        }

        it->second.values.push_back(argv[i + 1]);
    }

    for(auto &it : args) {
        if(!it.second.optional && !it.second.value()) {
            fprintf(stderr, "Missing argument '%s'\n", it.first.c_str());
            usage();
        }
    }

    VideoCodec video_codec;
    const char *codec_to_use = args["-k"].value();
    if(!codec_to_use)
        codec_to_use = "auto";

    if(strcmp(codec_to_use, "h264") == 0) {
        video_codec = VideoCodec::H264;
    } else if(strcmp(codec_to_use, "h265") == 0) {
        video_codec = VideoCodec::H265;
    } else if(strcmp(codec_to_use, "auto") != 0) {
        fprintf(stderr, "Error: -k should either be either 'auto', 'h264' or 'h265', got: '%s'\n", codec_to_use);
        usage();
    }

    const Arg &audio_input_arg = args["-a"];
    const std::vector<AudioInput> audio_inputs = get_pulseaudio_inputs();
    std::vector<AudioInput> requested_audio_inputs;

    // Manually check if the audio inputs we give exist. This is only needed for pipewire, not pulseaudio.
    // Pipewire instead DEFAULTS TO THE DEFAULT AUDIO INPUT. THAT'S RETARDED.
    // OH, YOU MISSPELLED THE AUDIO INPUT? FUCK YOU
    for(const char *audio_input : audio_input_arg.values) {
        requested_audio_inputs.push_back(parse_audio_input_arg(audio_input));
        AudioInput &request_audio_input = requested_audio_inputs.back();

        bool match = false;
        for(const auto &existing_audio_input : audio_inputs) {
            if(strcmp(request_audio_input.name.c_str(), existing_audio_input.name.c_str()) == 0) {
                if(request_audio_input.description.empty())
                    request_audio_input.description = "gsr-" + existing_audio_input.description;

                match = true;
                break;
            }
        }

        if(!match) {
            fprintf(stderr, "Error: Audio input device '%s' is not a valid audio device, expected one of:\n", request_audio_input.name.c_str());
            for(const auto &existing_audio_input : audio_inputs) {
                fprintf(stderr, "    %s\n", existing_audio_input.name.c_str());
            }
            exit(2);
        }
    }

    const char *container_format = args["-c"].value();
    int fps = atoi(args["-f"].value());
    if(fps == 0) {
        fprintf(stderr, "Invalid fps argument: %s\n", args["-f"].value());
        return 1;
    }
    if(fps < 1)
        fps = 1;

    const char *quality_str = args["-q"].value();
    if(!quality_str)
        quality_str = "very_high";

    VideoQuality quality;
    if(strcmp(quality_str, "medium") == 0) {
        quality = VideoQuality::MEDIUM;
    } else if(strcmp(quality_str, "high") == 0) {
        quality = VideoQuality::HIGH;
    } else if(strcmp(quality_str, "very_high") == 0) {
        quality = VideoQuality::VERY_HIGH;
    } else if(strcmp(quality_str, "ultra") == 0) {
        quality = VideoQuality::ULTRA;
    } else {
        fprintf(stderr, "Error: -q should either be either 'medium', 'high', 'very_high' or 'ultra', got: '%s'\n", quality_str);
        usage();
    }

    int replay_buffer_size_secs = -1;
    const char *replay_buffer_size_secs_str = args["-r"].value();
    if(replay_buffer_size_secs_str) {
        replay_buffer_size_secs = atoi(replay_buffer_size_secs_str);
        if(replay_buffer_size_secs < 5 || replay_buffer_size_secs > 1200) {
            fprintf(stderr, "Error: option -r has to be between 5 and 1200, was: %s\n", replay_buffer_size_secs_str);
            return 1;
        }
        replay_buffer_size_secs += 5; // Add a few seconds to account of lost packets because of non-keyframe packets skipped
    }

    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "Error: Failed to open display\n");
        return 1;
    }

    XSetErrorHandler(x11_error_handler);
    XSetIOErrorHandler(x11_io_error_handler);

    gsr_gl gl;
    if(!gsr_gl_load(&gl, dpy)) {
        fprintf(stderr, "Error: failed to load opengl\n");
        return 1;
    }

    bool very_old_gpu = false;
    const unsigned char *gl_renderer = gl.glGetString(GL_RENDERER);
    if(gl_renderer) {
        int gpu_num = 1000;
        sscanf((const char*)gl_renderer, "%*s %*s %*s %d", &gpu_num);
        if(gpu_num < 900) {
            fprintf(stderr, "Info: your gpu appears to be very old (older than maxwell architecture). Switching to lower preset\n");
            very_old_gpu = true;
        }
    }

    gsr_gl_unload(&gl);

    const char *window_str = args["-w"].value();

    gsr_capture *capture = nullptr;
    if(contains_non_hex_number(window_str)) {
        const char *capture_target = window_str;
        bool direct_capture = strcmp(window_str, "screen-direct") == 0;
        if(direct_capture) {
            capture_target = "screen";
            // TODO: Temporary disable direct capture because push model causes stuttering when it's direct capturing. This might be a nvfbc bug. This does not happen when using a compositor.
            direct_capture = false;
            fprintf(stderr, "Warning: screen-direct has temporary been disabled as it causes stuttering. This is likely a NvFBC bug. Falling back to \"screen\".\n");
        }

        gsr_capture_nvfbc_params nvfbc_params;
        nvfbc_params.dpy = dpy;
        nvfbc_params.display_to_capture = capture_target;
        nvfbc_params.fps = fps;
        nvfbc_params.pos = { 0, 0 };
        nvfbc_params.size = { 0, 0 };
        nvfbc_params.direct_capture = direct_capture;
        capture = gsr_capture_nvfbc_create(&nvfbc_params);
        if(!capture)
            return 1;

        if(strcmp(capture_target, "screen") != 0) {
            gsr_monitor gmon;
            if(!get_monitor_by_name(dpy, capture_target, &gmon)) {
                fprintf(stderr, "gsr error: display \"%s\" not found, expected one of:\n", capture_target);
                fprintf(stderr, "    \"screen\"    (%dx%d+%d+%d)\n", XWidthOfScreen(DefaultScreenOfDisplay(dpy)), XHeightOfScreen(DefaultScreenOfDisplay(dpy)), 0, 0);
                fprintf(stderr, "    \"screen-direct\"    (%dx%d+%d+%d)\n", XWidthOfScreen(DefaultScreenOfDisplay(dpy)), XHeightOfScreen(DefaultScreenOfDisplay(dpy)), 0, 0);
                for_each_active_monitor_output(dpy, monitor_output_callback_print, NULL);
                return 1;
            }
        }
    } else {
        errno = 0;
        Window src_window_id = strtol(window_str, nullptr, 0);
        if(src_window_id == None || errno == EINVAL) {
            fprintf(stderr, "Invalid window number %s\n", window_str);
            usage();
        }

        gsr_capture_xcomposite_params xcomposite_params;
        xcomposite_params.window = src_window_id;
        capture = gsr_capture_xcomposite_create(&xcomposite_params);
        if(!capture)
            return 1;
    }

    const char *filename = args["-o"].value();
    if(filename) {
        if(replay_buffer_size_secs != -1) {
            struct stat buf;
            if(stat(filename, &buf) == -1 || !S_ISDIR(buf.st_mode)) {
                fprintf(stderr, "%s does not exist or is not a directory\n", filename);
                usage();
            }
        }
    } else {
        if(replay_buffer_size_secs == -1) {
            filename = "/dev/stdout";
        } else {
            fprintf(stderr, "Option -o is required when using option -r\n");
            usage();
        }
    }

    const double target_fps = 1.0 / (double)fps;

    if(strcmp(codec_to_use, "auto") == 0) {
        const AVCodec *h265_codec = find_h265_encoder();

        // h265 generally allows recording at a higher resolution than h264 on nvidia cards. On a gtx 1080 4k is the max resolution for h264 but for h265 it's 8k.
        // Another important info is that when recording at a higher fps than.. 60? h265 has very bad performance. For example when recording at 144 fps the fps drops to 1
        // while with h264 the fps doesn't drop.
        if(!h265_codec) {
            fprintf(stderr, "Info: using h264 encoder because a codec was not specified and your gpu does not support h265\n");
        } else if(fps > 60) {
            fprintf(stderr, "Info: using h264 encoder because a codec was not specified and fps is more than 60\n");
            codec_to_use = "h264";
            video_codec = VideoCodec::H264;
        } else {
            fprintf(stderr, "Info: using h265 encoder because a codec was not specified\n");
            codec_to_use = "h265";
            video_codec = VideoCodec::H265;
        }
    }

    //bool use_hevc = strcmp(window_str, "screen") == 0 || strcmp(window_str, "screen-direct") == 0;
    if(video_codec != VideoCodec::H264 && strcmp(container_format, "flv") == 0) {
        video_codec = VideoCodec::H264;
        fprintf(stderr, "Warning: h265 is not compatible with flv, falling back to h264 instead.\n");
    }

    const AVCodec *video_codec_f = nullptr;
    switch(video_codec) {
        case VideoCodec::H264:
            video_codec_f = find_h264_encoder();
            break;
        case VideoCodec::H265:
            video_codec_f = find_h265_encoder();
            break;
    }

    if(!video_codec_f) {
        fprintf(stderr, "Error: your gpu does not support '%s' video codec\n", video_codec == VideoCodec::H264 ? "h264" : "h265");
        exit(2);
    }

    // Video start
    AVFormatContext *av_format_context;
    // The output format is automatically guessed by the file extension
    avformat_alloc_output_context2(&av_format_context, nullptr, container_format,
                                   nullptr);
    if (!av_format_context) {
        fprintf(
            stderr,
            "Error: Failed to deduce output format from file extension\n");
        return 1;
    }

    av_format_context->flags |= AVFMT_FLAG_GENPTS;
    const AVOutputFormat *output_format = av_format_context->oformat;

    const bool is_livestream = is_livestream_path(filename);
    // (Some?) livestreaming services require at least one audio track to work.
    // If not audio is provided then create one silent audio track.
    if(is_livestream && requested_audio_inputs.empty()) {
        fprintf(stderr, "Info: live streaming but no audio track was added. Adding a silent audio track\n");
        requested_audio_inputs.push_back({ "", "gsr-silent" });
    }

    AVStream *video_stream = nullptr;
    std::vector<AudioTrack> audio_tracks;

    AVCodecContext *video_codec_context = create_video_codec_context(av_format_context, quality, fps, video_codec_f, is_livestream);
    if(replay_buffer_size_secs == -1)
        video_stream = create_stream(av_format_context, video_codec_context);

    if(gsr_capture_start(capture, video_codec_context) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_start failed\n");
        return 1;
    }

    open_video(video_codec_context, quality, very_old_gpu);
    if(video_stream)
        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

    int audio_stream_index = VIDEO_STREAM_INDEX + 1;
    for(const AudioInput &audio_input : requested_audio_inputs) {
        AVCodecContext *audio_codec_context = create_audio_codec_context(av_format_context, fps);

        AVStream *audio_stream = nullptr;
        if(replay_buffer_size_secs == -1)
            audio_stream = create_stream(av_format_context, audio_codec_context);

        AVFrame *audio_frame = open_audio(audio_codec_context);
        if(audio_stream)
            avcodec_parameters_from_context(audio_stream->codecpar, audio_codec_context);

        audio_tracks.push_back({ audio_codec_context, audio_frame, audio_stream, {}, {}, audio_stream_index, audio_input });
        ++audio_stream_index;
    }

    //av_dump_format(av_format_context, 0, filename, 1);

    if (replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s\n", filename, av_error_to_string(ret));
            return 1;
        }
    }

    if(replay_buffer_size_secs == -1) {
        int ret = avformat_write_header(av_format_context, nullptr);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when writing header to output file: %s\n", av_error_to_string(ret));
            return 1;
        }
    }

    const double start_time_pts = clock_get_monotonic_seconds();

    double start_time = clock_get_monotonic_seconds();
    double frame_timer_start = start_time;
    int fps_counter = 0;

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Error: Failed to allocate frame\n");
        exit(1);
    }
    frame->format = video_codec_context->pix_fmt;
    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    if(video_codec_context->hw_frames_ctx) {
        // TODO: Unref at the end?
        frame->hw_frames_ctx = av_buffer_ref(video_codec_context->hw_frames_ctx);
        frame->buf[0] = av_buffer_pool_get(((AVHWFramesContext*)video_codec_context->hw_frames_ctx->data)->pool);
        frame->extended_data = frame->data;
    }

    std::mutex write_output_mutex;

    const double record_start_time = clock_get_monotonic_seconds();
    std::deque<AVPacket> frame_data_queue;
    bool frames_erased = false;

    const size_t audio_buffer_size = 1024 * 2 * 2; // 2 bytes/sample, 2 channels
    uint8_t *empty_audio = (uint8_t*)malloc(audio_buffer_size);
    if(!empty_audio) {
        fprintf(stderr, "Error: failed to create empty audio\n");
        exit(1);
    }
    memset(empty_audio, 0, audio_buffer_size);

    for(AudioTrack &audio_track : audio_tracks) {
        audio_track.thread = std::thread([record_start_time, replay_buffer_size_secs, &frame_data_queue, &frames_erased, &audio_track, empty_audio](AVFormatContext *av_format_context, std::mutex *write_output_mutex) mutable {
            #if LIBAVCODEC_VERSION_MAJOR < 60
            const int num_channels = audio_track.codec_context->channels;
            #else
            const int num_channels = audio_track.codec_context->ch_layout.nb_channels;
            #endif

            if(audio_track.audio_input.name.empty()) {
                audio_track.sound_device.handle = NULL;
                audio_track.sound_device.frames = 0;
            } else {
                if(sound_device_get_by_name(&audio_track.sound_device, audio_track.audio_input.name.c_str(), audio_track.audio_input.description.c_str(), num_channels, audio_track.codec_context->frame_size) != 0) {
                    fprintf(stderr, "failed to get 'pulse' sound device\n");
                    exit(1);
                }
            }

            SwrContext *swr = swr_alloc();
            if(!swr) {
                fprintf(stderr, "Failed to create SwrContext\n");
                exit(1);
            }
            av_opt_set_int(swr, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
            av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
            av_opt_set_int(swr, "in_sample_rate", audio_track.codec_context->sample_rate, 0);
            av_opt_set_int(swr, "out_sample_rate", audio_track.codec_context->sample_rate, 0);
            av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
            av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
            swr_init(swr);

            int64_t pts = 0;
            const double target_audio_hz = 1.0 / (double)audio_track.codec_context->sample_rate;
            double received_audio_time = clock_get_monotonic_seconds();
            const int64_t timeout_ms = std::round((1000.0 / (double)audio_track.codec_context->sample_rate) * 1000.0);

            while(running) {
                void *sound_buffer;
                int sound_buffer_size = -1;
                if(audio_track.sound_device.handle)
                    sound_buffer_size = sound_device_read_next_chunk(&audio_track.sound_device, &sound_buffer);
                const bool got_audio_data = sound_buffer_size >= 0;

                const double this_audio_frame_time = clock_get_monotonic_seconds();
                if(got_audio_data)
                    received_audio_time = this_audio_frame_time;

                int ret = av_frame_make_writable(audio_track.frame);
                if (ret < 0) {
                    fprintf(stderr, "Failed to make audio frame writable\n");
                    break;
                }

                const int64_t num_missing_frames = std::round((this_audio_frame_time - received_audio_time) / target_audio_hz / (int64_t)audio_track.frame->nb_samples);
                // Jesus is there a better way to do this? I JUST WANT TO KEEP VIDEO AND AUDIO SYNCED HOLY FUCK I WANT TO KILL MYSELF NOW.
                // THIS PIECE OF SHIT WANTS EMPTY FRAMES OTHERWISE VIDEO PLAYS TOO FAST TO KEEP UP WITH AUDIO OR THE AUDIO PLAYS TOO EARLY.
                // BUT WE CANT USE DELAYS TO GIVE DUMMY DATA BECAUSE PULSEAUDIO MIGHT GIVE AUDIO A BIG DELAYED!!!
                if(num_missing_frames >= 5 || (num_missing_frames > 0 && got_audio_data)) {
                    // TODO:
                    //audio_track.frame->data[0] = empty_audio;
                    received_audio_time = this_audio_frame_time;
                    swr_convert(swr, &audio_track.frame->data[0], audio_track.frame->nb_samples, (const uint8_t**)&empty_audio, audio_track.sound_device.frames);
                    // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
                    for(int i = 0; i < num_missing_frames; ++i) {
                        audio_track.frame->pts = pts;
                        pts += audio_track.frame->nb_samples;
                        ret = avcodec_send_frame(audio_track.codec_context, audio_track.frame);
                        if(ret >= 0){
                            receive_frames(audio_track.codec_context, audio_track.stream_index, audio_track.stream, audio_track.frame, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, *write_output_mutex);
                        } else {
                            fprintf(stderr, "Failed to encode audio!\n");
                        }
                    }
                }

                if(!audio_track.sound_device.handle) {
                    // TODO:
                    //audio_track.frame->data[0] = empty_audio;
                    received_audio_time = this_audio_frame_time;
                    swr_convert(swr, &audio_track.frame->data[0], audio_track.frame->nb_samples, (const uint8_t**)&empty_audio, audio_track.codec_context->frame_size);
                    audio_track.frame->pts = pts;
                    pts += audio_track.frame->nb_samples;
                    ret = avcodec_send_frame(audio_track.codec_context, audio_track.frame);
                    if(ret >= 0){
                        receive_frames(audio_track.codec_context, audio_track.stream_index, audio_track.stream, audio_track.frame, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, *write_output_mutex);
                    } else {
                        fprintf(stderr, "Failed to encode audio!\n");
                    }

                    usleep(timeout_ms * 1000);
                }

                if(got_audio_data) {
                    // TODO: Instead of converting audio, get float audio from alsa. Or does alsa do conversion internally to get this format?
                    swr_convert(swr, &audio_track.frame->data[0], audio_track.frame->nb_samples, (const uint8_t**)&sound_buffer, audio_track.sound_device.frames);

                    audio_track.frame->pts = pts;
                    pts += audio_track.frame->nb_samples;

                    ret = avcodec_send_frame(audio_track.codec_context, audio_track.frame);
                    if(ret >= 0){
                        receive_frames(audio_track.codec_context, audio_track.stream_index, audio_track.stream, audio_track.frame, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, *write_output_mutex);
                    } else {
                        fprintf(stderr, "Failed to encode audio!\n");
                    }
                }
            }

            sound_device_close(&audio_track.sound_device);
            swr_free(&swr);
        }, av_format_context, &write_output_mutex);
    }

    // Set update_fps to 24 to test if duplicate/delayed frames cause video/audio desync or too fast/slow video.
    const double update_fps = fps + 190;
    int64_t video_pts_counter = 0;
    bool should_stop_error = false;

    while (running) {
        double frame_start = clock_get_monotonic_seconds();

        gsr_capture_tick(capture, video_codec_context, &frame);
        should_stop_error = false;
        if(gsr_capture_should_stop(capture, &should_stop_error)) {
            running = 0;
            break;
        }
        ++fps_counter;

        double time_now = clock_get_monotonic_seconds();
        double frame_timer_elapsed = time_now - frame_timer_start;
        double elapsed = time_now - start_time;
        if (elapsed >= 1.0) {
            fprintf(stderr, "update fps: %d\n", fps_counter);
            start_time = time_now;
            fps_counter = 0;
        }

        double frame_time_overflow = frame_timer_elapsed - target_fps;
        if (frame_time_overflow >= 0.0) {
            frame_timer_start = time_now - frame_time_overflow;
            gsr_capture_capture(capture, frame);

            const double this_video_frame_time = clock_get_monotonic_seconds();
            const int64_t expected_frames = std::round((this_video_frame_time - start_time_pts) / target_fps);

            const int num_frames = std::max(0L, expected_frames - video_pts_counter);
            // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
            for(int i = 0; i < num_frames; ++i) {
                frame->pts = video_pts_counter + i;
                if (avcodec_send_frame(video_codec_context, frame) >= 0) {
                    receive_frames(video_codec_context, VIDEO_STREAM_INDEX, video_stream, frame, av_format_context,
                                record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, write_output_mutex);
                } else {
                    fprintf(stderr, "Error: avcodec_send_frame failed\n");
                }
            }
            video_pts_counter += num_frames;
        }

        if(save_replay_thread.valid() && save_replay_thread.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            save_replay_thread.get();
            puts(save_replay_output_filepath.c_str());
            for(size_t i = 0; i < save_replay_packets.size(); ++i) {
                av_packet_unref(&save_replay_packets[i]);
            }
            save_replay_packets.clear();
        }

        if(save_replay == 1 && !save_replay_thread.valid() && replay_buffer_size_secs != -1) {
            save_replay = 0;
            save_replay_async(video_codec_context, VIDEO_STREAM_INDEX, audio_tracks, frame_data_queue, frames_erased, filename, container_format, write_output_mutex);
        }

        // av_frame_free(&frame);
        double frame_end = clock_get_monotonic_seconds();
        double frame_sleep_fps = 1.0 / update_fps;
        double sleep_time = frame_sleep_fps - (frame_end - frame_start);
        if(sleep_time > 0.0)
            usleep(sleep_time * 1000.0 * 1000.0);
    }

	running = 0;

    if(save_replay_thread.valid())
        save_replay_thread.get();

    for(AudioTrack &audio_track : audio_tracks) {
        audio_track.thread.join();
    }

    if (replay_buffer_size_secs == -1 && av_write_trailer(av_format_context) != 0) {
        fprintf(stderr, "Failed to write trailer\n");
    }

    if(replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE))
        avio_close(av_format_context->pb);

    gsr_capture_destroy(capture, video_codec_context);

    if(dpy)
        XCloseDisplay(dpy);

    free(empty_audio);
    return should_stop_error ? 3 : 0;
}
