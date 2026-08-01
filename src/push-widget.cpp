#include "pch.h"
#include "helpers.h"
#include <algorithm>
#include <deque>
#include <regex>
#include <optional>
#include <tuple>
#include "push-widget.h"
#include "edit-widget.h"
#include "output-config.h"
#include "protocols.h"

#include "obs.hpp"

namespace
{
using StatsClock = std::chrono::steady_clock;

struct OutputSample
{
    StatsClock::time_point time;
    uint64_t bytes = 0;
    uint64_t frames = 0;
    uint64_t dropped = 0;
};

struct OutputStatsSnapshot
{
    double bitrate_bps = 0;
    double fps = 0;
    double recent_loss_percent = 0;
    double total_loss_percent = 0;
    double congestion_percent = 0;
    uint64_t total_frames = 0;
    uint64_t dropped_frames = 0;
    bool counters_reset = false;
};

class OutputStatsTracker
{
public:
    void Reset(StatsClock::time_point now, uint64_t bytes = 0, uint64_t frames = 0, uint64_t dropped = 0)
    {
        samples_.clear();
        samples_.push_back({now, bytes, frames, dropped});
        last_time_ = now;
        last_frames_ = frames;
        initialized_ = true;
    }

    OutputStatsSnapshot Update(StatsClock::time_point now, uint64_t bytes, uint64_t frames, uint64_t dropped,
        double congestion)
    {
        OutputStatsSnapshot result;
        result.total_frames = frames;
        result.dropped_frames = dropped;
        result.total_loss_percent = frames ? static_cast<double>(dropped) / static_cast<double>(frames) * 100.0 : 0.0;
        result.congestion_percent = std::max(0.0, std::min(100.0, congestion * 100.0));

        if (!initialized_) {
            Reset(now, bytes, frames, dropped);
            return result;
        }

        const auto& previous = samples_.back();
        if (bytes < previous.bytes || frames < previous.frames || dropped < previous.dropped) {
            Reset(now, bytes, frames, dropped);
            result.counters_reset = true;
            return result;
        }

        const auto update_interval =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - last_time_).count();
        if (update_interval > 0)
            result.fps = static_cast<double>(frames - last_frames_) / update_interval;

        samples_.push_back({now, bytes, frames, dropped});

        const auto window_start = now - std::chrono::seconds(10);
        while (samples_.size() > 1 && samples_[1].time <= window_start)
            samples_.pop_front();

        auto oldest_time = samples_.front().time;
        auto oldest_bytes = static_cast<double>(samples_.front().bytes);
        auto oldest_frames = static_cast<double>(samples_.front().frames);
        auto oldest_dropped = static_cast<double>(samples_.front().dropped);

        if (samples_.size() > 1 && oldest_time < window_start) {
            const auto& next = samples_[1];
            const auto sample_span =
                std::chrono::duration_cast<std::chrono::duration<double>>(next.time - oldest_time).count();

            if (sample_span > 0) {
                const auto interpolation_span =
                    std::chrono::duration_cast<std::chrono::duration<double>>(window_start - oldest_time).count();
                const auto ratio = interpolation_span / sample_span;
                oldest_bytes += (static_cast<double>(next.bytes) - oldest_bytes) * ratio;
                oldest_frames += (static_cast<double>(next.frames) - oldest_frames) * ratio;
                oldest_dropped += (static_cast<double>(next.dropped) - oldest_dropped) * ratio;
                oldest_time = window_start;
            }
        }

        const auto window_interval =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - oldest_time).count();
        if (window_interval > 0 && static_cast<double>(bytes) >= oldest_bytes)
            result.bitrate_bps = (static_cast<double>(bytes) - oldest_bytes) * 8.0 / window_interval;

        const auto window_frames = static_cast<double>(frames) - oldest_frames;
        const auto window_dropped = static_cast<double>(dropped) - oldest_dropped;
        if (window_frames > 0 && window_dropped >= 0)
            result.recent_loss_percent = window_dropped / window_frames * 100.0;

        last_time_ = now;
        last_frames_ = frames;
        return result;
    }

private:
    std::deque<OutputSample> samples_;
    StatsClock::time_point last_time_;
    uint64_t last_frames_ = 0;
    bool initialized_ = false;
};

std::string FormatBitrate(double bps)
{
    static const char* units[] = {
        "bps", "Kbps", "Mbps", "Gbps", "Tbps", "Pbps", "Ebps", "Zbps", "Ybps"
    };

    if (bps <= 0)
        return "0 bps";

    const int unit_max_index = sizeof(units) / sizeof(*units);
    auto unit_index = static_cast<int>(log10(bps) / 3);
    if (unit_index >= unit_max_index)
        unit_index = unit_max_index - 1;

    auto value = std::to_string(bps / pow(1000, unit_index)).substr(0, 4);
    if (!value.empty() && value.back() == '.')
        value.pop_back();
    return value + " " + units[unit_index];
}

