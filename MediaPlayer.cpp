#include <thread>

using namespace std;

#define ABS_DIFF(a, b) ( (a) < (b) ? (b - a) : (a - b) )
#define SIGN(a)	   (((a) < 0) ? -1.0 : 1.0)
#include "Log.h"
#include "SystemToolkit.h"
#include "BaseToolkit.h"
#include "GstToolkit.h"
#include "MediaPlayer.h"

std::list<MediaPlayer*> MediaPlayer::registered_;

template<typename T>
static int calculate_audio_db(const T* data, int channels, int channel_index, size_t length, const float max_level) 
{
    static const float kMaxSquaredLevel = max_level * max_level;
    constexpr float kMinLevel = -96.f;
    float sum_square_ = 0;
    size_t sample_count_ = 0;
    for (size_t i = 0; i < length; i += channels) 
    {
        T audio_data = data[i + channel_index];
        sum_square_ += audio_data * audio_data;
    }
    sample_count_ += length / channels;
    float rms = sum_square_ / (sample_count_ * kMaxSquaredLevel);
    rms = 10 * log10(rms);
    if (rms < kMinLevel)
        rms = kMinLevel;
    rms = -kMinLevel + rms;
    return static_cast<int>(rms + 0.5);
}

MediaPlayer::MediaPlayer()
{
    // create unique id
    id_ = BaseToolkit::uniqueId();

    uri_ = "undefined";
    pipeline_ = nullptr;
    opened_ = false;
    enabled_ = true;
    desired_state_ = GST_STATE_PAUSED;

    failed_ = false;
    seeking_ = false;
    rewind_on_disable_ = false;
    force_software_decoding_ = false;
    decoder_name_ = "";
    rate_ = 1.0;
    position_ = GST_CLOCK_TIME_NONE;
    loop_ = LoopMode::LOOP_REWIND;

    // start index in frame_ stack
    vwrite_index_ = 0;
    vlast_index_ = 0;

    awrite_index_ = 0;
    alast_index_ = 0;

    // audio level
    audio_channel_level.clear();
}

MediaPlayer::~MediaPlayer()
{
    close();
}

ImGui::ImMat MediaPlayer::videoMat() const
{
    return VMat;
}

guint MediaPlayer::audio_level(guint channel) const
{
    if (channel < audio_channel_level.size())
    {
        return audio_channel_level[channel];
    }
    return 0;
}

ImGui::ImMat MediaPlayer::audioMat() const
{
    return AMat;
}

MediaInfo MediaPlayer::UriDiscoverer(const std::string &uri)
{
#ifdef MEDIA_PLAYER_DEBUG
    Log::Info("Checking file '%s'", uri.c_str());
#endif

#ifdef LIMIT_DISCOVERER
    // Limiting the number of discoverer thread to TWO in parallel
    // Otherwise, a large number of discoverers are executed (when loading a file)
    // leading to a peak of memory and CPU usage : this causes slow down of FPS
    // and a hungry consumption of RAM.
    static std::mutex mtx_primary;
    static std::mutex mtx_secondary;
    bool use_primary = true;
    if ( !mtx_primary.try_lock() ) { // non-blocking
        use_primary = false;
        mtx_secondary.lock(); // blocking
    }
#endif
    MediaInfo stream_info;
    GError *err = NULL;
    GstDiscoverer *discoverer = gst_discoverer_new (5 * GST_SECOND, &err);

    /* Instantiate the Discoverer */
    if (!discoverer) {
        Log::Warning("MediaPlayer Error creating discoverer instance: %s\n", err->message);
    }
    else {
        GstDiscovererInfo *info = NULL;
        info = gst_discoverer_discover_uri (discoverer, uri.c_str(), &err);
        GstDiscovererResult result = gst_discoverer_info_get_result (info);
        switch (result) {
        case GST_DISCOVERER_URI_INVALID:
            Log::Warning("'%s': Invalid URI", uri.c_str());
            break;
        case GST_DISCOVERER_ERROR:
            Log::Warning("'%s': %s", uri.c_str(), err->message);
            break;
        case GST_DISCOVERER_TIMEOUT:
            Log::Warning("'%s': Timeout loading", uri.c_str());
            break;
        case GST_DISCOVERER_BUSY:
            Log::Warning("'%s': Busy", uri.c_str());
            break;
        case GST_DISCOVERER_MISSING_PLUGINS:
        {
            const GstStructure *s = gst_discoverer_info_get_misc (info);
            gchar *str = gst_structure_to_string (s);
            Log::Warning("'%s': Unknown file format (%s)", uri.c_str(), str);
            g_free (str);
        }
            break;
        default:
        case GST_DISCOVERER_OK:
            break;
        }
        // no error, handle information found
        if ( result == GST_DISCOVERER_OK ) {

            GList *video_streams = gst_discoverer_info_get_video_streams(info);
            GList *tmp;
            for (tmp = video_streams; tmp && !stream_info.video_valid; tmp = tmp->next ) {
                GstDiscovererStreamInfo *tmpinf = (GstDiscovererStreamInfo *) tmp->data;
                if ( !stream_info.video_valid && GST_IS_DISCOVERER_VIDEO_INFO(tmpinf) )
                {
                    // found a video / image stream : fill-in information
                    GstDiscovererVideoInfo* vinfo = GST_DISCOVERER_VIDEO_INFO(tmpinf);
                    stream_info.width = gst_discoverer_video_info_get_width(vinfo);
                    stream_info.height = gst_discoverer_video_info_get_height(vinfo);
                    guint parn = gst_discoverer_video_info_get_par_num(vinfo);
                    guint pard = gst_discoverer_video_info_get_par_denom(vinfo);
                    stream_info.par_width = (stream_info.width * parn) / pard;
                    stream_info.depth = gst_discoverer_video_info_get_depth(vinfo);
                    stream_info.interlaced = gst_discoverer_video_info_is_interlaced(vinfo);
                    stream_info.bitrate = gst_discoverer_video_info_get_bitrate(vinfo);
                    stream_info.isimage = gst_discoverer_video_info_is_image(vinfo);
                    // if its a video, set duration, framerate, etc.
                    if ( !stream_info.isimage ) {
                        stream_info.end = gst_discoverer_info_get_duration (info) ;
                        stream_info.seekable = gst_discoverer_info_get_seekable (info);
                        stream_info.framerate_n = gst_discoverer_video_info_get_framerate_num(vinfo);
                        stream_info.framerate_d = gst_discoverer_video_info_get_framerate_denom(vinfo);
                        if (stream_info.framerate_n == 0 || stream_info.framerate_d == 0) {
                            Log::Info("'%s': No framerate indicated in the file; using default 30fps", uri.c_str());
                            stream_info.framerate_n = 30;
                            stream_info.framerate_d = 1;
                        }
                        stream_info.dt = ( (GST_SECOND * static_cast<guint64>(stream_info.framerate_d)) / (static_cast<guint64>(stream_info.framerate_n)) );
                        // confirm (or infirm) that its not a single frame
                        if ( stream_info.end < stream_info.dt * 2)
                            stream_info.isimage = true;
                    }
                    // try to fill-in the codec information
                    GstCaps *caps = gst_discoverer_stream_info_get_caps (tmpinf);
                    if (caps) {
                        gchar *codecstring = gst_pb_utils_get_codec_description(caps);
                        stream_info.video_codec_name = std::string( codecstring );
                        g_free(codecstring);
                        gst_caps_unref (caps);
                    }
                    const GstTagList *tags = gst_discoverer_stream_info_get_tags(tmpinf);
                    if ( tags ) {
                        gchar *container = NULL;
                        if ( gst_tag_list_get_string (tags, GST_TAG_CONTAINER_FORMAT, &container) )
                             stream_info.video_codec_name += ", " + std::string(container);
                        if (container)
                            g_free(container);
                    }
                    // exit loop
                    // inform that it succeeded
                    stream_info.video_valid = true;
                }
            }
            gst_discoverer_stream_info_list_free(video_streams);

            GList *audio_streams = gst_discoverer_info_get_audio_streams(info);
            for (tmp = audio_streams; tmp && !stream_info.audio_valid; tmp = tmp->next ) {
                GstDiscovererStreamInfo *tmpinf = (GstDiscovererStreamInfo *) tmp->data;
                if ( !stream_info.audio_valid && GST_IS_DISCOVERER_AUDIO_INFO(tmpinf) )
                {
                    // found a audio stream : fill-in information
                    GstDiscovererAudioInfo* ainfo = GST_DISCOVERER_AUDIO_INFO(tmpinf);
                    stream_info.audio_sample_rate = gst_discoverer_audio_info_get_sample_rate(ainfo);
                    stream_info.audio_channels = gst_discoverer_audio_info_get_channels(ainfo);
                    stream_info.audio_depth = gst_discoverer_audio_info_get_depth(ainfo);
                    stream_info.audio_bitrate = gst_discoverer_audio_info_get_bitrate(ainfo);
                    // try to fill-in the codec information
                    GstCaps *caps = gst_discoverer_stream_info_get_caps (tmpinf);
                    if (caps) {
                        gchar *codecstring = gst_pb_utils_get_codec_description(caps);
                        stream_info.video_codec_name = std::string( codecstring );
                        g_free(codecstring);
                        gst_caps_unref (caps);
                    }
                    const GstTagList *tags = gst_discoverer_stream_info_get_tags(tmpinf);
                    if ( tags ) {
                        gchar *container = NULL;
                        if ( gst_tag_list_get_string (tags, GST_TAG_CONTAINER_FORMAT, &container) )
                             stream_info.audio_codec_name += ", " + std::string(container);
                        if (container)
                            g_free(container);
                    }
                    // exit loop
                    // inform that it succeeded
                    stream_info.audio_valid = true;
                }
            }
            gst_discoverer_stream_info_list_free(audio_streams);

            if (!stream_info.video_valid) {
                Log::Warning("'%s': No video stream", uri.c_str());
            }
            if (!stream_info.audio_valid) {
                Log::Warning("'%s': No audio stream", uri.c_str());
            }
        }

        if (info)
            gst_discoverer_info_unref (info);

        g_object_unref( discoverer );
    }

    g_clear_error (&err);

#ifdef LIMIT_DISCOVERER
    if (use_primary)
        mtx_primary.unlock();
    else
        mtx_secondary.unlock();
#endif
    // return the info
    return stream_info;
}

