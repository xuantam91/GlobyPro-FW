/**
 * @file esp32_radio.cc
 * @brief Internet radio player  supports AAC and MP3 streams.
 *
 * Inherits AudioStreamPlayer for streaming, decoding and playback.
 * Adds: preset station list, station search, volume per station,
 *       decoder type auto-detection.
 */

#include "esp32_radio.h"
#include "board.h"
#include "display.h"
#include "audio/audio_codec.h"

#include <esp_log.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <cstdlib>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "Esp32Radio";

namespace {
int ExtractTargetDurationMs(const std::string& m3u8_body)
{
    const char* key = "#EXT-X-TARGETDURATION:";
    size_t pos = m3u8_body.find(key);
    if (pos == std::string::npos) {
        return 2000;
    }
    pos += strlen(key);
    while (pos < m3u8_body.size() && (m3u8_body[pos] == ' ' || m3u8_body[pos] == '\t')) {
        ++pos;
    }
    size_t end = pos;
    while (end < m3u8_body.size() && isdigit((unsigned char)m3u8_body[end])) {
        ++end;
    }
    if (end <= pos) {
        return 2000;
    }
    int sec = atoi(m3u8_body.substr(pos, end - pos).c_str());
    if (sec <= 0) sec = 2;
    if (sec > 12) sec = 12;
    return sec * 1000;
}
} // namespace

/* ================================================================== */
/*  Constructor / Destructor                                          */
/* ================================================================== */

Esp32Radio::Esp32Radio()
    : AudioStreamPlayer(),
      current_station_key_(),
      station_name_displayed_(false),
      current_station_volume_(RADIO_DEFAULT_VOLUME)
{
    station_switch_mutex_ = xSemaphoreCreateMutex();
}

Esp32Radio::~Esp32Radio()
{
    ESP_LOGI(TAG, "Destroying Esp32Radio");
    Stop();
    if (station_switch_mutex_) {
        vSemaphoreDelete(station_switch_mutex_);
        station_switch_mutex_ = nullptr;
    }
    ESP_LOGI(TAG, "Esp32Radio destroyed");
}