QString FormatNetworkStats(const OutputStatsSnapshot& stats)
{
    QStringList parts;
    if (stats.recent_loss_percent < 0.05 && stats.total_loss_percent < 0.05) {
        parts << QString::fromUtf8(obs_module_text("Stats.Network.NoLoss"));
    } else {
        parts << QString::fromUtf8(obs_module_text("Stats.Network.Recent"))
                     .arg(QString::number(stats.recent_loss_percent, 'f', 1));
        if (stats.total_loss_percent >= 0.05) {
            parts << QString::fromUtf8(obs_module_text("Stats.Network.Total"))
                         .arg(QString::number(stats.total_loss_percent, 'f', 1));
        }
    }

    if (stats.congestion_percent >= 1.0) {
        parts << QString::fromUtf8(obs_module_text("Stats.Network.Congestion"))
                     .arg(QString::number(stats.congestion_percent, 'f', 0));
    }
    return parts.join(QStringLiteral(" · "));
}

QString FormatNetworkStatsTooltip(const OutputStatsSnapshot& stats)
{
    return QString::fromUtf8(obs_module_text("Stats.Network.Tooltip"))
        .arg(QString::number(stats.recent_loss_percent, 'f', 1))
        .arg(QString::number(stats.dropped_frames))
        .arg(QString::number(stats.total_frames))
        .arg(QString::number(stats.total_loss_percent, 'f', 1))
        .arg(QString::number(stats.congestion_percent, 'f', 0));
}

enum class StreamHealth
{
    Inactive,
    Healthy,
    Warning,
    Error,
};

void SetStatusDot(QLabel* label, StreamHealth health)
{
    label->setText(QString(QChar(0x25CF)));
    switch (health) {
    case StreamHealth::Healthy:
        label->setStyleSheet("color: #49c66a;");
        break;
    case StreamHealth::Warning:
        label->setStyleSheet("color: #ffb020;");
        break;
    case StreamHealth::Error:
        label->setStyleSheet("color: #ff4d4d;");
        break;
    default:
        label->setStyleSheet("color: #777777;");
        break;
    }
}

StreamHealth UpdateNetworkStatsLabel(QLabel* label, const OutputStatsSnapshot& stats)
{
    label->setText(FormatNetworkStats(stats));
    label->setToolTip(FormatNetworkStatsTooltip(stats));

    if (stats.recent_loss_percent > 5.0 || stats.congestion_percent >= 80.0) {
        label->setStyleSheet("color: #ff4d4d;");
        return StreamHealth::Error;
    } else if (stats.recent_loss_percent > 1.0 || stats.congestion_percent >= 50.0) {
        label->setStyleSheet("color: #ffb020;");
        return StreamHealth::Warning;
    } else {
        label->setStyleSheet("");
        return StreamHealth::Healthy;
    }
}
}

class IOBSOutputEventHanlder
{
public:
    virtual void OnStarting() {}
    static void OnOutputStarting(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStarting();
    }

    virtual void OnStarted() {}
    static void OnOutputStarted(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStarted();
    }

    virtual void OnStopping() {}
    static void OnOutputStopping(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStopping();
    }
   
    virtual void OnStopped(int) {}
    static void OnOutputStopped(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStopped(calldata_int(param, "code"));
    }

    virtual void OnReconnect() {}
    static void OnOutputReconnect(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnReconnect();
    }

    virtual void OnReconnected() {}
    static void OnOutputReconnected(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnReconnected();
    }

    virtual void onDeactive() {}
    static void OnOutputDeactive(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->onDeactive();
    }

    void SetMeAsHandler(obs_output_t* output)
    {
        auto outputSignal = obs_output_get_signal_handler(output);
        if (outputSignal)
        {
            signal_handler_connect(outputSignal, "starting", &IOBSOutputEventHanlder::OnOutputStarting, this);
            signal_handler_connect(outputSignal, "start", &IOBSOutputEventHanlder::OnOutputStarted, this);
            signal_handler_connect(outputSignal, "reconnect", &IOBSOutputEventHanlder::OnOutputReconnect, this);
            signal_handler_connect(outputSignal, "reconnect_success", &IOBSOutputEventHanlder::OnOutputReconnected, this);
            signal_handler_connect(outputSignal, "stopping", &IOBSOutputEventHanlder::OnOutputStopping, this);
            signal_handler_connect(outputSignal, "deactivate", &IOBSOutputEventHanlder::OnOutputDeactive, this);
            signal_handler_connect(outputSignal, "stop", &IOBSOutputEventHanlder::OnOutputStopped, this);
        }
    }

    void DisconnectSignals(obs_output_t* output)
    {
        auto outputSignal = obs_output_get_signal_handler(output);
        if (outputSignal)
        {
            signal_handler_disconnect(outputSignal, "starting", &IOBSOutputEventHanlder::OnOutputStarting, this);
            signal_handler_disconnect(outputSignal, "start", &IOBSOutputEventHanlder::OnOutputStarted, this);
            signal_handler_disconnect(outputSignal, "reconnect", &IOBSOutputEventHanlder::OnOutputReconnect, this);
            signal_handler_disconnect(outputSignal, "reconnect_success", &IOBSOutputEventHanlder::OnOutputReconnected, this);
            signal_handler_disconnect(outputSignal, "stopping", &IOBSOutputEventHanlder::OnOutputStopping, this);
            signal_handler_disconnect(outputSignal, "deactivate", &IOBSOutputEventHanlder::OnOutputDeactive, this);
            signal_handler_disconnect(outputSignal, "stop", &IOBSOutputEventHanlder::OnOutputStopped, this);
        }
    }
};