void MediaPlayer::open (const std::string & filename, const string &uri)
{
    // set path
    filename_ = BaseToolkit::transliterate( filename );

    // set uri to open
    if (uri.empty())
        uri_ = GstToolkit::filename_to_uri( filename );
    else
        uri_ = uri;

    if (uri_.empty())
        failed_ = true;

    // close before re-openning
    if (isOpen())
        close();

    // start URI discovering thread:
    discoverer_ = std::async( MediaPlayer::UriDiscoverer, uri_);
    // wait for discoverer to finish in the future (test in update)

    // debug without thread
    //media_ = MediaPlayer::UriDiscoverer(uri_);
    //if (media_.valid) {
    //    timeline_.setEnd( media_.end );
    //    timeline_.setStep( media_.dt );
    //    execute_open();
    //}
}


void MediaPlayer::reopen()
{
    // re-openning is meaningfull only if it was already open
    if (pipeline_ != nullptr) {
        // reload : terminate pipeline and re-create it
        close();
        execute_open();
    }
}

void MediaPlayer::execute_open() 
{
    // Create gstreamer pipeline :
    //         "uridecodebin uri=file:///path_to_file/filename.mp4 ! videoconvert ! appsink "
    // equivalent to command line
    //         "gst-launch-1.0 uridecodebin uri=file:///path_to_file/filename.mp4 ! videoconvert ! ximagesink"
    string description = "uridecodebin name=decoder uri=" + uri_ + " ! queue max-size-time=0 ! ";
    // NB: queue adds some control over the buffer, thereby limiting the frame delay. zero size means no buffering

#ifdef VIDEO_FORMAT_RGBA
    // string description = "uridecodebin name=decoder uri=" + uri_ + " decoder. ! ";
    // description += "audioconvert ! autoaudiosink decoder. ! ";

    // video deinterlacing method (if media is interlaced)
    //      tomsmocomp (0) – Motion Adaptive: Motion Search
    //      greedyh (1) – Motion Adaptive: Advanced Detection
    //      greedyl (2) – Motion Adaptive: Simple Detection
    //      vfir (3) – Blur Vertical
    //      linear (4) – Linear
    //      scalerbob (6) – Double lines
    if (media_.interlaced)
        description += "deinterlace method=2 ! ";

    // video convertion algorithm (should only do colorspace conversion, no scaling)
    // chroma-resampler:
    //      Duplicates the samples when upsampling and drops when downsampling 0
    //      Uses linear interpolation 1 (default)
    //      Uses cubic interpolation 2
    //      Uses sinc interpolation 3
    //  dither:
    //      no dithering 0
    //      propagate rounding errors downwards 1
    //      Dither with floyd-steinberg error diffusion 2
    //      Dither with Sierra Lite error diffusion 3
    //      ordered dither using a bayer pattern 4 (default)
    description += "videoconvert chroma-resampler=1 dither=0 ! "; // fast
#else
    description += "videoconvert chroma-resampler=0 dither=0 ! ";
#endif

    // hack to compensate for lack of PTS in gif animations
    if (media_.video_codec_name.compare("image/gst-libav-gif") == 0){
        description += "videorate ! video/x-raw,framerate=";
        description += std::to_string(media_.framerate_n) + "/";
        description += std::to_string(media_.framerate_d) + " ! ";
    }

    // set app sink
    description += "appsink name=video_appsink";

    // set audio convert and sink
    if (media_.audio_valid)
    {
        //description += " decoder. ! queue ! audioconvert ! tee name=t ! queue ! autoaudiosink t. ! queue ! appsink name=audio_sink";
        description += " decoder. ! queue ! audioconvert !";
        description += " audio/x-raw,channels=" + std::to_string(media_.audio_channels) +
                       ",format=F32LE,rate=" + std::to_string(media_.audio_sample_rate) + " ! ";
        description += " tee name=t ! queue !";
        description += " appsink name=audio_appsink";
        description += " t. ! queue ! volume name=audio_volume ! autoaudiosink name=audio_render";
    }

    // parse pipeline descriptor
    GError *error = NULL;
    pipeline_ = gst_parse_launch (description.c_str(), &error);
    if (error != NULL) {
        Log::Warning("MediaPlayer %s Could not construct pipeline %s:\n%s", std::to_string(id_).c_str(), description.c_str(), error->message);
        g_clear_error (&error);
        failed_ = true;
        return;
    }

    // setup pipeline
    g_object_set(G_OBJECT(pipeline_), "name", std::to_string(id_).c_str(), NULL);
    gst_pipeline_set_auto_flush_bus( GST_PIPELINE(pipeline_), true);

    // format: { AYUV64, ARGB64, GBRA_12LE, GBRA_12BE, Y412_LE, Y412_BE, A444_10LE, GBRA_10LE, 
    //           A444_10BE, GBRA_10BE, A422_10LE, A422_10BE, A420_10LE, A420_10BE, RGB10A2_LE, BGR10A2_LE,
    //           Y410, GBRA, ABGR, VUYA, BGRA, AYUV, ARGB, 
    //           RGBA, A420, AV12, Y444_16LE, Y444_16BE, v216, P016_LE, P016_BE,
    //           Y444_12LE, GBR_12LE, Y444_12BE, GBR_12BE, I422_12LE, I422_12BE, Y212_LE, Y212_BE,
    //           I420_12LE, I420_12BE, P012_LE, P012_BE, Y444_10LE, GBR_10LE, Y444_10BE, GBR_10BE,
    //           r210, I422_10LE, I422_10BE, NV16_10LE32, Y210, v210, UYVP, I420_10LE,
    //           I420_10BE, P010_10LE, NV12_10LE32, NV12_10LE40, P010_10BE, Y444, RGBP, GBR,
    //           BGRP, NV24, xBGR, BGRx, xRGB, RGBx, BGR, IYU2,
    //           v308, RGB, Y42B, NV61, NV16, VYUY, UYVY, YVYU,
    //           YUY2, I420, YV12, NV21, NV12, NV12_64Z32, NV12_4L4, NV12_32L32,
    //           Y41B, IYU1, YVU9, YUV9, RGB16, BGR16, RGB15, BGR15,
    //           RGB8P, GRAY16_LE, GRAY16_BE, GRAY10_LE32, GRAY8 }
    int pixel_element_depth = media_.depth / 3;
#ifdef VIDEO_FORMAT_RGBA
    string capstring = "video/x-raw,format=RGBA,width="+ std::to_string(media_.width) +
            ",height=" + std::to_string(media_.height);
#elif defined(VIDEO_FORMAT_NV12)
    string capstring = "video/x-raw,format=" + (pixel_element_depth == 8 ? std::string("NV12") : std::string("P010_10LE")) + ",width=" + std::to_string(media_.width) +
            ",height=" + std::to_string(media_.height);
#elif defined(VIDEO_FORMAT_YV12)
    string capstring = "video/x-raw,format=" + (pixel_element_depth == 8 ? std::string("I420") : std::string("I420_10LE")) + ",width=" + std::to_string(media_.width) +
            ",height=" + std::to_string(media_.height);
#else
    #error "please define VIDEO_FORMAT_ in header file"
#endif
    GstCaps *caps = gst_caps_from_string(capstring.c_str());
    if (!gst_video_info_from_caps (&v_frame_video_info_, caps)) {
        Log::Warning("MediaPlayer %s Could not configure video frame info", std::to_string(id_).c_str());
        failed_ = true;
        return;
    }

    // setup uridecodebin
    if (force_software_decoding_) {
        g_object_set (G_OBJECT (gst_bin_get_by_name (GST_BIN (pipeline_), "decoder")), "force-sw-decoders", true,  NULL);
    }

    // setup appsink
    GstElement *video_appsink = gst_bin_get_by_name (GST_BIN (pipeline_), "video_appsink");
    if (!video_appsink) {
        Log::Warning("MediaPlayer %s Could not configure video_appsink", std::to_string(id_).c_str());
        failed_ = true;
        return;
    }

    // instruct the sink to send samples synched in time
    gst_base_sink_set_sync (GST_BASE_SINK(video_appsink), true);

    // instruct sink to use the required caps
    gst_app_sink_set_caps (GST_APP_SINK(video_appsink), caps);

    // Instruct appsink to drop old buffers when the maximum amount of queued buffers is reached.
    gst_app_sink_set_max_buffers( GST_APP_SINK(video_appsink), N_VFRAME);
    gst_app_sink_set_buffer_list_support( GST_APP_SINK(video_appsink), true);
    gst_app_sink_set_drop (GST_APP_SINK(video_appsink), true);

#ifdef USE_GST_APPSINK_CALLBACKS
    // set the callbacks
    GstAppSinkCallbacks callbacks;
    callbacks.new_preroll = video_callback_new_preroll;
    if (media_.isimage) {
        callbacks.eos = NULL;
        callbacks.new_sample = NULL;
    }
    else {
        callbacks.eos = video_callback_end_of_stream;
        callbacks.new_sample = video_callback_new_sample;
    }
    gst_app_sink_set_callbacks (GST_APP_SINK(video_appsink), &callbacks, this, NULL);
    gst_app_sink_set_emit_signals (GST_APP_SINK(video_appsink), false);
#else
    // connect video signals callbacks
    g_signal_connect(G_OBJECT(video_appsink), "new-preroll", G_CALLBACK (video_callback_new_preroll), this);
    if (!media_.isimage) {
        g_signal_connect(G_OBJECT(video_appsink), "new-sample", G_CALLBACK (video_callback_new_sample), this);
        g_signal_connect(G_OBJECT(video_appsink), "eos", G_CALLBACK (video_callback_end_of_stream), this);
    }
    gst_app_sink_set_emit_signals (GST_APP_SINK(video_appsink), true);
#endif

    // done with ref to sink
    gst_object_unref (video_appsink);
    gst_caps_unref (caps);

    if (media_.audio_valid)
    {
       gst_audio_info_set_format(&v_frame_audio_info_, GST_AUDIO_FORMAT_F32LE, media_.audio_sample_rate, media_.audio_channels, nullptr);
       GstCaps *caps_audio = gst_audio_info_to_caps(&v_frame_audio_info_);
       if (!caps_audio)
       {
           Log::Warning("MediaPlayer %s Could not configure audio frame info", std::to_string(id_).c_str());
            failed_ = true;
            return;
       }

        // setup audio app sink
        GstElement *audio_appsink = gst_bin_get_by_name (GST_BIN (pipeline_), "audio_appsink");
        if (!audio_appsink) {
            Log::Warning("MediaPlayer %s Could not get audio_appsink", std::to_string(id_).c_str());
            failed_ = true;
            return;
        }
        
        // instruct the sink to send samples synched in time
        gst_base_sink_set_sync (GST_BASE_SINK(audio_appsink), true);

        // instruct sink to use the required caps
        gst_app_sink_set_caps (GST_APP_SINK(audio_appsink), caps_audio);

        // Instruct appsink to drop old buffers when the maximum amount of queued buffers is reached.
        gst_app_sink_set_max_buffers( GST_APP_SINK(audio_appsink), N_AFRAME);
        gst_app_sink_set_buffer_list_support( GST_APP_SINK(audio_appsink), true);
        gst_app_sink_set_drop (GST_APP_SINK(audio_appsink), true);

#ifdef USE_GST_APPSINK_CALLBACKS
    // set the callbacks
        GstAppSinkCallbacks callbacks;
        callbacks.new_preroll = audio_callback_new_preroll;
        callbacks.eos = audio_callback_end_of_stream;
        callbacks.new_sample = audio_callback_new_sample;
        gst_app_sink_set_callbacks (GST_APP_SINK(audio_appsink), &callbacks, this, NULL);
        gst_app_sink_set_emit_signals (GST_APP_SINK(audio_appsink), false);
#else
        // connect video signals callbacks
        g_signal_connect(G_OBJECT(audio_appsink), "new-preroll", G_CALLBACK (audio_callback_new_preroll), this);
        g_signal_connect(G_OBJECT(audio_appsink), "new-sample", G_CALLBACK (audio_callback_new_sample), this);
        g_signal_connect(G_OBJECT(audio_appsink), "eos", G_CALLBACK (audio_callback_end_of_stream), this);
        gst_app_sink_set_emit_signals (GST_APP_SINK(audio_appsink), true);
#endif
        // done with ref to audio appsink
        gst_object_unref(audio_appsink);

        // done with ref to audio caps
        gst_caps_unref(caps_audio);

        // init audio channel level
        audio_channel_level.resize(media_.audio_channels);
    }

    // set to desired state (PLAY or PAUSE)
    GstStateChangeReturn ret = gst_element_set_state (pipeline_, desired_state_);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        Log::Warning("MediaPlayer %s Could not open '%s'", std::to_string(id_).c_str(), uri_.c_str());
        failed_ = true;
        return;
    }

    // in case discoverer failed to get duration
    if (timeline_.end() == GST_CLOCK_TIME_NONE) {
        gint64 d = GST_CLOCK_TIME_NONE;
        if ( gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &d) )
            timeline_.setEnd(d);
    }

    // all good
    Log::Info("MediaPlayer %s Opened '%s' (%s %d x %d)", std::to_string(id_).c_str(),
              uri_.c_str(), media_.video_codec_name.c_str(), media_.width, media_.height);

    Log::Info("MediaPlayer %s Timeline [%ld %ld] %ld frames, %d gaps", std::to_string(id_).c_str(),
              timeline_.begin(), timeline_.end(), timeline_.numFrames(), timeline_.numGaps());

    opened_ = true;

    // register media player
    MediaPlayer::registered_.push_back(this);
}