bool Esp32Radio::BeginStationSwitch()
{
    if (!station_switch_mutex_) {
        return true;
    }
    if (xSemaphoreTake(station_switch_mutex_, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Skip switch: another station switch is in progress");
        return false;
    }
    const TickType_t now = xTaskGetTickCount();
    if ((now - last_switch_tick_) < pdMS_TO_TICKS(160)) {
        xSemaphoreGive(station_switch_mutex_);
        ESP_LOGW(TAG, "Skip switch: debounced");
        return false;
    }
    last_switch_tick_ = now;
    return true;
}

void Esp32Radio::EndStationSwitch()
{
    if (station_switch_mutex_) {
        xSemaphoreGive(station_switch_mutex_);
    }
}

void Esp32Radio::Initialize(AudioCodec* codec)
{
    if (codec) {
        SetAudioCodec(codec);
    }
    InitializeRadioStations();
    ESP_LOGI(TAG, "Radio player initialised with %d stations (codec=%s)",
             (int)radio_stations_.size(), codec ? "direct" : "app-pipeline");
}

/* ================================================================== */
/*  Station presets                                                    */
/* ================================================================== */

void Esp32Radio::InitializeRadioStations()
{
    station_order_.clear();
    station_order_.reserve(9);

    // Keep names/description short so AI can list and match quickly.
    // Stable channels first; VOV kept for fallback when upstream recovers.
    auto add_station = [this](const std::string& key, const RadioStation& s) {
        radio_stations_[key] = s;
        station_order_.push_back(key);
    };
    add_station("ABC1",       RadioStation("ABC1 AU News",   "https://abc.streamguys1.com/live/rnnsw/icecast.audio",       "Australia news", "News",    4.0f));
    add_station("ABC2",       RadioStation("ABC2 AU Talk",   "https://abc.streamguys1.com/live/localsydney/icecast.audio", "Australia talk", "Talk",    4.0f));
    add_station("NPR",        RadioStation("NPR US",         "https://npr-ice.streamguys1.com/live.aac",                    "US public radio","News",    4.0f));
    add_station("SOMA1",      RadioStation("SOMA1 Indie",    "https://ice6.somafm.com/indiepop-128-aac",                    "Indie Pop",      "Music",   4.0f));
    add_station("SOMA2",      RadioStation("SOMA2 Chill",    "https://ice2.somafm.com/groovesalad-128-aac",                 "Chill",          "Music",   4.0f));
    add_station("VOV1",       RadioStation("VOV1 News",      "https://audio-lss.vov.vn/han/live/vov1/audio/haudio-eng.m3u8","Tin tuc",        "News",    4.6f));
    add_station("VOV2",       RadioStation("VOV2 Culture",   "https://audio-lss.vov.vn/han/live/vov2/audio/haudio-eng.m3u8","Van hoa",        "Talk",    4.5f));
    add_station("VOV3",       RadioStation("VOV3 Music",     "https://audio-lss.vov.vn/han/live/vov3/audio/haudio-eng.m3u8","Am nhac",        "Music",   4.5f));
    add_station("VOV_GT_HN",  RadioStation("VOV GT Hanoi",   "https://play.vovgiaothong.vn/live/gthn/playlist.m3u8",        "Giao thong HN",  "Traffic", 4.5f));
    
    ESP_LOGI(TAG, "Initialised %d radio stations", (int)radio_stations_.size());
}

/* ================================================================== */
/*  Decoder-type heuristic                                            */
/* ================================================================== */

AudioDecoderType Esp32Radio::GuessDecoderType(const std::string& url) const
{
    /* VOV streams are AAC/AAC+ */
    if (url.find("vovmedia.vn") != std::string::npos || IsM3u8Url(url)) {
        return AudioDecoderType::AAC;
    }

    /* Check common URL patterns */
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find(".aac") != std::string::npos ||
        lower.find("aacp") != std::string::npos ||
        lower.find("aac+") != std::string::npos) {
        return AudioDecoderType::AAC;
    }

    if (lower.find(".mp3") != std::string::npos) {
        return AudioDecoderType::MP3;
    }

    // Some Icecast endpoints use extension-less paths (e.g. /icecast.audio)
    // and commonly carry AAC/AAC+.
    if (lower.find("icecast.audio") != std::string::npos ||
        lower.find("streamguys") != std::string::npos) {
        return AudioDecoderType::AAC;
    }

    /* Default to AAC for current station set (VOV/Soma/NPR/ABC are AAC-family). */
    ESP_LOGW(TAG, "Cannot determine stream type from URL, defaulting to AAC");
    return AudioDecoderType::AAC;
}

bool Esp32Radio::IsM3u8Url(const std::string& url) const
{
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find(".m3u8") != std::string::npos;
}

bool Esp32Radio::IsTsUrl(const std::string& url) const
{
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find(".ts") != std::string::npos;
}

std::string Esp32Radio::ResolveUrl(const std::string& base_url, const std::string& ref) const
{
    if (ref.empty()) {
        return {};
    }
    if (ref.find("http://") == 0 || ref.find("https://") == 0) {
        return ref;
    }
    if (base_url.find("http://") != 0 && base_url.find("https://") != 0) {
        return ref;
    }

    size_t scheme_pos = base_url.find("://");
    if (scheme_pos == std::string::npos) {
        return ref;
    }
    size_t host_start = scheme_pos + 3;
    size_t path_start = base_url.find('/', host_start);
    const std::string origin = (path_start == std::string::npos) ? base_url : base_url.substr(0, path_start);

    if (ref[0] == '/') {
        return origin + ref;
    }

    std::string base_dir = base_url;
    size_t q = base_dir.find('?');
    if (q != std::string::npos) {
        base_dir = base_dir.substr(0, q);
    }
    size_t slash = base_dir.rfind('/');
    if (slash != std::string::npos) {
        base_dir = base_dir.substr(0, slash + 1);
    } else {
        base_dir.push_back('/');
    }
    return base_dir + ref;
}