class PushWidgetImpl : public PushWidget, public IOBSOutputEventHanlder
{
    std::string targetid_;
    OutputTargetConfigPtr config_;

    QToolButton* btn_ = 0;
    QLabel* status_dot_ = 0;
    QLabel* name_ = 0;
    QLabel* msg_ = 0;
    QLabel* network_msg_ = 0;

    using clock = StatsClock;
    clock::time_point begin_time_;
    OutputStatsTracker stats_tracker_;
    QTimer* timer_ = 0;

    QAction* edit_action_ = 0;
    QAction* remove_action_ = 0;

    obs_output_t* output_ = 0;
    bool using_main_video_encoder_ = false;
    bool using_main_audio_encoder_ = false;
    obs_view_t* scene_view_ = 0;
    bool isUseDelay_ = false;


    bool PrepareOutputService()
    {
        if (!output_) {
            blog(LOG_ERROR, TAG "Prepare output service before output object is created.");
            return false;
        }
        
        ReleaseOutputService();
        
        auto conf = obs_data_create_from_json(config_->serviceParam.dump().c_str());

        auto protocolInfo = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
        assert(protocolInfo);
        if (!protocolInfo) {
        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
        	return false;
        }
        auto service_id = protocolInfo->serviceId;

        if (!conf)
            return false;
        
        auto service = obs_service_create(service_id, "multi-output-service", conf, nullptr);
        obs_data_release(conf);
        if (!service)
            return false;
        obs_output_set_service(output_, service);

        return true;
    }


    bool ReleaseOutputService()
    {
        if (!output_)
            return true;
        else if (output_ && obs_output_active(output_) == false)
        {
            auto service = obs_output_get_service(output_);
            if (service)
            {
                obs_output_set_service(output_, nullptr);
                obs_service_release(service);
            }
            return true;
        }
        else {
            return false;
        }
    }


    bool PrepareEncoderSource() {
        if (!output_) {
            blog(LOG_ERROR, TAG "Prepare output scene before output object is created.");
            return false;
        }

        if (!using_main_video_encoder_) {
            auto venc = obs_output_get_video_encoder(output_);
            if (!venc) {
                blog(LOG_ERROR, TAG "Prepare output scene before encoder is created.");
                return false;
            }
            auto videoConfig = FindById(GlobalMultiOutputConfig().videoConfig, config_->videoConfig.value_or(""));

            if (!videoConfig || !videoConfig->outputScene.has_value()) {
                obs_encoder_set_video(venc, obs_get_video());
            } else {
                auto sceneName = *videoConfig->outputScene;
                OBSSourceAutoRelease scene = obs_get_source_by_name(sceneName.c_str());
                if (scene == nullptr) {
                    blog(LOG_ERROR, TAG "Output scene is not found.");
                    return false;
                }
                ReleaseOutputSceneView();

                scene_view_ = obs_view_create();
                obs_view_set_source(scene_view_, 0, scene);
                obs_source_inc_active(scene);
                auto scene_video = obs_view_add(scene_view_);
                obs_encoder_set_video(venc, scene_video);
            }
        }

        if (!using_main_audio_encoder_) {
            auto aenc = obs_output_get_audio_encoder(output_, 0);
            if (!aenc) {
                blog(LOG_ERROR, TAG "Prepare output scene before encoder is created.");
                return false;
            }
            obs_encoder_set_audio(aenc, obs_get_audio());

            // Set other audio tracks
            auto audioConfig = FindById(GlobalMultiOutputConfig().audioConfig, config_->audioConfig.value_or(""));
            if (audioConfig) {
                for (auto& track : audioConfig->audioTracks) {
                    auto enc = obs_output_get_audio_encoder(output_, track->output_track);
                    if (enc) {
                        obs_encoder_set_audio(enc, obs_get_audio());
                    }
                }
            }
        }
        
        return true;
    }


    bool ReleaseOutputSceneView() {
        if (!scene_view_)
            return true;

        obs_view_remove(scene_view_);
        OBSSourceAutoRelease source = obs_view_get_source(scene_view_, 0);
        if (source) {
            obs_source_dec_active(source);
        }
        obs_view_set_source(scene_view_, 0, nullptr);
        obs_view_destroy(scene_view_);
        scene_view_ = nullptr;

        return true;
    }


    std::string VideoEncoderName() {
        return "multi-rtmp-venc" + config_->videoConfig.value_or("");
    }

    std::string AudioEncoderName(int track) {
        return "multi-rtmp-aenc" + config_->audioConfig.value_or("") + "-track-idx-" + std::to_string(track);
    }