bool MediaPlayer::isOpen() const
{
    return opened_;
}

bool MediaPlayer::failed() const
{
    return failed_;
}

void MediaPlayer::VFrame::unmap()
{
    if ( full )
        gst_video_frame_unmap(&frame);
    full = false;
}

void MediaPlayer::AFrame::unmap()
{
    if ( full )
        gst_audio_buffer_unmap(&frame);
    full = false;
}

void MediaPlayer::close()
{
    // not openned?
    if (!opened_) {
        // wait for loading to finish
        if (discoverer_.valid())
            discoverer_.wait();
        // nothing else to change
        return;
    }

    // un-ready the media player
    opened_ = false;

    // clean up GST
    if (pipeline_ != nullptr) {

        // force flush
        GstState state;
        gst_element_send_event(pipeline_, gst_event_new_seek (1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
                    GST_SEEK_TYPE_NONE, 0, GST_SEEK_TYPE_NONE, 0) );
        gst_element_get_state (pipeline_, &state, NULL, GST_CLOCK_TIME_NONE);

        // end pipeline
        gst_element_set_state (pipeline_, GST_STATE_NULL);
        gst_element_get_state (pipeline_, &state, NULL, GST_CLOCK_TIME_NONE);

        gst_object_unref (pipeline_);
        pipeline_ = nullptr;
    }

    // cleanup eventual remaining video frame memory
    for(guint i = 0; i < N_VFRAME; i++) {
        vframe_[i].access.lock();
        vframe_[i].unmap();
        vframe_[i].access.unlock();
    }
    vwrite_index_ = 0;
    vlast_index_ = 0;

    // cleanup eventual remaining audio frame memory ?
    //for(guint i = 0; i < N_AFRAME; i++) {
    //    aframe_[i].access.lock();
    //    aframe_[i].unmap();
    //    aframe_[i].access.unlock();
    //}
    awrite_index_ = 0;
    alast_index_ = 0;

    VMat.release();

    audio_channel_level.clear();
    AMat.release();

#ifdef MEDIA_PLAYER_DEBUG
    Log::Info("MediaPlayer %s closed", std::to_string(id_).c_str());
#endif

    // unregister media player
    MediaPlayer::registered_.remove(this);
}