bool Esp32Radio::DownloadText(const std::string& url, std::string& out) const
{
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("User-Agent", "ESP32-Radio/1.0");
    http->SetHeader("Accept", "*/*");
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "HTTP open failed: %s", url.c_str());
        return false;
    }
    int status = http->GetStatusCode();
    if (status != 200 && status != 206) {
        ESP_LOGE(TAG, "HTTP status=%d for: %s", status, url.c_str());
        http->Close();
        return false;
    }
    out = http->ReadAll();
    http->Close();
    return !out.empty();
}

std::string Esp32Radio::PickM3u8Target(const std::string& m3u8_url, const std::string& body) const
{
    std::string first_variant;
    std::string last_segment;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        std::string line = (end == std::string::npos) ? body.substr(pos) : body.substr(pos, end - pos);
        pos = (end == std::string::npos) ? body.size() : end + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) start++;
        if (start > 0) line = line.substr(start);
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        if (line.find(".m3u8") != std::string::npos) {
            if (first_variant.empty()) {
                first_variant = ResolveUrl(m3u8_url, line);
            }
            continue;
        }
        last_segment = ResolveUrl(m3u8_url, line);
    }

    if (!last_segment.empty()) return last_segment;
    return first_variant;
}

bool Esp32Radio::StreamBinaryUrlToBuffer(const std::string& url)
{
    if (IsTsUrl(url)) {
        return StreamTsSegmentAsAdts(url);
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("User-Agent", "ESP32-Radio/1.0");
    http->SetHeader("Accept", "*/*");

    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Failed to open segment: %s", url.c_str());
        return false;
    }
    int status = http->GetStatusCode();
    if (status != 200 && status != 206) {
        ESP_LOGW(TAG, "Segment status=%d: %s", status, url.c_str());
        http->Close();
        return false;
    }

    constexpr size_t kChunk = 4096;
    std::unique_ptr<char[]> buf(new char[kChunk]);
    bool pushed_any = false;
    while (IsSourceActive() && IsPlaying()) {
        int n = http->Read(buf.get(), kChunk);
        if (n <= 0) break;
        if (!PushToBuffer(buf.get(), (size_t)n)) {
            http->Close();
            return false;
        }
        pushed_any = true;
    }
    http->Close();
    return pushed_any;
}