    std::optional<std::tuple<int, int>> ParseResolution(const std::optional<std::string>& res) {
        if (!res.has_value())
            return std::nullopt;
        std::regex res_pattern(R"__(\s*(\d{1,5})\s*x\s*(\d{1,5})\s*)__");
        std::smatch match;
        if (std::regex_match(*res, match, res_pattern))
        {
            auto width = std::stoi(match[1].str());
            auto height = std::stoi(match[2].str());
            return {{ width, height }};
        }

        return std::nullopt;
    }

    OBSEncoder GetVideoEncoder() {
        auto config_id = config_->videoConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (config_id == "" || config_id == OBS_STREAMING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_streaming_output();
            OBSEncoder enc = obs_output_get_video_encoder(stream_output);
            using_main_video_encoder_ = true;
            return enc.Get();
        } else if (config_id == OBS_RECORDING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_recording_output();
            OBSEncoder enc = obs_output_get_video_encoder(stream_output);
            using_main_video_encoder_ = true;
            return enc.Get();
        } else {
            OBSEncoderAutoRelease enc = obs_get_encoder_by_name(VideoEncoderName().c_str());
            if (!enc) {
                auto& global = GlobalMultiOutputConfig();
                auto videoConfig = FindById(global.videoConfig, config_id);
                if (videoConfig) {
                    OBSDataAutoRelease settings = obs_data_create_from_json(videoConfig->encoderParams.dump().c_str());
                    enc = obs_video_encoder_create(videoConfig->encoderId.c_str(), VideoEncoderName().c_str(), settings, nullptr);
                    if (enc) {
                        auto wh = ParseResolution(videoConfig->resolution);
                        if (wh.has_value()) {
                            obs_encoder_set_gpu_scale_type(enc, obs_scale_type::OBS_SCALE_BICUBIC);
                            auto [w, h] = *wh;
                            obs_encoder_set_scaled_size(enc, w, h);
                        }
                        obs_encoder_set_frame_rate_divisor(enc, videoConfig->fpsDenumerator);
                    }
                } else {
                    assert(false && "No video encoder config found with specified id.");
                    blog(LOG_ERROR, TAG "Load video encoder config failed for %s. Sharing with main output.", config_->name.c_str());
                    config_->videoConfig = OBS_STREAMING_ENC_PLACEHOLDER;
                    return GetVideoEncoder();
                }
            }

            using_main_video_encoder_ = false;
            return enc.Get();
        }
    }

    OBSEncoder GetAudioEncoder(int trackIdx = 0, std::optional<int> mixerId = std::nullopt) {
        auto config_id = config_->audioConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (config_id == "" || config_id == OBS_STREAMING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_streaming_output();
            OBSEncoder enc = obs_output_get_audio_encoder(stream_output, 0);
            using_main_audio_encoder_ = true;
            return enc.Get();
        } else if (config_id == OBS_RECORDING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_recording_output();
            OBSEncoder enc = obs_output_get_audio_encoder(stream_output, 0);
            using_main_audio_encoder_ = true;
            return enc.Get();
        } else {
            OBSEncoderAutoRelease enc = obs_get_encoder_by_name(AudioEncoderName(trackIdx).c_str());
            if (!enc) {
                auto& global = GlobalMultiOutputConfig();
                auto audioConfigId = *config_->audioConfig;
                auto audioConfig = FindById(global.audioConfig, audioConfigId);
                if (audioConfig) {
                    OBSDataAutoRelease settings = obs_data_create_from_json(audioConfig->encoderParams.dump().c_str());

                    // If we were provided with a mixerId, override the audioConfig's mixerId with it
                    int defaultMixerId = audioConfig->mixerId;
                    if (auto overrideMixerId = mixerId) {
                        defaultMixerId = *overrideMixerId;
                    }

                    enc = obs_audio_encoder_create(audioConfig->encoderId.c_str(), AudioEncoderName(trackIdx).c_str(), settings, defaultMixerId, nullptr);
                } else {
                    assert(false && "No audio encoder config found with specified id.");
                    blog(LOG_ERROR, TAG "Load audio encoder config failed for %s. Sharing with main output.", config_->name.c_str());
                    config_->audioConfig = OBS_STREAMING_ENC_PLACEHOLDER;
                    return GetAudioEncoder();
                }
            }
            
            using_main_audio_encoder_ = false;
            return enc.Get();
        }
    }