guint MediaPlayer::width() const
{
    return media_.width;
}

guint MediaPlayer::height() const
{
    return media_.height;
}

float MediaPlayer::aspectRatio() const
{
    return static_cast<float>(media_.par_width) / static_cast<float>(media_.height);
}

guint MediaPlayer::sample_rate() const
{
    return media_.audio_sample_rate;
}

guint MediaPlayer::channels() const
{
    return media_.audio_channels;
}

guint MediaPlayer::audio_depth() const
{
    return media_.audio_depth;
}

double MediaPlayer::volume() const
{
    double vol = 0;
    GstElement *audio_volume = gst_bin_get_by_name (GST_BIN (pipeline_), "audio_volume");
    if (!audio_volume)
    {
        Log::Warning("MediaPlayer %s Could not get audio volume control", std::to_string(id_).c_str());
        return vol;
    }
    g_object_get(audio_volume, "volume", &vol, NULL);
    gst_object_unref(audio_volume);
    return vol;
}

void MediaPlayer::set_volume(double vol)
{
    g_object_set(G_OBJECT (gst_bin_get_by_name (GST_BIN (pipeline_), "audio_volume")), "volume", vol,  NULL);
}

GstClockTime MediaPlayer::position()
{
    if (position_ == GST_CLOCK_TIME_NONE && pipeline_ != nullptr) {
        gint64 p = GST_CLOCK_TIME_NONE;
        if ( gst_element_query_position (pipeline_, GST_FORMAT_TIME, &p) )
            position_ = p;
    }

    return position_;
}

void MediaPlayer::enable(bool on)
{
    if ( !opened_ || pipeline_ == nullptr)
        return;

    if ( enabled_ != on ) {

        // option to automatically rewind each time the player is disabled
        if (!on && rewind_on_disable_ && desired_state_ == GST_STATE_PLAYING)
            rewind(true);

        // apply change
        enabled_ = on;

        // default to pause
        GstState requested_state = GST_STATE_PAUSED;

        // unpause only if enabled
        if (enabled_)
            requested_state = desired_state_;

        //  apply state change
        GstStateChangeReturn ret = gst_element_set_state (pipeline_, requested_state);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            Log::Warning("MediaPlayer %s Failed to enable", std::to_string(id_).c_str());
            failed_ = true;
        }

    }
}

bool MediaPlayer::isEnabled() const
{
    return enabled_;
}

bool MediaPlayer::isImage() const
{
    return media_.isimage;
}

std::string MediaPlayer::decoderName()
{
    // decoder_name_ not initialized
    if (decoder_name_.empty()) {
        // try to know if it is a hardware decoder
        decoder_name_ = GstToolkit::used_gpu_decoding_plugins(pipeline_);
        // nope, then it is a sofware decoder
        if (decoder_name_.empty())
            decoder_name_ = "software";
    }

    return decoder_name_;
}

bool MediaPlayer::softwareDecodingForced()
{
    return force_software_decoding_;
}

void MediaPlayer::setSoftwareDecodingForced(bool on)
{
    bool need_reload = force_software_decoding_ != on;

    // set parameter
    force_software_decoding_ = on;
    decoder_name_ = "";

    // changing state requires reload
    if (need_reload)
        reopen();
}

void MediaPlayer::play(bool on)
{
    // ignore if disabled, and cannot play an image
    if (!enabled_ || media_.isimage)
        return;

    // request state 
    GstState requested_state = on ? GST_STATE_PLAYING : GST_STATE_PAUSED;

    // ignore if requesting twice same state
    if (desired_state_ == requested_state)
        return;

    // accept request to the desired state
    desired_state_ = requested_state;

    // if not ready yet, the requested state will be handled later
    if ( pipeline_ == nullptr )
        return;

    // requesting to play, but stopped at end of stream : rewind first !
    if ( desired_state_ == GST_STATE_PLAYING) {
        if ( ( rate_ < 0.0 && position_ <= timeline_.next(0)  )
             || ( rate_ > 0.0 && position_ >= timeline_.previous(timeline_.last()) ) )
            rewind();
    }

    // all ready, apply state change immediately
    GstStateChangeReturn ret = gst_element_set_state (pipeline_, desired_state_);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        Log::Warning("MediaPlayer %s Failed to set play state", std::to_string(id_).c_str());
        failed_ = true;
    }