bool Esp32Radio::StreamTsSegmentAsAdts(const std::string& url)
{
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("User-Agent", "ESP32-Radio/1.0");
    http->SetHeader("Accept", "*/*");

    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Failed to open TS segment: %s", url.c_str());
        return false;
    }
    int status = http->GetStatusCode();
    if (status != 200 && status != 206) {
        ESP_LOGW(TAG, "TS segment status=%d: %s", status, url.c_str());
        http->Close();
        return false;
    }

    constexpr size_t kChunk = 4096;
    std::vector<uint8_t> seg_data;
    seg_data.reserve(64 * 1024);
    std::unique_ptr<char[]> read_buf(new char[kChunk]);
    while (IsSourceActive() && IsPlaying()) {
        int n = http->Read(read_buf.get(), kChunk);
        if (n <= 0) {
            break;
        }
        seg_data.insert(seg_data.end(),
                        reinterpret_cast<uint8_t*>(read_buf.get()),
                        reinterpret_cast<uint8_t*>(read_buf.get()) + n);
    }
    http->Close();
    if (seg_data.empty()) {
        return false;
    }

    // Demux TS -> AAC elementary stream (audio PID from PMT),
    // then extract ADTS frames only from the ES payload.
    bool pushed_any = false;
    const uint8_t* d = seg_data.data();
    const size_t len = seg_data.size();
    if (len < 188) {
        ESP_LOGW(TAG, "TS segment too small: %u", (unsigned)len);
        return false;
    }
    // Some CDN segments may contain prefix/trailing bytes.
    // Re-sync by finding first valid 0x47 alignment and ignore tail remainder.
    size_t sync_off = len;
    for (size_t i = 0; i + 188 < len; ++i) {
        if (d[i] == 0x47 && d[i + 188] == 0x47) {
            sync_off = i;
            break;
        }
    }
    if (sync_off == len) {
        ESP_LOGW(TAG, "TS sync not found: %u", (unsigned)len);
        return false;
    }
    const size_t usable = ((len - sync_off) / 188) * 188;
    if (usable < 188) {
        ESP_LOGW(TAG, "TS usable data too small: %u", (unsigned)len);
        return false;
    }
    if (sync_off != 0 || usable != len) {
        ESP_LOGW(TAG, "TS realign: raw=%u sync_off=%u usable=%u",
                 (unsigned)len, (unsigned)sync_off, (unsigned)usable);
    }
    d += sync_off;
    const size_t ts_len = usable;

    int pmt_pid = -1;
    int audio_pid = -1;
    for (size_t off = 0; off + 188 <= ts_len; off += 188) {
        const uint8_t* p = d + off;
        if (p[0] != 0x47) continue;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        const int afc = (p[3] >> 4) & 0x3;
        size_t payload_off = 4;
        if (afc == 0 || afc == 2) continue;
        if (afc == 3) {
            payload_off = 5 + p[4];
        }
        if (payload_off >= 188) continue;

        if (pid == 0 && (p[1] & 0x40)) {
            size_t sec = payload_off + 1 + p[payload_off];
            if (sec + 8 >= 188 || p[sec] != 0x00) continue;
            const size_t sec_len = ((p[sec + 1] & 0x0F) << 8) | p[sec + 2];
            size_t j = sec + 8;
            const size_t end = sec + 3 + sec_len - 4;
            while (j + 4 <= end && end < 188) {
                const int program = (p[j] << 8) | p[j + 1];
                const int pp = ((p[j + 2] & 0x1F) << 8) | p[j + 3];
                if (program != 0) {
                    pmt_pid = pp;
                    break;
                }
                j += 4;
            }
        } else if (pmt_pid >= 0 && pid == pmt_pid && (p[1] & 0x40)) {
            size_t sec = payload_off + 1 + p[payload_off];
            if (sec + 12 >= 188 || p[sec] != 0x02) continue;
            const size_t sec_len = ((p[sec + 1] & 0x0F) << 8) | p[sec + 2];
            const size_t prog_info_len = ((p[sec + 10] & 0x0F) << 8) | p[sec + 11];
            size_t j = sec + 12 + prog_info_len;
            const size_t end = sec + 3 + sec_len - 4;
            while (j + 5 <= end && end < 188) {
                const int stream_type = p[j];
                const int ep = ((p[j + 1] & 0x1F) << 8) | p[j + 2];
                const size_t es_info_len = ((p[j + 3] & 0x0F) << 8) | p[j + 4];
                if (stream_type == 0x0F || stream_type == 0x11) {  // AAC LC / LATM
                    audio_pid = ep;
                    break;
                }
                j += 5 + es_info_len;
            }
            if (audio_pid >= 0) break;
        }
    }
    if (audio_pid < 0) {
        ESP_LOGW(TAG, "AAC PID not found in TS segment: %s", url.c_str());
        return false;
    }

    std::vector<uint8_t> es;
    es.reserve(ts_len / 2);
    for (size_t off = 0; off + 188 <= ts_len; off += 188) {
        const uint8_t* p = d + off;
        if (p[0] != 0x47) continue;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid != audio_pid) continue;
        const bool pusi = (p[1] & 0x40) != 0;
        const int afc = (p[3] >> 4) & 0x3;
        size_t payload_off = 4;
        if (afc == 0 || afc == 2) continue;
        if (afc == 3) {
            payload_off = 5 + p[4];
        }
        if (payload_off >= 188) continue;

        size_t data_off = payload_off;
        if (pusi && (188 - payload_off) >= 9 &&
            p[payload_off] == 0x00 && p[payload_off + 1] == 0x00 && p[payload_off + 2] == 0x01) {
            const size_t pes_hdr_data_len = p[payload_off + 8];
            data_off = payload_off + 9 + pes_hdr_data_len;
            if (data_off > 188) {
                continue;
            }
        }
        es.insert(es.end(), p + data_off, p + 188);
    }

    for (size_t i = 0; i + 7 < es.size() && IsSourceActive() && IsPlaying();) {
        if (!(es[i] == 0xFF && (es[i + 1] & 0xF6) == 0xF0)) {
            ++i;
            continue;
        }
        const size_t frame_len = ((size_t)(es[i + 3] & 0x03) << 11) |
                                 ((size_t)es[i + 4] << 3) |
                                 ((size_t)(es[i + 5] & 0xE0) >> 5);
        if (frame_len < 7 || i + frame_len > es.size()) {
            ++i;
            continue;
        }
        if (!PushToBuffer(es.data() + i, frame_len)) {
            return false;
        }
        pushed_any = true;
        i += frame_len;
    }

    if (!pushed_any) {
        ESP_LOGW(TAG, "No ADTS frames extracted from TS segment: %s", url.c_str());
    }
    return pushed_any;
}