    bool PrepareOutputEncoders()
    {
        if (!output_) {
            blog(LOG_ERROR, TAG "Prepare output encoder before output object is created.");
            return false;
        }
        
        ReleaseOutputEncoder();

        auto& global = GlobalMultiOutputConfig();

        // main output
        OBSOutput mainOutput = obs_frontend_get_streaming_output();
        OBSOutput recordingOutput =  obs_frontend_get_recording_output();

        obs_output_release(mainOutput);
        obs_output_release(recordingOutput);

        OBSEncoder venc = GetVideoEncoder();
        OBSEncoder aenc = GetAudioEncoder();

        std::vector<std::tuple<int, OBSEncoder>> additionalTracks;
        if (auto audioConfigId = config_->audioConfig) {
            auto audioConfig = FindById(global.audioConfig, *audioConfigId);

            if (!audioConfig) {
                blog(LOG_ERROR, TAG "Load audio encoder config failed for %s. Could not determine additional tracks.", config_->name.c_str());
            } else {
                additionalTracks.reserve(audioConfig->audioTracks.size());
                for (auto& track : audioConfig->audioTracks) {
                    OBSEncoder enc = GetAudioEncoder(track->output_track, track->mixer_track);
                    if (enc) {
                        // Record the output track index and the encoder for later when we set the encoders on the output
                        additionalTracks.push_back({ track->output_track, enc });
                    }
                }
            }
        }

        if (!aenc || !venc) {
            // If we don't have a valid encoder, we're likely using a special encoder type that
            // needs to be started by the user (i.e. start streaming or start recording)
            ReleaseOutputEncoder();

            auto msgbox = new QMessageBox(QMessageBox::Icon::Critical, 
                obs_module_text("Notice.Title"), 
                obs_module_text("Notice.GetEncoder"),
                QMessageBox::StandardButton::Ok,
                this
                );
            msgbox->exec();
            return false;
        }

        obs_output_set_audio_encoder(output_, obs_encoder_get_ref(aenc), 0);
        for (auto& track : additionalTracks) {
            auto trackIdx = std::get<0>(track);
            auto enc = std::get<1>(track);
            obs_output_set_audio_encoder(output_, obs_encoder_get_ref(enc), trackIdx);
        }
        obs_output_set_video_encoder(output_, obs_encoder_get_ref(venc));

        return true;
    }


    bool ReleaseOutputEncoder()
    {
        if (!output_)
            return true;
        else if (obs_output_active(output_) == false)
        {
            auto venc = obs_output_get_video_encoder(output_);
            if (venc)
            {
                obs_output_set_video_encoder(output_, nullptr);
                obs_encoder_release(venc);
            }
            
            auto aenc = obs_output_get_audio_encoder(output_, 0);
            if (aenc)
            {
                obs_output_set_audio_encoder(output_, nullptr, 0);
                obs_encoder_release(aenc);
            }

            return true;
        }
        else {
            blog(LOG_ERROR, TAG "Release output while it is active.");
            return false;
        }
    }


    bool ReleaseOutput()
    {
        if (output_) {
            DisconnectSignals(output_);
        }

        if (output_ && obs_output_active(output_)) {
            obs_output_force_stop(output_);
        }

        if (output_ && obs_output_active(output_) == false)
        {
            bool ret = ReleaseOutputService();
            ret = ReleaseOutputEncoder() && ret;

            obs_output_release(output_);
            output_ = nullptr;

            ret = ReleaseOutputSceneView() && ret;

            return ret;
        }
        else if (output_) {
            obs_output_release(output_);
            output_ = nullptr;

            return true;
        }
        else if (output_ == nullptr)
            return true;
        else
            return false;
    }


    void UpdateStreamStatus() {
        using namespace std::chrono;

        if (!output_)
            return;

        auto new_bytes = obs_output_get_total_bytes(output_);
        auto new_frames = obs_output_get_total_frames(output_);
        auto new_dropped = static_cast<uint64_t>(std::max(0, obs_output_get_frames_dropped(output_)));
        auto now = clock::now();
        auto stats = stats_tracker_.Update(
            now, new_bytes, new_frames, new_dropped, obs_output_get_congestion(output_));

        auto duration = now - begin_time_;
        auto hh = duration_cast<hours>(duration);
        duration -= hh;
        auto mm = duration_cast<minutes>(duration);
        duration -= mm;
        auto ss = duration_cast<seconds>(duration);

        char str_duration[64] = { 0 };
        snprintf(str_duration, sizeof(str_duration), "%02d:%02d:%02d", (int)hh.count(), (int)mm.count(),
            (int)ss.count());

        char str_fps[32] = { 0 };
        snprintf(str_fps, sizeof(str_fps), "%d FPS", static_cast<int>(std::round(stats.fps)));

        msg_->setText((std::string(str_duration) + "  " + FormatBitrate(stats.bitrate_bps) + "  " + str_fps).c_str());
        SetStatusDot(status_dot_, UpdateNetworkStatsLabel(network_msg_, stats));
    }

public:
    PushWidgetImpl(const std::string& targetid, QWidget* parent = 0)
        : QWidget(parent)
        , targetid_(targetid)
    {
        QObject::setObjectName("push-widget");

        auto& global = GlobalMultiOutputConfig();
        config_ = FindById(global.targets, targetid_);
        if (!config_)
            return;

        timer_ = new QTimer(this);
        timer_->setInterval(std::chrono::milliseconds(1000));
        QObject::connect(timer_, &QTimer::timeout, [this]() {
            UpdateStreamStatus();
        });

        auto layout = new QGridLayout(this);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setHorizontalSpacing(4);
        layout->setVerticalSpacing(2);
        layout->setColumnStretch(2, 1);

        layout->addWidget(status_dot_ = new QLabel(this), 0, 0);
        status_dot_->setFixedWidth(12);
        SetStatusDot(status_dot_, StreamHealth::Inactive);

        layout->addWidget(name_ = new QLabel(obs_module_text("NewStreaming"), this), 0, 1);
        name_->setStyleSheet("font-weight: bold;");

        layout->addWidget(msg_ = new QLabel(u8"", this), 0, 2);
        msg_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        layout->addWidget(btn_ = new QToolButton(this), 0, 3);
        btn_->setText(QString(QChar(0x25B6)));
        btn_->setToolTip(obs_module_text("Btn.Start"));
        btn_->setAccessibleName(obs_module_text("Btn.Start"));
        btn_->setFixedWidth(30);
        QObject::connect(btn_, &QToolButton::clicked, [this]() {
            StartStop();
        });

        auto menuButton = new QToolButton(this);
        menuButton->setText(QString(QChar(0x22EF)));
        menuButton->setAccessibleName(obs_module_text("Btn.More"));
        menuButton->setFixedWidth(30);
        menuButton->setPopupMode(QToolButton::InstantPopup);
        auto menu = new QMenu(menuButton);
        edit_action_ = menu->addAction(obs_module_text("Btn.Edit"));
        remove_action_ = menu->addAction(obs_module_text("Btn.Delete"));
        menuButton->setMenu(menu);
        layout->addWidget(menuButton, 0, 4);

        QObject::connect(edit_action_, &QAction::triggered, [this]() {
            ShowEditDlg();
        });

        QObject::connect(remove_action_, &QAction::triggered, [this]() {
            auto msgbox = new QMessageBox(QMessageBox::Icon::Question,
                obs_module_text("Question.Title"),
                obs_module_text("Question.Delete"),
                QMessageBox::Yes | QMessageBox::No,
                this
            );
            if (msgbox->exec() == QMessageBox::Yes) {
                GetGlobalService().RunInUIThread([this]() {
                    auto& global = GlobalMultiOutputConfig();
                    auto it = std::find_if(global.targets.begin(), global.targets.end(), [&](auto& x) { return x->id == targetid_; });
                    if (it != global.targets.end()) {
                        global.targets.erase(it);
                    }
                    delete this;
                    SaveMultiOutputConfig();
                });
            }
        });

        layout->addWidget(network_msg_ = new QLabel(u8"", this), 1, 1, 1, 4);
        layout->addItem(new QSpacerItem(0, 3), 2, 0);
        setLayout(layout);

        LoadConfig();
    }
    