#ifdef MEDIA_PLAYER_DEBUG
    else if (on)
        Log::Info("MediaPlayer %s Start", std::to_string(id_).c_str());
    else
        Log::Info("MediaPlayer %s Stop [%ld]", std::to_string(id_).c_str(), position());
#endif
}

bool MediaPlayer::isPlaying(bool testpipeline) const
{
    // image cannot play
    if (media_.isimage)
        return false;

    // if not ready yet, answer with requested state
    if ( !testpipeline || pipeline_ == nullptr || !enabled_)
        return desired_state_ == GST_STATE_PLAYING;

    // if ready, answer with actual state
    GstState state;
    gst_element_get_state (pipeline_, &state, NULL, GST_CLOCK_TIME_NONE);
    return state == GST_STATE_PLAYING;
}


MediaPlayer::LoopMode MediaPlayer::loop() const
{
    return loop_;
}
    
void MediaPlayer::setLoop(MediaPlayer::LoopMode mode)
{
    loop_ = mode;
}

void MediaPlayer::rewind(bool force)
{
    if (!enabled_ || !media_.seekable)
        return;

    // playing forward, loop to begin
    if (rate_ > 0.0) {
        // begin is the end of a gab which includes the first PTS (if exists)
        // normal case, begin is zero
        execute_seek_command( timeline_.next(0) );
    }
    // playing backward, loop to endTimeInterval gap;
    else {
        // end is the start of a gab which includes the last PTS (if exists)
        // normal case, end is last frame
        execute_seek_command( timeline_.previous(timeline_.last()) );
    }

    if (force) {
        GstState state;
        gst_element_get_state (pipeline_, &state, NULL, GST_CLOCK_TIME_NONE);
        update();
    }
}


void MediaPlayer::step()
{
    // useful only when Paused
    if (!enabled_ || isPlaying())
        return;

    if ( ( rate_ < 0.0 && position_ <= timeline_.next(0)  )
         || ( rate_ > 0.0 && position_ >= timeline_.previous(timeline_.last()) ) )
        rewind();

    // step 
    gst_element_send_event (pipeline_, gst_event_new_step (GST_FORMAT_BUFFERS, 1, ABS(rate_), TRUE,  FALSE));
}

bool MediaPlayer::go_to(GstClockTime pos)
{
    bool ret = false;
    TimeInterval gap;
    if (pos != GST_CLOCK_TIME_NONE ) {

        GstClockTime jumpPts = pos;

        if (timeline_.getGapAt(pos, gap)) {
            // if in a gap, find closest seek target
            if (gap.is_valid()) {
                // jump in one or the other direction
                jumpPts = (rate_>0.f) ? gap.end : gap.begin;
            }
        }

        if (ABS_DIFF (position_, jumpPts) > 2 * timeline_.step() ) {
            ret = true;
            seek( jumpPts );
        }
    }
    return ret;
}

void MediaPlayer::seek(GstClockTime pos)
{
    if (!enabled_ || !media_.seekable || seeking_)
        return;

    // apply seek
    GstClockTime target = CLAMP(pos, timeline_.begin(), timeline_.end());
    execute_seek_command(target);

}

void MediaPlayer::jump()
{
    if (!enabled_ || !isPlaying())
        return;

    gst_element_send_event (pipeline_, gst_event_new_step (GST_FORMAT_BUFFERS, 1, 30.f * ABS(rate_), TRUE,  FALSE));
}

void MediaPlayer::fill_video(guint index)
{
#ifdef VIDEO_FORMAT_RGBA
    int data_shift = media_.depth > 32 ? 1 : 0;
    VMat.create_type(media_.width, media_.height, 4, data_shift ? IM_DT_INT16 : IM_DT_INT8);
    uint8_t* src_data = (uint8_t*)vframe_[index].frame.data[0];
    uint8_t* dst_data = (uint8_t*)VMat.data;
    memcpy(dst_data, src_data, media_.width * media_.height * (data_shift ? 2 : 1) * 4);
#else
    int data_shift = media_.depth > 24 ? 1 : 0;
#ifdef VIDEO_FORMAT_NV12
    int UV_shift_w = 0;
#elif defined(VIDEO_FORMAT_YV12)
    int UV_shift_w = 1;
#else
    #error "please define VIDEO_FORMAT_ in header file"
#endif
    int UV_shift_h = 1;
    VMat.create_type(media_.width, media_.height, 4, data_shift ? IM_DT_INT16 : IM_DT_INT8);
    ImGui::ImMat mat_Y = VMat.channel(0);
    {
        uint8_t* src_data = (uint8_t*)vframe_[index].frame.data[0];
        uint8_t* dst_data = (uint8_t*)mat_Y.data;
        for (int i = 0; i < media_.height; i++)
        {
            memcpy(dst_data, src_data, media_.width * (data_shift ? 2 : 1));
            src_data += GST_VIDEO_FRAME_PLANE_STRIDE(&vframe_[index].frame, 0);
            dst_data += media_.width << data_shift;
        }
    }
    ImGui::ImMat mat_Cb = VMat.channel(1);
    {
        uint8_t* src_data = (uint8_t*)vframe_[index].frame.data[1];
        uint8_t* dst_data = (uint8_t*)mat_Cb.data;
        for (int i = 0; i < media_.height >> UV_shift_h; i++)
        {
            memcpy(dst_data, src_data, (media_.width >> UV_shift_w) * (data_shift ? 2 : 1));
            src_data += GST_VIDEO_FRAME_PLANE_STRIDE(&vframe_[index].frame, 1);
            dst_data += (media_.width >> UV_shift_w) << data_shift;
        }
    }
#ifdef VIDEO_FORMAT_YV12
    ImGui::ImMat mat_Cr = VMat.channel(2);
    {
        uint8_t* src_data = (uint8_t*)vframe_[index].frame.data[2];
        uint8_t* dst_data = (uint8_t*)mat_Cr.data;
        for (int i = 0; i < media_.height >> UV_shift_h; i++)
        {
            memcpy(dst_data, src_data, (media_.width >> UV_shift_w) * (data_shift ? 2 : 1));
            src_data += GST_VIDEO_FRAME_PLANE_STRIDE(&vframe_[index].frame, 2);
            dst_data += (media_.width >> UV_shift_w) << data_shift;
        }
    }
#endif
#endif
    auto color_space = GST_VIDEO_INFO_COLORIMETRY(&vframe_[index].frame.info);
    auto color_range = GST_VIDEO_INFO_CHROMA_SITE(&vframe_[index].frame.info);
    VMat.time_stamp = vframe_[index].position / (1e+9);
    VMat.depth = media_.depth / 3;
    VMat.rate = {static_cast<int>(media_.framerate_n), static_cast<int>(media_.framerate_d)};
    VMat.flags = IM_MAT_FLAGS_VIDEO_FRAME;
#ifdef VIDEO_FORMAT_RGBA
    VMat.color_space = IM_CS_SRGB;
    VMat.color_format = IM_CF_ABGR;
    VMat.color_range = IM_CR_FULL_RANGE;
#else
    VMat.color_space = color_space.primaries == GST_VIDEO_COLOR_PRIMARIES_BT709 ? IM_CS_BT709 :
                       color_space.primaries == GST_VIDEO_COLOR_PRIMARIES_BT2020 ? IM_CS_BT2020 : IM_CS_BT601;
    VMat.color_range = color_range == GST_VIDEO_CHROMA_SITE_JPEG ? IM_CR_FULL_RANGE : 
                       color_range == GST_VIDEO_CHROMA_SITE_MPEG2 ? IM_CR_NARROW_RANGE : IM_CR_FULL_RANGE;
#ifdef VIDEO_FORMAT_NV12
    VMat.color_format = data_shift ? IM_CF_P010LE : IM_CF_NV12;
    VMat.flags |= IM_MAT_FLAGS_VIDEO_FRAME_UV;
#elif defined(VIDEO_FORMAT_YV12)
    VMat.color_format = IM_CF_YUV420;
#else
    #error "please define VIDEO_FORMAT_ in header file"
#endif
    if (GST_VIDEO_INFO_IS_INTERLACED(&vframe_[index].frame.info))  VMat.flags |= IM_MAT_FLAGS_VIDEO_INTERLACED;
#endif
    if (gst_video_colorimetry_matches(&color_space, GST_VIDEO_COLORIMETRY_BT2100_PQ)) VMat.flags |= IM_MAT_FLAGS_VIDEO_HDR_PQ;
    if (gst_video_colorimetry_matches(&color_space, GST_VIDEO_COLORIMETRY_BT2100_HLG)) VMat.flags |= IM_MAT_FLAGS_VIDEO_HDR_HLG;
}