void Esp32Radio::SourceDataLoop(const std::string& source)
{
    if (!IsM3u8Url(source)) {
        AudioStreamPlayer::SourceDataLoop(source);
        return;
    }

    ESP_LOGI(TAG, "HLS source started: %s", source.c_str());
    std::string current_playlist = source;
    std::string last_segment_url;
    int failures = 0;
    int target_duration_ms = 2000;
    int tls_error_count = 0;
    int next_tls_warn_at = 1;

    while (IsSourceActive() && IsPlaying()) {
        std::string playlist_body;
        if (!DownloadText(current_playlist, playlist_body)) {
            failures++;
            tls_error_count++;
            ESP_LOGW(TAG, "HLS playlist fetch failed (%d): %s", failures, current_playlist.c_str());
            if (tls_error_count >= next_tls_warn_at) {
                ESP_LOGW(TAG, "HLS transient network errors=%d", tls_error_count);
                next_tls_warn_at *= 2;
                if (next_tls_warn_at > 64) next_tls_warn_at = 64;
            }
            if (failures >= 5) break;
            const int backoff = std::min(target_duration_ms, 4000);
            vTaskDelay(pdMS_TO_TICKS(backoff));
            continue;
        }
        failures = 0;
        tls_error_count = 0;
        next_tls_warn_at = 1;
        target_duration_ms = ExtractTargetDurationMs(playlist_body);

        std::string target = PickM3u8Target(current_playlist, playlist_body);
        if (target.empty()) {
            vTaskDelay(pdMS_TO_TICKS(std::max(400, target_duration_ms / 3)));
            continue;
        }

        if (target.find(".m3u8") != std::string::npos) {
            current_playlist = target;
            continue;
        }

        if (target == last_segment_url) {
            // No new segment yet: wait proportionally to target duration.
            vTaskDelay(pdMS_TO_TICKS(std::max(350, target_duration_ms / 2)));
            continue;
        }

        if (StreamBinaryUrlToBuffer(target)) {
            last_segment_url = target;
        } else {
            failures++;
            if (failures >= 3) {
                // force playlist refresh on repeated segment failures
                last_segment_url.clear();
            }
            vTaskDelay(pdMS_TO_TICKS(std::max(400, target_duration_ms / 3)));
        }
    }

    ESP_LOGI(TAG, "HLS source finished: %s", source.c_str());
}

/* ================================================================== */
/*  PlayStation  find station by name/key                            */
/* ================================================================== */