    ~PushWidgetImpl()
    {
        ReleaseOutput();
    }


    void StartStreaming() override {
        if (IsRunning())
            return;
        
        // recreate output
        ReleaseOutput();

        if (output_ == nullptr)
        {
            obs_data* output_settings = obs_data_create_from_json(config_->outputParam.dump().c_str());
            
            auto protocolInfo = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
            assert(protocolInfo);
            if (!protocolInfo) {
	        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
	        	protocolInfo = GetProtocolInfos()->GetList();
	        }
            auto output_id = protocolInfo->outputId;

            blog(LOG_DEBUG, "Streaming to output: %s", output_id);

            output_ = obs_output_create(output_id, "multi-output", output_settings, nullptr);
            SetMeAsHandler(output_);
        }

        if (output_) {
            isUseDelay_ = false;

            auto profileConfig = obs_frontend_get_profile_config();
            if (profileConfig) {
                bool useDelay = config_get_bool(profileConfig, "Output", "DelayEnable");
                bool preserveDelay = config_get_bool(profileConfig, "Output", "DelayPreserve");
                int delaySec = config_get_int(profileConfig, "Output", "DelaySec");
                obs_output_set_delay(output_,
                    useDelay ? delaySec : 0,
                    preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0
                );

                if (useDelay && delaySec > 0)
                    isUseDelay_ = true;
            }
        }

        if (!PrepareOutputService())
        {
            SetMsg(obs_module_text("Error.CreateRtmpService"));
            return;
        }

        if (!PrepareOutputEncoders())
        {
            SetMsg(obs_module_text("Error.CreateEncoder"));
            return;
        }

        if (!PrepareEncoderSource())
        {
            SetMsg(obs_module_text("Error.SceneNotExist"));
            return;
        }

        if (!obs_output_start(output_))
        {
            SetMsg(obs_module_text("Error.StartOutput"));
        }
    }

    void StopStreaming() override {
        if (!IsRunning())
            return;
        
        bool useForce = false;
        if (isUseDelay_) {
            auto res = QMessageBox(QMessageBox::Icon::Information,
                "?",
                obs_module_text("Ques.DropDelay"),
                QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No,
                this
            ).exec();
            if (res == QMessageBox::Yes)
                useForce = true;
        }

        if (!useForce)
            obs_output_stop(output_);
        else
            obs_output_force_stop(output_);
    }
   