void MediaPlayer::fill_audio(guint index)
{
    // deal with the frame at reading index
    auto data = aframe_[index].frame.planes[0];
    for (int i = 0; i < v_frame_audio_info_.channels; i++)
    {
        audio_channel_level[i] = calculate_audio_db<float>((const float *)data, v_frame_audio_info_.channels, i, aframe_[index].frame.n_samples, 1.0);
    }
    auto total_sample_length = aframe_[index].frame.n_samples / v_frame_audio_info_.channels;
    AMat.create_type(total_sample_length, 1, v_frame_audio_info_.channels, IM_DT_FLOAT32);
    float * buffer = (float *)data;
    for (int i = 0; i < AMat.w; i++)
    {
        for (int c = 0; c < AMat.c; c++)
        {
            AMat.at<float>(i, 0, c) = (*buffer);
            buffer ++;
        }
    }
    AMat.time_stamp = aframe_[index].position / 1e+9;
    AMat.rate = {v_frame_audio_info_.rate, 1};
    AMat.flags = IM_MAT_FLAGS_AUDIO_FRAME;
}

void MediaPlayer::update()
{
    // discard
    if (failed_)
        return;

    // not ready yet
    if (!opened_) {
        if (discoverer_.valid()) {
            // try to get info from discoverer
            if (discoverer_.wait_for( std::chrono::milliseconds(4) ) == std::future_status::ready )
            {
                media_ = discoverer_.get();
                // if its ok, open the media
                if (media_.video_valid || media_.audio_valid) {
                    timeline_.setEnd( media_.end );
                    timeline_.setStep( media_.dt );
                    execute_open();
                }
                else {
                    Log::Warning("MediaPlayer %s Loading cancelled", std::to_string(id_).c_str());
                    failed_ = true;
                }
            }
        }
        // wait next frame to display
        return;
    }

    // prevent unnecessary updates: disabled or already filled image
    if (!enabled_)
        return;

    if (media_.isimage && !VMat.empty())
        return;

    // video update
    // local variables before trying to update
    guint v_read_index = 0;
    bool need_loop = false;

    // locked access to current index
    vindex_lock_.lock();
    // get the last frame filled from fill_video_frame()
    v_read_index = vlast_index_;
//    // Do NOT miss and jump directly (after seek) to a pre-roll
//    for (guint i = 0; i < N_VFRAME; ++i) {
//        if (frame_[i].status == PREROLL) {
//            read_index = i;
//            break;
//        }
//    }
    // unlock access to index change
    vindex_lock_.unlock();

    // lock frame while reading it
    vframe_[v_read_index].access.lock();

    // do not fill a frame twice
    if (vframe_[v_read_index].status != INVALID ) {

        // is this an End-of-Stream frame ?
        if (vframe_[v_read_index].status == EOS )
        {
            // will execute seek command below (after unlock)
            need_loop = true;
        }
        // otherwise just fill non-empty SAMPLE or PREROLL
        else if (vframe_[v_read_index].full)
        {
            // fill the texture with the frame at reading index
            fill_video(v_read_index);

            // double update for pre-roll frame and dual PBO (ensure frame is displayed now)
            if (vframe_[v_read_index].status == PREROLL || seeking_ )
                fill_video(v_read_index);

            // free frame
            vframe_[v_read_index].unmap();
        }

        // we just displayed a vframe : set position time to frame PTS
        position_ = vframe_[v_read_index].position;

        // avoid reading it again
        vframe_[v_read_index].status = INVALID;
    }

    // unlock frame after reading it
    vframe_[v_read_index].access.unlock();

    // audio update
    // local variables before trying to update
    guint a_read_index = 0;

    // locked access to current index
    aindex_lock_.lock();
    // get the last frame filled from fill_audio_frame()
    a_read_index = alast_index_;
//    // Do NOT miss and jump directly (after seek) to a pre-roll
//    for (guint i = 0; i < N_AFRAME; ++i) {
//        if (frame_[i].status == PREROLL) {
//            read_index = i;
//            break;
//        }
//    }
    // unlock access to index change
    aindex_lock_.unlock();

    // lock frame while reading it
    aframe_[a_read_index].access.lock();

    // do not fill a frame twice
    if (aframe_[a_read_index].status != INVALID ) {

        // is this an End-of-Stream frame ?
        if (aframe_[a_read_index].status == EOS )
        {
            // will execute seek command below (after unlock)
            need_loop = true;
        }
        // otherwise just fill non-empty SAMPLE or PREROLL
        else if (aframe_[a_read_index].full)
        {
            fill_audio(a_read_index);

            // double update for pre-roll frame and dual PBO (ensure frame is displayed now)
            if (aframe_[a_read_index].status == PREROLL || seeking_ )
            {
                fill_audio(a_read_index);
            }

            // free frame
            aframe_[a_read_index].unmap();
        }

        // do we set position time to frame PTS ?
        if (position_ == GST_CLOCK_TIME_NONE)
            position_ = aframe_[a_read_index].position;

        // avoid reading it again
        aframe_[a_read_index].status = INVALID;
    }

    // unlock frame after reading it
    aframe_[a_read_index].access.unlock();

    // if already seeking (asynch)
    if (seeking_) {
        // request status update to pipeline (re-sync gst thread)
        GstState state;
        gst_element_get_state (pipeline_, &state, NULL, GST_CLOCK_TIME_NONE);
        // seek should be resolved next frame
        seeking_ = false;
        // do NOT do another seek yet
    }
    // otherwise check for need to seek (pipeline management)
    else {
        // manage timeline: test if position falls into a gap
        TimeInterval gap;
        if (position_ != GST_CLOCK_TIME_NONE && timeline_.getGapAt(position_, gap)) {
            // if in a gap, seek to next section
            if (gap.is_valid()) {
                // jump in one or the other direction
                GstClockTime jumpPts = timeline_.step(); // round jump time to frame pts
                if ( rate_ > 0.f )
                    jumpPts *= ( gap.end / timeline_.step() ) + 1; // FWD: go to end of gap
                else
                    jumpPts *= ( gap.begin / timeline_.step() );   // BWD: go to begin of gap
                // (if not beginnig or end of timeline)
                if (jumpPts > timeline_.first() && jumpPts < timeline_.last())
                    // seek to jump PTS time
                    seek( jumpPts );
                // otherwise, we should loop
                else
                    need_loop = true;
            }
        }
    }

    // manage loop mode
    if (need_loop) {
        execute_loop_command();
    }
}

