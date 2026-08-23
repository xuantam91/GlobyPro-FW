#ifndef ESP32_RADIO_H
#define ESP32_RADIO_H

/**
 * @file esp32_radio.h
 * @brief Internet radio player – inherits AudioStreamPlayer.
 *
 * Supports both AAC and MP3 streams (auto-detect or manual).
 * Manages a list of preset Vietnamese VOV radio stations.
 */

#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "audio_stream_player.h"
#include "radio.h"

/* ------------------------------------------------------------------ */
/*  Macros                                                            */
/* ------------------------------------------------------------------ */

/** Default volume amplification for radio stations */
#define RADIO_DEFAULT_VOLUME    4.5f

/* ------------------------------------------------------------------ */
/*  RadioStation                                                      */
/* ------------------------------------------------------------------ */

struct RadioStation {
    std::string name;
    std::string url;
    std::string description;
    std::string genre;
    float       volume;           ///< Amplification factor (1.0 = 100%)

    RadioStation() : volume(RADIO_DEFAULT_VOLUME) {}
    RadioStation(const std::string& n, const std::string& u,
                 const std::string& d = "", const std::string& g = "",
                 float v = RADIO_DEFAULT_VOLUME)
        : name(n), url(u), description(d), genre(g), volume(v) {}
};

/* ------------------------------------------------------------------ */
/*  Esp32Radio                                                        */
/* ------------------------------------------------------------------ */

class Esp32Radio : public Radio, public AudioStreamPlayer {
public:
    Esp32Radio();
    virtual ~Esp32Radio();

    /**
     * @brief Initialize the radio player.
     * @param codec  AudioCodec for direct PCM output (nullptr = use Application pipeline)
     */
    void Initialize(AudioCodec* codec);

    /* ---- Radio interface ---- */
    bool PlayStation(const std::string& station_name) override;
    bool PlayUrl(const std::string& radio_url, const std::string& station_name = "") override;
    bool Stop() override;
    bool NextStation();
    bool PrevStation();

    std::vector<std::string> GetStationList() const override;

    bool IsPlaying() const override        { return AudioStreamPlayer::IsPlaying(); }
    std::string GetCurrentStation() const override { return current_station_name_; }

    size_t GetBufferSize() const override  { return AudioStreamPlayer::GetBufferSize(); }
    bool IsDownloading() const override    { return AudioStreamPlayer::IsDownloading(); }

    /* Display mode */
    using AudioStreamPlayer::DisplayMode;
    void SetDisplayMode(DisplayMode mode) { AudioStreamPlayer::SetDisplayMode(mode); }
    DisplayMode GetDisplayMode() const    { return AudioStreamPlayer::GetDisplayMode(); }

protected:
    void SourceDataLoop(const std::string& source) override;

    /* ---- AudioStreamPlayer hooks ---- */
    void OnStreamInfoReady(int sample_rate, int bits_per_sample, int channels, int bitrate, int frame_size) override;
    void OnDisplayReady() override;
    bool OnPlaybackFinishedAndContinue() override;

private:
    static void ReconnectTaskEntry(void* param);
    bool BeginStationSwitch();
    void EndStationSwitch();

    void InitializeRadioStations();
    bool IsM3u8Url(const std::string& url) const;
    bool IsTsUrl(const std::string& url) const;
    bool DownloadText(const std::string& url, std::string& out) const;
    bool StreamBinaryUrlToBuffer(const std::string& url);
    bool StreamTsSegmentAsAdts(const std::string& url);
    std::string ResolveUrl(const std::string& base_url, const std::string& ref) const;
    std::string PickM3u8Target(const std::string& m3u8_url, const std::string& body) const;

    /**
     * Detect decoder type from URL heuristic.
     * Defaults to AAC for VOV streams, MP3 otherwise.
     */
    AudioDecoderType GuessDecoderType(const std::string& url) const;

    /* ---- State ---- */
    std::string current_station_name_;
    std::string current_station_url_;
    std::string current_station_key_;
    bool        station_name_displayed_;
    float       current_station_volume_;

    std::map<std::string, RadioStation> radio_stations_;
    std::vector<std::string> station_order_;
    SemaphoreHandle_t station_switch_mutex_ = nullptr;
    TickType_t last_switch_tick_ = 0;
    std::atomic<bool> reconnect_pending_{false};
    std::atomic<uint32_t> switch_generation_{0};
    bool last_playurl_busy_ = false;
};

#endif // ESP32_RADIO_H