bool Esp32Radio::PlayStation(const std::string& station_name)
{
    ESP_LOGI(TAG, "PlayRadio: %s", station_name.c_str());

    std::string lower_input = station_name;
    std::transform(lower_input.begin(), lower_input.end(), lower_input.begin(), ::tolower);

    /* 1) Search by display name (partial, case-insensitive) */
    for (const auto& kv : radio_stations_) {
        std::string lower_name = kv.second.name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

        if (lower_name.find(lower_input) != std::string::npos ||
            lower_input.find(lower_name) != std::string::npos) {
            ESP_LOGI(TAG, "Matched display name: '%s' -> %s (vol=%.1f)",
                     station_name.c_str(), kv.second.name.c_str(), kv.second.volume);
            current_station_volume_ = kv.second.volume;
            current_station_key_ = kv.first;
            return PlayUrl(kv.second.url, kv.second.name);
        }
    }

    /* 2) Exact key match */
    auto it = radio_stations_.find(station_name);
    if (it != radio_stations_.end()) {
        current_station_volume_ = it->second.volume;
        current_station_key_ = it->first;
        return PlayUrl(it->second.url, it->second.name);
    }

    /* 3) Key match (case-insensitive) */
    for (const auto& kv : radio_stations_) {
        std::string lower_key = kv.first;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        if (lower_key == lower_input) {
            current_station_volume_ = kv.second.volume;
            current_station_key_ = kv.first;
            return PlayUrl(kv.second.url, kv.second.name);
        }
    }

    /* 4) Vietnamese phonetic / keyword matching */
    static const std::vector<std::pair<std::string, std::string>> keyword_map = {
        {"npr", "NPR"},
        {"abc", "ABC1"},
        {"soma", "SOMA1"},
        {"chill", "SOMA2"},
        {"giao thong", "VOV_GT_HN"},
        {"giao thông", "VOV_GT_HN"},
    };

    for (const auto& pair : keyword_map) {
        if (lower_input.find(pair.first) != std::string::npos) {
            auto found = radio_stations_.find(pair.second);
            if (found != radio_stations_.end()) {
                current_station_volume_ = found->second.volume;
                current_station_key_ = found->first;
                return PlayUrl(found->second.url, found->second.name);
            }
        }
    }

    /* 5) VOV + number shorthand */
    if (lower_input.find("vov") != std::string::npos) {
        for (char c = '1'; c <= '5'; ++c) {
            if (lower_input.find(c) != std::string::npos) {
                std::string key = "VOV" + std::string(1, c);
                auto found = radio_stations_.find(key);
                if (found != radio_stations_.end()) {
                    current_station_volume_ = found->second.volume;
                    current_station_key_ = found->first;
                    return PlayUrl(found->second.url, found->second.name);
                }
            }
        }
        /* Default to VOV1 for generic "vov" */
        auto vov1 = radio_stations_.find("VOV1");
        if (vov1 != radio_stations_.end()) {
            current_station_volume_ = vov1->second.volume;
            current_station_key_ = vov1->first;
            return PlayUrl(vov1->second.url, vov1->second.name);
        }
    }

    ESP_LOGE(TAG, "Station not found: '%s'", station_name.c_str());
    return false;
}

bool Esp32Radio::NextStation()
{
    if (!BeginStationSwitch()) {
        return false;
    }
    if (station_order_.empty()) {
        EndStationSwitch();
        return false;
    }
    int start = 0;
    if (!current_station_key_.empty()) {
        for (size_t i = 0; i < station_order_.size(); ++i) {
            if (station_order_[i] == current_station_key_) {
                start = static_cast<int>(i);
                break;
            }
        }
    }
    const int n = static_cast<int>(station_order_.size());
    for (int step = 1; step <= n; ++step) {
        const std::string& key = station_order_[(start + step) % n];
        auto it = radio_stations_.find(key);
        if (it == radio_stations_.end()) continue;
        current_station_key_ = key;
        current_station_volume_ = it->second.volume;
        if (PlayUrl(it->second.url, it->second.name)) {
            EndStationSwitch();
            return true;
        }
        if (last_playurl_busy_) {
            EndStationSwitch();
            return false;
        }
        auto* display = Board::GetInstance().GetDisplay();
        if (display) {
            std::string msg = "Skip error: " + it->second.name;
            display->ShowNotification(msg.c_str());
        }
    }
    EndStationSwitch();
    return false;
}