void MediaPlayer::execute_loop_command()
{
    if (loop_==LOOP_REWIND) {
        rewind();
    } 
    else if (loop_==LOOP_BIDIRECTIONAL) {
        rate_ *= - 1.f;
        execute_seek_command();
    }
    else { //LOOP_NONE
        play(false);
    }
}

void MediaPlayer::execute_seek_command(GstClockTime target)
{
    if ( pipeline_ == nullptr || !media_.seekable )
        return;

    // seek position : default to target
    GstClockTime seek_pos = target;

    // no target given
    if (target == GST_CLOCK_TIME_NONE) 
        // create seek event with current position (rate changed ?)
        seek_pos = position_;
    // target is given but useless
    else if ( ABS_DIFF(target, position_) < timeline_.step()) {
        // ignore request
        return;
    }

    // seek with flush (always)
    int seek_flags = GST_SEEK_FLAG_FLUSH;

    // seek with trick mode if fast speed
    if ( ABS(rate_) > 1.5 )
        seek_flags |= GST_SEEK_FLAG_TRICKMODE;
    else
        seek_flags |= GST_SEEK_FLAG_ACCURATE;

    // create seek event depending on direction
    GstEvent *seek_event = nullptr;
    if (rate_ > 0) {
        seek_event = gst_event_new_seek (rate_, GST_FORMAT_TIME, (GstSeekFlags) seek_flags,
            GST_SEEK_TYPE_SET, seek_pos, GST_SEEK_TYPE_END, 0);
    }
    else {
        seek_event = gst_event_new_seek (rate_, GST_FORMAT_TIME, (GstSeekFlags) seek_flags,
            GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_SET, seek_pos);
    }

    // Send the event (ASYNC)
    if (seek_event && !gst_element_send_event(pipeline_, seek_event) )
        Log::Warning("MediaPlayer %s Seek failed", std::to_string(id_).c_str());
    else {
        seeking_ = true;
#ifdef MEDIA_PLAYER_DEBUG
        Log::Info("MediaPlayer %s Seek %ld %.1f", std::to_string(id_).c_str(), seek_pos, rate_);
#endif
    }
}

void MediaPlayer::setPlaySpeed(double s)
{
    if (media_.isimage)
        return;

    // bound to interval [-MAX_PLAY_SPEED MAX_PLAY_SPEED] 
    rate_ = CLAMP(s, -MAX_PLAY_SPEED, MAX_PLAY_SPEED);
    // skip interval [-MIN_PLAY_SPEED MIN_PLAY_SPEED]
    if (ABS(rate_) < MIN_PLAY_SPEED)
        rate_ = SIGN(rate_) * MIN_PLAY_SPEED;
        
    // apply with seek
    execute_seek_command();
}

double MediaPlayer::playSpeed() const
{
    return rate_;
}

Timeline *MediaPlayer::timeline()
{
    return &timeline_;
}

float MediaPlayer::currentTimelineFading()
{
    return timeline_.fadingAt(position_);
}

void MediaPlayer::setTimeline(const Timeline &tl)
{
    timeline_ = tl;
}

MediaInfo MediaPlayer::media() const
{
    return media_;
}

std::string MediaPlayer::uri() const
{
    return uri_;
}

std::string MediaPlayer::filename() const
{
    return filename_;
}

double MediaPlayer::frameRate() const
{
    return static_cast<double>(media_.framerate_n) / static_cast<double>(media_.framerate_d);;
}

double MediaPlayer::updateFrameRate() const
{
    return timecount_.frameRate();
}


// CALLBACKS

bool MediaPlayer::fill_video_frame(GstBuffer *buf, FrameStatus status)
{
    // Do NOT overwrite an unread EOS
    if ( vframe_[vwrite_index_].status == EOS )
        vwrite_index_ = (vwrite_index_ + 1) % N_VFRAME;

    // lock access to frame
    vframe_[vwrite_index_].access.lock();

    // always empty frame before filling it again
    vframe_[vwrite_index_].unmap();

    // accept status of frame received
    vframe_[vwrite_index_].status = status;

    // a buffer is given (not EOS)
    if (buf != NULL) {
        // get the frame from buffer
        if ( !gst_video_frame_map (&vframe_[vwrite_index_].frame, &v_frame_video_info_, buf, GST_MAP_READ ) )
        {
#ifdef MEDIA_PLAYER_DEBUG
            Log::Info("MediaPlayer %s Failed to map the video buffer", std::to_string(id_).c_str());
#endif
            // free access to frame & exit
            vframe_[vwrite_index_].status = INVALID;
            vframe_[vwrite_index_].access.unlock();
            return false;
        }

        // successfully filled the frame
        vframe_[vwrite_index_].full = true;

        // validate frame format
#ifdef VIDEO_FORMAT_RGBA
        if( GST_VIDEO_INFO_IS_RGB(&(vframe_[vwrite_index_].frame).info) && GST_VIDEO_INFO_N_PLANES(&(vframe_[vwrite_index_].frame).info) == 1)
#elif defined(VIDEO_FORMAT_NV12)
        if( GST_VIDEO_INFO_IS_YUV(&(vframe_[vwrite_index_].frame).info) && GST_VIDEO_INFO_N_PLANES(&(vframe_[vwrite_index_].frame).info) == 2) // NV12/NV16
#elif defined(VIDEO_FORMAT_YV12)
        if( GST_VIDEO_INFO_IS_YUV(&(vframe_[vwrite_index_].frame).info) && GST_VIDEO_INFO_N_PLANES(&(vframe_[vwrite_index_].frame).info) == 3) // I420/I420_10LE
#else
        #error "please define VIDEO_FORMAT_ in header file"
#endif
        {
            // set presentation time stamp
            vframe_[vwrite_index_].position = buf->pts;

            // set the start position (i.e. pts of first frame we got)
            if (timeline_.first() == GST_CLOCK_TIME_NONE) {
                timeline_.setFirst(buf->pts);
            }
        }
        // full but invalid frame : will be deleted next iteration
        // (should never happen)
        else {
#ifdef MEDIA_PLAYER_DEBUG
            Log::Info("MediaPlayer %s Received an Invalid video frame", std::to_string(id_).c_str());
#endif
            // free access to frame & exit
            vframe_[vwrite_index_].status = INVALID;
            vframe_[vwrite_index_].access.unlock();
            return false;
        }
    }
    // else; null buffer for EOS: give a position
    else {
        vframe_[vwrite_index_].status = EOS;
        vframe_[vwrite_index_].position = rate_ > 0.0 ? timeline_.end() : timeline_.begin();
    }

    // unlock access to frame
    vframe_[vwrite_index_].access.unlock();

    // lock access to change current index (very quick)
    vindex_lock_.lock();
    // indicate update() that this is the last frame filled (and unlocked)
    vlast_index_ = vwrite_index_;
    // unlock access to index change
    vindex_lock_.unlock();

    // for writing, we will access the next in stack
    vwrite_index_ = (vwrite_index_ + 1) % N_VFRAME;

    // calculate actual FPS of update
    timecount_.tic();

    return true;
}

void MediaPlayer::video_callback_end_of_stream (GstAppSink *, gpointer p)
{
    MediaPlayer *m = static_cast<MediaPlayer *>(p);
    if (m && m->opened_) {
        m->fill_video_frame(NULL, MediaPlayer::EOS);
    }
}