    void OnOBSEvent(obs_frontend_event ev) override
    {
        if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_EXIT
            || ev == obs_frontend_event::OBS_FRONTEND_EVENT_PROFILE_CHANGED
            || ev == obs_frontend_event::OBS_FRONTEND_EVENT_PROFILE_LIST_CHANGED
        ) {
            Stop();
        } else if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_STREAMING_STARTING) {
            if (!IsRunning() && config_->syncStart) {
                StartStop();
            }
        } else if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_STREAMING_STOPPING) {
            if (IsRunning() && config_->syncStop) {
                StartStop();
            }
        }
    }

    void LoadConfig()
    {
        name_->setText(QString::fromUtf8(config_->name));
    }

    void ResetInfo()
    {
        const auto bytes = output_ ? obs_output_get_total_bytes(output_) : 0;
        const auto frames = output_ ? obs_output_get_total_frames(output_) : 0;
        const auto dropped = output_
            ? static_cast<uint64_t>(std::max(0, obs_output_get_frames_dropped(output_)))
            : 0;
        stats_tracker_.Reset(clock::now(), bytes, frames, dropped);
        msg_->setText("");
        network_msg_->setText("");
        network_msg_->setStyleSheet("");
    }

    bool IsRunning()
    {
        if (output_ == nullptr)
            return false;
        if (output_ != nullptr && obs_output_active(output_) == false)
            return false;
        if (output_ != nullptr && obs_output_active(output_) == true)
            return true;
        assert(false);
        return false;
    }

    void StartStop()
    {
        if (IsRunning() == false)
        {
            StartStreaming();
        }
        else if (output_ != nullptr)
        {
            StopStreaming();
        }
    }

    void Stop()
    {
        if (IsRunning())
        {
            obs_output_force_stop(output_);
        }
    }

    bool ShowEditDlg() override
    {
        std::unique_ptr<EditOutputWidget> dlg{ createEditOutputWidget(targetid_, (QMainWindow*)obs_frontend_get_main_window()) };

        if (dlg->exec() == QDialog::DialogCode::Accepted)
        {
            SaveMultiOutputConfig();
            LoadConfig();
            return true;
        }
        else
            return false;
    }

    void SetMsg(QString msg)
    {
        msg_->setText(msg);
        msg_->setToolTip(msg);
    }

    void SetPrimaryAction(bool running)
    {
        const auto text = QString(QChar(running ? 0x25A0 : 0x25B6));
        const auto tooltip = obs_module_text(running ? "Status.Stop" : "Btn.Start");
        btn_->setText(text);
        btn_->setToolTip(tooltip);
        btn_->setAccessibleName(tooltip);
        btn_->setEnabled(true);
    }

    // obs logical
    void OnStarting() override
    {
        GetGlobalService().RunInUIThread([this]() {
            begin_time_ = clock::now();
            remove_action_->setEnabled(false);
            SetPrimaryAction(true);
            SetStatusDot(status_dot_, StreamHealth::Warning);
            SetMsg(obs_module_text("Status.Connecting"));
        });
    }

    void OnStarted() override
    {
        GetGlobalService().RunInUIThread([this]() {
            remove_action_->setEnabled(false);
            SetPrimaryAction(true);
            SetStatusDot(status_dot_, StreamHealth::Healthy);
            SetMsg(obs_module_text("Status.Streaming"));

            ResetInfo();
            timer_->start();
        });
    }

    void OnReconnect() override
    {
        GetGlobalService().RunInUIThread([this]() {
            timer_->stop();

            remove_action_->setEnabled(false);
            SetPrimaryAction(true);
            SetStatusDot(status_dot_, StreamHealth::Warning);
            SetMsg(obs_module_text("Status.Reconnecting"));
        });
    }

    void OnReconnected() override
    {
        GetGlobalService().RunInUIThread([this]() {
            remove_action_->setEnabled(false);
            SetPrimaryAction(true);
            SetStatusDot(status_dot_, StreamHealth::Healthy);
            SetMsg(obs_module_text("Status.Streaming"));

            ResetInfo();
            timer_->start();
        });
    }

    void OnStopping() override
    {
        GetGlobalService().RunInUIThread([this]() {
            timer_->stop();

            remove_action_->setEnabled(false);
            SetPrimaryAction(true);
            SetStatusDot(status_dot_, StreamHealth::Warning);
            SetMsg(obs_module_text("Status.Stopping"));
        });
    }

    void OnStopped(int code) override
    {
        GetGlobalService().RunInUIThread([this, code]() {
            ResetInfo();
            timer_->stop();

            remove_action_->setEnabled(true);
            SetPrimaryAction(false);
            SetStatusDot(status_dot_, code == 0 ? StreamHealth::Inactive : StreamHealth::Error);
            SetMsg(u8"");

            switch(code)
            {
                case 0:
                    SetMsg(u8"");
                    break;
                case -1:
                    SetMsg(obs_module_text("Error.WrongRTMPUrl"));
                    break;
                case -2:
                    SetMsg(obs_module_text("Error.ServerConnect"));
                    break;
                case -3:
                    SetMsg(obs_module_text("Error.ServerHandshake"));
                    break;
                case -4:
                    SetMsg(obs_module_text("Error.ServerRefuse"));
                    break;
                default:
                    SetMsg(obs_module_text("Error.Unknown"));
                    break;
            }
        });

        ReleaseOutputEncoder();
        ReleaseOutputSceneView();
    }
};