bool Esp32Radio::PrevStation()
{
    if (!BeginStationSwitch()) {
        return false;
    }
    if (station_order_.empty()) {
        EndStationSwitch();
        return false;
    }
    int start = 0;
    if (!current_station_key_.empty()) {
        for (size_t i = 0; i < station_order_.size(); ++i) {
            if (station_order_[i] == current_station_key_) {
                start = static_cast<int>(i);
                break;
            }
        }
    }
    const int n = static_cast<int>(station_order_.size());
    for (int step = 1; step <= n; ++step) {
        int idx = start - step;
        while (idx < 0) idx += n;
        const std::string& key = station_order_[idx];
        auto it = radio_stations_.find(key);
        if (it == radio_stations_.end()) continue;
        current_station_key_ = key;
        current_station_volume_ = it->second.volume;
        if (PlayUrl(it->second.url, it->second.name)) {
            EndStationSwitch();
            return true;
        }
        if (last_playurl_busy_) {
            EndStationSwitch();
            return false;
        }
        auto* display = Board::GetInstance().GetDisplay();
        if (display) {
            std::string msg = "Skip error: " + it->second.name;
            display->ShowNotification(msg.c_str());
        }
    }
    EndStationSwitch();
    return false;
}

/* ================================================================== */
/*  PlayUrl                                                           */
/* ================================================================== */

bool Esp32Radio::PlayUrl(const std::string& radio_url, const std::string& station_name)
{
    last_playurl_busy_ = false;
    if (radio_url.empty()) {
        ESP_LOGE(TAG, "Radio URL is empty");
        return false;
    }

    ESP_LOGI(TAG, "PlayUrl: %s (%s)",
             station_name.empty() ? "Custom URL" : station_name.c_str(),
             radio_url.c_str());

    // Invalidate pending reconnect attempts from previous station/session.
    switch_generation_.fetch_add(1);
    reconnect_pending_ = false;

    if (!Stop()) {
        vTaskDelay(pdMS_TO_TICKS(350));
        if (!Stop()) {
            last_playurl_busy_ = true;
            ESP_LOGE(TAG, "PlayUrl aborted: previous stream is still stopping");
            auto* display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ShowNotification("Radio busy, retry...");
            }
            return false;
        }
    }

    current_station_url_  = radio_url;
    current_station_name_ = station_name.empty() ? "Custom Radio" : station_name;
    station_name_displayed_ = false;

    if (current_station_volume_ <= 0.0f) {
        current_station_volume_ = RADIO_DEFAULT_VOLUME;
    }
    SetVolume(current_station_volume_);

    /* Auto-detect decoder type */
    AudioDecoderType dtype = GuessDecoderType(radio_url);
    ESP_LOGI(TAG, "Using decoder: %s", (dtype == AudioDecoderType::AAC) ? "AAC" : "MP3");

    // Preflight check for HLS sources: if playlist is unavailable or malformed,
    // fail fast so caller can skip station instead of entering unstable playback.
    if (IsM3u8Url(radio_url)) {
        std::string playlist_body;
        if (!DownloadText(radio_url, playlist_body)) {
            ESP_LOGE(TAG, "HLS preflight failed: cannot fetch playlist: %s", radio_url.c_str());
            auto* display = Board::GetInstance().GetDisplay();
            if (display) {
                std::string msg = "Radio unavailable: " + current_station_name_;
                display->ShowNotification(msg.c_str());
            }
            return false;
        }

        std::string target = PickM3u8Target(radio_url, playlist_body);
        if (target.empty()) {
            ESP_LOGE(TAG, "HLS preflight failed: no playable segment/variant: %s", radio_url.c_str());
            auto* display = Board::GetInstance().GetDisplay();
            if (display) {
                std::string msg = "Radio unavailable: " + current_station_name_;
                display->ShowNotification(msg.c_str());
            }
            return false;
        }
    }

    return StartStream(radio_url, dtype);
}

/* ================================================================== */
/*  Stop                                                              */
/* ================================================================== */

bool Esp32Radio::Stop()
{
    // Explicit stop from UI/control should cancel pending reconnect work.
    switch_generation_.fetch_add(1);
    reconnect_pending_ = false;

    if (!IsPlaying() && !IsDownloading()) {
        return true;
    }

    ESP_LOGI(TAG, "Stopping radio");
    return StopStream();
}