GstFlowReturn MediaPlayer::video_callback_new_preroll (GstAppSink *sink, gpointer p)
{
    GstFlowReturn ret = GST_FLOW_OK;

    // blocking read pre-roll samples
    GstSample *sample = gst_app_sink_pull_preroll(sink);

    // if got a valid sample
    if (sample != NULL) {

        // send frames to media player only if ready
        MediaPlayer *m = static_cast<MediaPlayer *>(p);
        if (m && m->opened_) {

            // get buffer from sample
            GstBuffer *buf = gst_sample_get_buffer (sample);

            // fill frame from buffer
            if ( !m->fill_video_frame(buf, MediaPlayer::PREROLL) )
                ret = GST_FLOW_ERROR;
            // loop negative rate: emulate an EOS
            else if (m->playSpeed() < 0.f && !(buf->pts > 0) ) {
                m->fill_video_frame(NULL, MediaPlayer::EOS);
            }
        }
    }
    else
        ret = GST_FLOW_FLUSHING;

    // release sample
    gst_sample_unref (sample);

    return ret;
}

GstFlowReturn MediaPlayer::video_callback_new_sample (GstAppSink *sink, gpointer p)
{
    GstFlowReturn ret = GST_FLOW_OK;

    // non-blocking read new sample
    GstSample *sample = gst_app_sink_pull_sample(sink);

    // if got a valid sample
    if (sample != NULL && !gst_app_sink_is_eos (sink)) {

        // send frames to media player only if ready
        MediaPlayer *m = static_cast<MediaPlayer *>(p);
        if (m && m->opened_) {

            // get buffer from sample (valid until sample is released)
            GstBuffer *buf = gst_sample_get_buffer (sample) ;

            // fill frame with buffer
            if ( !m->fill_video_frame(buf, MediaPlayer::SAMPLE) )
                ret = GST_FLOW_ERROR;
            // loop negative rate: emulate an EOS
            else if (m->playSpeed() < 0.f && !(buf->pts > 0) ) {
                m->fill_video_frame(NULL, MediaPlayer::EOS);
            }
        }
    }
    else
        ret = GST_FLOW_FLUSHING;

    // release sample
    gst_sample_unref (sample);

    return ret;
}

bool MediaPlayer::fill_audio_frame(GstBuffer *buf, FrameStatus status)
{
    // Do NOT overwrite an unread EOS
    if ( aframe_[awrite_index_].status == EOS )
        awrite_index_ = (awrite_index_ + 1) % N_AFRAME;

    // lock access to frame
    aframe_[awrite_index_].access.lock();

    // always empty frame before filling it again ?
    //aframe_[awrite_index_].unmap();

    // accept status of frame received
    aframe_[awrite_index_].status = status;

    // a buffer is given (not EOS)
    if (buf != NULL) {
        // get the frame from buffer
        if ( !gst_audio_buffer_map (&aframe_[awrite_index_].frame, &v_frame_audio_info_, buf, GST_MAP_READ ) )
        {
#ifdef MEDIA_PLAYER_DEBUG
            Log::Info("MediaPlayer %s Failed to map the audio buffer", std::to_string(id_).c_str());
#endif
            // free access to frame & exit
            aframe_[awrite_index_].status = INVALID;
            aframe_[awrite_index_].access.unlock();
            return false;
        }

        // successfully filled the frame
        aframe_[awrite_index_].full = true;

        // validate frame format
        if( GST_AUDIO_INFO_IS_FLOAT(&(aframe_[awrite_index_].frame).info) && GST_AUDIO_BUFFER_N_PLANES(&aframe_[awrite_index_].frame) == 1)
        {
            // set presentation time stamp
            aframe_[awrite_index_].position = buf->pts;

            // set the start position (i.e. pts of first frame we got)
            if (timeline_.first() == GST_CLOCK_TIME_NONE) {
                timeline_.setFirst(buf->pts);
            }
        }
        // full but invalid frame : will be deleted next iteration
        // (should never happen)
        else {
#ifdef MEDIA_PLAYER_DEBUG
            Log::Info("MediaPlayer %s Received an Invalid audio frame", std::to_string(id_).c_str());
#endif
            // free access to frame & exit
            aframe_[awrite_index_].status = INVALID;
            aframe_[awrite_index_].access.unlock();
            return false;
        }
    }
    // else; null buffer for EOS: give a position
    else {
        aframe_[awrite_index_].status = EOS;
        aframe_[awrite_index_].position = rate_ > 0.0 ? timeline_.end() : timeline_.begin();
    }

    // unlock access to frame
    aframe_[awrite_index_].access.unlock();

    // lock access to change current index (very quick)
    aindex_lock_.lock();
    // indicate update() that this is the last frame filled (and unlocked)
    alast_index_ = awrite_index_;
    // unlock access to index change
    aindex_lock_.unlock();

    // for writing, we will access the next in stack
    awrite_index_ = (awrite_index_ + 1) % N_AFRAME;

    return true;
}

void MediaPlayer::audio_callback_end_of_stream (GstAppSink *, gpointer p)
{
    MediaPlayer *m = static_cast<MediaPlayer *>(p);
    if (m && m->opened_) {
        m->fill_audio_frame(NULL, MediaPlayer::EOS);
    }
}

GstFlowReturn MediaPlayer::audio_callback_new_preroll (GstAppSink *sink, gpointer p)
{
    GstFlowReturn ret = GST_FLOW_OK;
    // blocking read pre-roll samples
    GstSample *sample = gst_app_sink_pull_preroll(sink);

    // if got a valid sample
    if (sample != NULL) {
        // send frames to media player only if ready
        MediaPlayer *m = static_cast<MediaPlayer *>(p);
        if (m && m->opened_) {

            // get buffer from sample
            GstBuffer *buf = gst_sample_get_buffer (sample);
            // fill audio from buffer
            if ( !m->fill_audio_frame(buf, MediaPlayer::PREROLL) )
                ret = GST_FLOW_ERROR;
            // loop negative rate: emulate an EOS
            else if (m->playSpeed() < 0.f && !(buf->pts > 0) ) {
                m->fill_audio_frame(NULL, MediaPlayer::EOS);
            }
        }
    }
    else
        ret = GST_FLOW_FLUSHING;

    // release sample
    gst_sample_unref (sample);

    return ret;
}

GstFlowReturn MediaPlayer::audio_callback_new_sample (GstAppSink *sink, gpointer p)
{
    GstFlowReturn ret = GST_FLOW_OK;
    // non-blocking read new sample
    GstSample *sample = gst_app_sink_pull_sample(sink);

    // if got a valid sample
    if (sample != NULL && !gst_app_sink_is_eos (sink)) {
        // send frames to media player only if ready
        MediaPlayer *m = static_cast<MediaPlayer *>(p);
        if (m && m->opened_) {
            // get buffer from sample (valid until sample is released)
            GstBuffer *buf = gst_sample_get_buffer (sample) ;
            // fill audio with buffer
            if ( !m->fill_audio_frame(buf, MediaPlayer::SAMPLE) )
                ret = GST_FLOW_ERROR;
            // loop negative rate: emulate an EOS
            else if (m->playSpeed() < 0.f && !(buf->pts > 0) ) {
                m->fill_audio_frame(NULL, MediaPlayer::EOS);
            }
        }
    }
    else
        ret = GST_FLOW_FLUSHING;

    // release sample
    gst_sample_unref (sample);

    return ret;
}

MediaPlayer::TimeCounter::TimeCounter()
{
    timer = g_timer_new ();
}

MediaPlayer::TimeCounter::~TimeCounter()
{
    g_free(timer);
}

void MediaPlayer::TimeCounter::tic ()
{
    const double dt = g_timer_elapsed (timer, NULL) * 1000.0;

    // ignore refresh after too little time
    if (dt > 3.0){
        // restart timer
        g_timer_start(timer);
        // calculate instantaneous framerate
        // Exponential moving averate with previous framerate to filter jitter
        fps = CLAMP( 0.5 * fps + 500.0 / dt, 0.0, 1000.0);
    }
}