class MainOutputStatsWidgetImpl : public MainOutputStatsWidget
{
public:
    explicit MainOutputStatsWidgetImpl(QWidget* parent = 0)
        : QWidget(parent)
    {
        setObjectName("main-output-stats-widget");

        auto layout = new QGridLayout(this);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setHorizontalSpacing(4);
        layout->setVerticalSpacing(2);
        layout->setColumnStretch(2, 1);

        layout->addWidget(status_dot_ = new QLabel(this), 0, 0);
        status_dot_->setFixedWidth(12);
        SetStatusDot(status_dot_, StreamHealth::Inactive);

        auto name = new QLabel(QString::fromUtf8(obs_module_text("Stats.MainOutput")), this);
        name->setStyleSheet("font-weight: bold;");
        layout->addWidget(name, 0, 1);

        layout->addWidget(msg_ = new QLabel(QString::fromUtf8(obs_module_text("Stats.Inactive")), this), 0, 2);
        msg_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto badge = new QLabel(QString::fromUtf8(obs_module_text("Stats.MainBadge")), this);
        badge->setStyleSheet("color: #888888; font-size: 10px;");
        layout->addWidget(badge, 0, 3);

        layout->addWidget(network_msg_ = new QLabel(u8"", this), 1, 1, 1, 3);
        layout->addItem(new QSpacerItem(0, 3), 2, 0);
        setLayout(layout);

        timer_ = new QTimer(this);
        timer_->setInterval(std::chrono::milliseconds(1000));
        QObject::connect(timer_, &QTimer::timeout, [this]() {
            UpdateStreamStatus();
        });
    }

    void OnOBSEvent(obs_frontend_event ev) override
    {
        if (ev == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
            timer_->start();
            UpdateStreamStatus();
        } else if (ev == OBS_FRONTEND_EVENT_EXIT) {
            timer_->stop();
        }
    }

private:
    void ResetInactive()
    {
        active_ = false;
        observed_output_ = nullptr;
        stats_tracker_.Reset(StatsClock::now());
        msg_->setText(QString::fromUtf8(obs_module_text("Stats.Inactive")));
        network_msg_->setText("");
        network_msg_->setStyleSheet("");
        SetStatusDot(status_dot_, StreamHealth::Inactive);
    }

    void UpdateStreamStatus()
    {
        using namespace std::chrono;

        OBSOutputAutoRelease output = obs_frontend_get_streaming_output();
        if (!output || !obs_output_active(output)) {
            if (active_)
                ResetInactive();
            return;
        }

        const auto now = StatsClock::now();
        const auto bytes = obs_output_get_total_bytes(output);
        const auto frames = obs_output_get_total_frames(output);
        const auto dropped = static_cast<uint64_t>(std::max(0, obs_output_get_frames_dropped(output)));

        if (!active_ || observed_output_ != output) {
            active_ = true;
            observed_output_ = output;
            begin_time_ = now;
            stats_tracker_.Reset(now, bytes, frames, dropped);
            msg_->setText(QString::fromUtf8(obs_module_text("Status.Streaming")));
            OutputStatsSnapshot initial_stats;
            initial_stats.total_frames = frames;
            initial_stats.dropped_frames = dropped;
            initial_stats.total_loss_percent =
                frames ? static_cast<double>(dropped) / static_cast<double>(frames) * 100.0 : 0.0;
            initial_stats.congestion_percent = std::max(
                0.0, std::min(100.0, static_cast<double>(obs_output_get_congestion(output)) * 100.0));
            SetStatusDot(status_dot_, UpdateNetworkStatsLabel(network_msg_, initial_stats));
            return;
        }

        auto stats = stats_tracker_.Update(now, bytes, frames, dropped, obs_output_get_congestion(output));
        if (stats.counters_reset)
            begin_time_ = now;

        auto duration = now - begin_time_;
        auto hh = duration_cast<hours>(duration);
        duration -= hh;
        auto mm = duration_cast<minutes>(duration);
        duration -= mm;
        auto ss = duration_cast<seconds>(duration);

        char str_duration[64] = { 0 };
        snprintf(str_duration, sizeof(str_duration), "%02d:%02d:%02d", (int)hh.count(), (int)mm.count(),
            (int)ss.count());
        char str_fps[32] = { 0 };
        snprintf(str_fps, sizeof(str_fps), "%d FPS", static_cast<int>(std::round(stats.fps)));

        msg_->setText((std::string(str_duration) + "  " + FormatBitrate(stats.bitrate_bps) + "  " + str_fps).c_str());
        SetStatusDot(status_dot_, UpdateNetworkStatsLabel(network_msg_, stats));
    }

    QLabel* status_dot_ = 0;
    QLabel* msg_ = 0;
    QLabel* network_msg_ = 0;
    QTimer* timer_ = 0;
    OutputStatsTracker stats_tracker_;
    StatsClock::time_point begin_time_;
    obs_output_t* observed_output_ = nullptr;
    bool active_ = false;
};

PushWidget* createPushWidget(const std::string& targetid, QWidget* parent) {
    return new PushWidgetImpl(targetid, parent);
}

MainOutputStatsWidget* createMainOutputStatsWidget(QWidget* parent) {
    return new MainOutputStatsWidgetImpl(parent);
}