/* ================================================================== */
/*  GetStationList                                                    */
/* ================================================================== */

std::vector<std::string> Esp32Radio::GetStationList() const
{
    std::vector<std::string> list;
    list.reserve(radio_stations_.size());
    for (const auto& kv : radio_stations_) {
        list.push_back(kv.first + " - " + kv.second.name);
    }
    return list;
}

/* ================================================================== */
/*  AudioStreamPlayer hooks                                           */
/* ================================================================== */

void Esp32Radio::OnStreamInfoReady(int sample_rate, int bits_per_sample, int channels, int bitrate, int frame_size)
{
    ESP_LOGI(TAG, "Stream info: %s, %d Hz, %d bit, %d ch, %d kbps, %d frame size",
             current_station_name_.c_str(), sample_rate, bits_per_sample, channels, bitrate, frame_size);
}

void Esp32Radio::OnDisplayReady()
{
    /* Display is now handled externally via Application callbacks */
    ESP_LOGD(TAG, "Display ready for station: %s", current_station_name_.c_str());
    station_name_displayed_ = true;
}

bool Esp32Radio::OnPlaybackFinishedAndContinue()
{
    ESP_LOGW(TAG, "Radio playback finished");
    if (current_station_url_.empty()) {
        return false;
    }

    if (reconnect_pending_.exchange(true)) {
        ESP_LOGW(TAG, "Radio auto-reconnect already pending");
        return true;
    }

    struct ReconnectCtx {
        Esp32Radio* self;
        std::string url;
        std::string name;
        AudioDecoderType dtype;
        uint32_t generation;
    };

    auto* ctx = new ReconnectCtx{
        this,
        current_station_url_,
        current_station_name_,
        GuessDecoderType(current_station_url_),
        switch_generation_.load()
    };
    BaseType_t created = xTaskCreatePinnedToCore(
        ReconnectTaskEntry,
        "radio_reconnect",
        4096,
        ctx,
        4,
        nullptr,
        tskNO_AFFINITY);
    if (created != pdPASS) {
        reconnect_pending_ = false;
        ESP_LOGE(TAG, "Failed to create radio reconnect task");
        delete ctx;
        return false;
    }
    return true;
}

void Esp32Radio::ReconnectTaskEntry(void* param)
{
    struct ReconnectCtx {
        Esp32Radio* self;
        std::string url;
        std::string name;
        AudioDecoderType dtype;
        uint32_t generation;
    };
    std::unique_ptr<ReconnectCtx> ctx(static_cast<ReconnectCtx*>(param));
    if (!ctx || !ctx->self) {
        vTaskDelete(nullptr);
        return;
    }

    Esp32Radio* self = ctx->self;
    vTaskDelay(pdMS_TO_TICKS(1200));

    if (self->IsPlaying()) {
        self->reconnect_pending_ = false;
        vTaskDelete(nullptr);
        return;
    }
    if (self->current_station_url_ != ctx->url) {
        self->reconnect_pending_ = false;
        vTaskDelete(nullptr);
        return;
    }
    if (self->switch_generation_.load() != ctx->generation) {
        self->reconnect_pending_ = false;
        vTaskDelete(nullptr);
        return;
    }

    bool resumed = self->StartStream(ctx->url, ctx->dtype);
    if (resumed) {
        ESP_LOGI(TAG, "Radio auto-reconnected: %s", ctx->name.c_str());
        self->reconnect_pending_ = false;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGE(TAG, "Radio auto-reconnect failed: %s", ctx->name.c_str());
    auto* display = Board::GetInstance().GetDisplay();
    if (display) {
        std::string msg = "Radio error: " + ctx->name;
        display->ShowNotification(msg.c_str());
    }
    if (self->NextStation()) {
        if (display) {
            std::string msg = "Skip -> " + self->current_station_name_;
            display->ShowNotification(msg.c_str());
        }
    }
    self->reconnect_pending_ = false;
    vTaskDelete(nullptr);
}
