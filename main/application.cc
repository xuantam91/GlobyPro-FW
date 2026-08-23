#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "ota_server.h"
#include "wifi_station.h"
#include "ssid_manager.h"
#include "sd_card.h"
#include "esp32_sd_music.h"
#include "features/mcp_server_features.h"
#include "features/music/audio_stream_player.h"
#include "lcd_display.h"
#include "oled_display.h"
#include "lvgl_theme.h"
#include "features/music/music_visualizer.h"
#include "features/spectrum/spectrum_manager.h"
#include "features/video/video_player.h"
#include "features/QRCode/qrcode_display.h"
#include <esp_lvgl_port.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <array>
#include <string_view>
#include <unordered_set>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include "features/weather/weather_ui.h"
#include "ml307_board.h"
#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
#if CONFIG_USE_ALARM
    "alarm",
#endif
    "fatal_error",
    "invalid_state"
};

namespace {

bool SupportsJiuchuanLcdDriverSetting() {
    return std::string_view(BOARD_TYPE) == "jiuchuan-s3";
}

bool IsLuxiaoban154Board() {
    return std::string_view(BOARD_TYPE) == "luxiaoban-xiaozhi-1.54tft";
}

bool IsSdMusicDisabledForBoard() {
    return IsLuxiaoban154Board();
}

bool IsAlarmDisabledForBoard() {
    return IsLuxiaoban154Board();
}

bool IsSystemTimeSynced() {
    time_t now = 0;
    struct tm timeinfo = {};
    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (2025 - 1900);
}

bool SyncTimeViaSntpFallback() {
    // Keep local time in Vietnam (UTC+7)
    setenv("TZ", "UTC-7", 1);
    tzset();

    if (IsSystemTimeSynced()) {
        ESP_LOGI(TAG, "System time already synced");
        return true;
    }

    ESP_LOGW(TAG, "No server_time from OTA, trying SNTP fallback...");

    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, (char*)"time.google.com");
    esp_sntp_setservername(1, (char*)"pool.ntp.org");
    esp_sntp_init();

    bool synced = false;
    for (int i = 0; i < 24; ++i) {  // up to ~12 seconds
        vTaskDelay(pdMS_TO_TICKS(500));
        if (IsSystemTimeSynced()) {
            synced = true;
            break;
        }
    }

    if (synced) {
        time_t now = 0;
        struct tm timeinfo = {};
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "SNTP sync OK: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP fallback timeout, system time still not synced");
    }

    return synced;
}

int GetJiuchuanLcdDriverSetting() {
    Settings display_settings("display", false);
    int driver = display_settings.GetInt("lcd_driver", JIUCHUAN_DEFAULT_LCD_DRIVER);
    if (driver != 1 && driver != 2) {
        driver = JIUCHUAN_DEFAULT_LCD_DRIVER;
    }
    return driver;
}

std::string BuildRolePortalUrlFromMac() {
    constexpr const char* kDefaultBaseUrl = "https://globyai.online/role?mac=";

    Settings wifi_settings("wifi", false);
    std::string base_url = wifi_settings.GetString("role_qr_base_url", kDefaultBaseUrl);
    if (base_url.empty()) {
        base_url = kDefaultBaseUrl;
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string mac_compact;
    mac_compact.reserve(mac.size());
    for (unsigned char ch : mac) {
        if (std::isalnum(ch)) {
            mac_compact.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    if (mac_compact.empty()) {
        return {};
    }
    return base_url + mac_compact;
}

std::string NormalizeSsidForCompare(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (unsigned char ch : src) {
        if (ch >= 32 && ch != 127) {
            out.push_back(static_cast<char>(ch));
        }
    }
    size_t start = 0;
    while (start < out.size() && std::isspace(static_cast<unsigned char>(out[start]))) {
        ++start;
    }
    size_t end = out.size();
    while (end > start && std::isspace(static_cast<unsigned char>(out[end - 1]))) {
        --end;
    }
    return out.substr(start, end - start);
}

bool IsAllDigits(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (unsigned char ch : s) {
        if (!std::isdigit(ch)) {
            return false;
        }
    }
    return true;
}

std::string NormalizeIntentText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool prev_space = true;
    for (unsigned char ch : text) {
        const bool is_alpha_num = std::isalnum(ch) != 0;
        if (is_alpha_num) {
            out.push_back(static_cast<char>(std::tolower(ch)));
            prev_space = false;
            continue;
        }
        if (!prev_space) {
            out.push_back(' ');
            prev_space = true;
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

bool ContainsAnyIntentPhrase(const std::string& normalized_text,
    std::initializer_list<const char*> phrases) {
    for (const char* phrase : phrases) {
        if (normalized_text.find(phrase) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsReadingBookModeStartIntent(const std::string& text) {
    (void)text;
    // Temporarily disabled on v2.1.0 due to current server/session limitations.
    return false;
}

bool IsReadingBookModeEndIntent(const std::string& text) {
    (void)text;
    // Temporarily disabled on v2.1.0 due to current server/session limitations.
    return false;
}

int Base64UrlCharToVal(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

std::string DecodeBase64Url(const std::string& input) {
    std::string out;
    out.reserve((input.size() * 3) / 4);

    int val = 0;
    int valb = -8;
    for (unsigned char c : input) {
        if (c == '=') {
            break;
        }
        int d = Base64UrlCharToVal(c);
        if (d < 0) {
            continue;
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string ExtractPairIdFromJwtToken(const std::string& token_raw) {
    std::string token = token_raw;
    const std::string bearer = "Bearer ";
    if (token.rfind(bearer, 0) == 0) {
        token = token.substr(bearer.size());
    }

    size_t first_dot = token.find('.');
    if (first_dot == std::string::npos) {
        return {};
    }
    size_t second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string::npos || second_dot <= first_dot + 1) {
        return {};
    }

    std::string payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
    std::string payload_json = DecodeBase64Url(payload_b64);
    if (payload_json.empty()) {
        return {};
    }

    cJSON* root = cJSON_Parse(payload_json.c_str());
    if (root == nullptr) {
        return {};
    }

    static const char* const kKeys[] = {
        "pair_id", "pairId", "device_id", "deviceId", "uid", "user_id", "id"
    };
    std::string out;
    for (const char* key : kKeys) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsNumber(item)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f", item->valuedouble);
            out = buf;
            break;
        }
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            std::string s = item->valuestring;
            if (IsAllDigits(s)) {
                out = s;
                break;
            }
        }
    }

    if (out.empty()) {
        std::string best;
        std::string run;
        for (unsigned char ch : payload_json) {
            if (std::isdigit(ch)) {
                run.push_back(static_cast<char>(ch));
            } else {
                if (run.size() >= 6 && run.size() > best.size()) {
                    best = run;
                }
                run.clear();
            }
        }
        if (run.size() >= 6 && run.size() > best.size()) {
            best = run;
        }
        out = best;
    }

    cJSON_Delete(root);
    return out;
}

std::string ResolvePairDeviceId() {
    Settings device("device", false);
    std::string cached_pair_id = device.GetString("pair_id");
    if (IsAllDigits(cached_pair_id)) {
        return cached_pair_id;
    }

    Settings mqtt("mqtt", false);
    std::string from_mqtt_username = ExtractPairIdFromJwtToken(mqtt.GetString("username"));
    if (!from_mqtt_username.empty()) {
        return from_mqtt_username;
    }
    std::string from_mqtt_password = ExtractPairIdFromJwtToken(mqtt.GetString("password"));
    if (!from_mqtt_password.empty()) {
        return from_mqtt_password;
    }

    Settings ws("websocket", false);
    std::string from_ws_token = ExtractPairIdFromJwtToken(ws.GetString("token"));
    if (!from_ws_token.empty()) {
        return from_ws_token;
    }

    return {};
}

std::string ToLowerAscii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::string ExtractLongestDigits(const std::string& text, size_t min_len = 5, size_t max_len = 16) {
    std::string best;
    std::string run;
    for (unsigned char ch : text) {
        if (std::isdigit(ch)) {
            run.push_back(static_cast<char>(ch));
        } else {
            if (run.size() >= min_len && run.size() <= max_len && run.size() > best.size()) {
                best = run;
            }
            run.clear();
        }
    }
    if (run.size() >= min_len && run.size() <= max_len && run.size() > best.size()) {
        best = run;
    }
    return best;
}

std::string ExtractPairIdFromAssistantText(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    std::string lower = ToLowerAscii(text);
    bool likely_device_id_text =
        (lower.find("device id") != std::string::npos) ||
        (lower.find("deviceid") != std::string::npos) ||
        (lower.find("pair id") != std::string::npos) ||
        (lower.find("id cua") != std::string::npos) ||
        (lower.find("id la") != std::string::npos);
    if (!likely_device_id_text) {
        return {};
    }

    return ExtractLongestDigits(text);
}

#if CONFIG_USE_ALARM
time_t BuildNextLocalTime(int hour, int minute, int weekday /* 0..6, -1 means daily */) {
    time_t now = time(nullptr);
    struct tm now_tm;
    localtime_r(&now, &now_tm);

    struct tm target = now_tm;
    target.tm_sec = 0;
    target.tm_min = minute;
    target.tm_hour = hour;

    if (weekday >= 0) {
        int delta_days = (weekday - now_tm.tm_wday + 7) % 7;
        if (delta_days == 0) {
            struct tm tmp = target;
            if (mktime(&tmp) <= now) {
                delta_days = 7;
            }
        }
        target.tm_mday += delta_days;
    } else {
        struct tm tmp = target;
        if (mktime(&tmp) <= now) {
            target.tm_mday += 1;
        }
    }

    return mktime(&target);
}

int ParseWeekdayShort(const char* day) {
    if (day == nullptr) return -1;
    if (strcmp(day, "sun") == 0) return 0;
    if (strcmp(day, "mon") == 0) return 1;
    if (strcmp(day, "tue") == 0) return 2;
    if (strcmp(day, "wed") == 0) return 3;
    if (strcmp(day, "thu") == 0) return 4;
    if (strcmp(day, "fri") == 0) return 5;
    if (strcmp(day, "sat") == 0) return 6;
    return -1;
}
#endif

constexpr int kPowerMenuPageSize = 4;
constexpr int kDefaultPowerOffTimeoutSeconds = 60 * 60;

bool GetPowerSaveEnabledSetting() {
    Settings power_settings("power", false);
    Settings wifi_settings("wifi", false);
    return power_settings.GetBool("save_en", wifi_settings.GetBool("sleep_mode", true));
}

void SetPowerSaveEnabledSetting(bool enabled) {
    Settings power_settings("power", true);
    power_settings.SetBool("save_en", enabled);

    // Keep legacy key for backward compatibility with existing code paths.
    Settings wifi_settings("wifi", true);
    wifi_settings.SetBool("sleep_mode", enabled);
}

int GetPowerOffTimeoutSecondsSetting() {
    Settings power_settings("power", false);
    int timeout = power_settings.GetInt("off_sec", kDefaultPowerOffTimeoutSeconds);
    if (timeout < 60) {
        timeout = kDefaultPowerOffTimeoutSeconds;
    }
    return timeout;
}

void SetPowerOffTimeoutSecondsSetting(int seconds) {
    if (seconds < 60) {
        seconds = 60;
    }
    Settings power_settings("power", true);
    power_settings.SetInt("off_sec", seconds);
}

std::string FormatPowerOffTimeoutLabel(int seconds) {
    if (seconds < 3600) {
        return std::to_string(seconds / 60) + "m";
    }
    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    if (minutes == 0) {
        return std::to_string(hours) + "h";
    }
    return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
}

std::vector<int> GetPowerTimeoutOptions() {
    return {30 * 60, 45 * 60, 60 * 60, 90 * 60, 120 * 60, 180 * 60};
}

bool IsLuxiaobanBoard() {
    return strcmp(BOARD_NAME, "luxiaoban-xiaozhi-1.54tft") == 0;
}

bool IsLuxiaobanActivated() {
    if (!IsLuxiaobanBoard()) {
        return false;
    }
    Settings board_settings("luxiaoban", false);
    return board_settings.GetBool("activated", false);
}

void SetLuxiaobanActivated(bool activated) {
    if (!IsLuxiaobanBoard()) {
        return;
    }
    Settings board_settings("luxiaoban", true);
    board_settings.SetBool("activated", activated);
    ESP_LOGI(TAG, "Luxiaoban activation flag set: %s", activated ? "true" : "false");
}

void EnsureLuxiaobanPostActivationOtaUrl() {
    if (!IsLuxiaobanBoard()) {
        return;
    }
    constexpr const char* kLuxiaobanOtaUrl = "http://ota.globy.tech/";
    Settings wifi_settings("wifi", true);
    if (wifi_settings.GetString("ota_url") == kLuxiaobanOtaUrl) {
        return;
    }
    wifi_settings.SetString("ota_url", kLuxiaobanOtaUrl);
    ESP_LOGI(TAG, "Applied post-activation OTA URL for luxiaoban: %s", kLuxiaobanOtaUrl);
}

void EnsureLuxiaobanPreActivationOtaUrl() {
    if (!IsLuxiaobanBoard()) {
        return;
    }
    Settings wifi_settings("wifi", true);
    if (wifi_settings.GetString("ota_url") == CONFIG_OTA_URL) {
        return;
    }
    wifi_settings.SetString("ota_url", CONFIG_OTA_URL);
    ESP_LOGI(TAG, "Applied pre-activation OTA URL for luxiaoban: %s", CONFIG_OTA_URL);
}

} // namespace

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay is 10 seconds

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        std::string url;
        constexpr const char* kGlobyOtaUrl = "http://ota.globy.tech/";
        if (IsLuxiaobanBoard()) {
            if (IsLuxiaobanActivated()) {
                url = kGlobyOtaUrl;
            } else {
                url = CONFIG_OTA_URL;
            }
        } else {
            url = CONFIG_OTA_URL;
        }

        // Flow #1 (always): default endpoint for server time + system config.
        if (!ota.CheckVersion(url)) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // The delay time doubles after each retry.
            continue;
        }

        // Flow #2: custom endpoint for firmware update (Globy / custom OTA).
        // Keep this independent so custom server failures do not break
        // time sync and base system config from default endpoint.
        if (!IsLuxiaobanBoard()) {
            Settings wifi_settings("wifi", false);
            std::string custom_ota_url = wifi_settings.GetString("ota_url_custom");
            if (custom_ota_url.empty()) {
                // backward compatibility with old builds
                custom_ota_url = wifi_settings.GetString("ota_url");
            }
            if (!custom_ota_url.empty() && custom_ota_url != CONFIG_OTA_URL) {
                if (!ota.CheckVersion(custom_ota_url)) {
                    ESP_LOGW(TAG, "Custom OTA check failed: %s", custom_ota_url.c_str());
                }
            }
        }

        // Luxiaoban activation flow:
        // - Unactivated device must keep default OTA URL (activation section available).
        // - After activation, persist Globy OTA URL for subsequent boots.
        if (IsLuxiaobanBoard()) {
            if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
                SetLuxiaobanActivated(true);
                EnsureLuxiaobanPostActivationOtaUrl();
            } else {
                SetLuxiaobanActivated(false);
                EnsureLuxiaobanPreActivationOtaUrl();
            }
        }

        retry_count = 0;
        retry_delay = 10; // Reset retry delay time

        if (ota.HasNewVersion()) {
            if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                if (IsLuxiaobanBoard()) {
                    SetLuxiaobanActivated(true);
                    EnsureLuxiaobanPostActivationOtaUrl();
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            ESP_LOGI(TAG, "Stopped speaking by user");
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            story_mode_active_ = false;
            protocol_->CloseAudioChannel();
            ESP_LOGI(TAG, "Stopped listening by user");
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            // Ensure media playback is fully stopped and codec returns to
            // baseline sample-rate before entering voice conversation path.
            StopOtherMedia();
            if (auto* codec = Board::GetInstance().GetAudioCodec()) {
                codec->SetOutputSampleRate(-1);
            }

            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        story_mode_active_ = false;
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Ensure local timezone is applied before any local-time based features
    // (e.g. default alarm creation) are initialized.
    setenv("TZ", "UTC-7", 1);  // Vietnam UTC+7 (POSIX sign convention)
    tzset();

    /* Setup the display */
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

#if CONFIG_ENABLE_IDLE_SCREEN
    idle_screen_ = std::make_unique<IdleScreen>(display);
    idle_screen_->Start();
#endif

#if (0) // Test QR code display
    qrcode::QRCodeDisplay::GetInstance().Show("http://192.168.1.100/ota", "192.168.1.100/ota");
    return;
#endif

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
#ifdef CONFIG_MIC_HIGH_PASS_FILTER_ENABLE
    // Enable high pass filter to reduce low frequency noise
    {
        float gain = CONFIG_MIC_HIGH_PASS_FILTER_GAIN / 100.0f;
        audio_service_.SetHighPassFilter(new HighPassFilter(gain));
    }
#endif
    audio_service_.Start();
    // codec->SetOutputVolume(10);

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Start the main event loop task with priority 3.
    // 5 KB was insufficient for some conversational/music tool flows
    // (e.g. online music intent -> long JSON/message handling).
    // Increase stack headroom to avoid overflow.
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 1024 * 10, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Check for offline mode prompt during startup */
    offline_mode_ = board.PromptOfflineMode();

    if (!offline_mode_) {
        /* Wait for the network to be ready */
        board.StartNetwork();

#ifdef CONFIG_WEATHER_IDLE_DISPLAY_ENABLE
        // Start the independent weather idle display task after network is ready
        StartWeatherIdleTask();
#endif

        // Register network tool — pass overlay callback so the QR canvas can
        // hide/restore the host display's normal UI while it is visible.
        // SetMediaOverlayActive is virtual: works for both LCD and OLED.
        McpFeatureTools::RegisterIp2QrCodeTool([display](bool active) {
            display->SetMediaOverlayActive(active);
        });

        // Initialize media components and register their MCP tools
        InitMusic();
        InitRadio();

#ifdef CONFIG_SD_CARD_ENABLE
        auto sd_card = board.GetSdCard();
        if (sd_card != nullptr) {
            if (sd_card->Initialize() == ESP_OK) {
                ESP_LOGI(TAG, "SD card mounted successfully");
                InitSdMusic();
                InitVideo();
            } else {
                ESP_LOGW(TAG, "Failed to mount SD card");
            }
        }
#endif

        // Initialize alarm after SD init so CSV persistence can be loaded/saved
        // immediately without early "card not mounted" warnings.
        InitAlarmClock();

        // Update the status bar immediately to show the network state
        display->UpdateStatusBar(true);

        // Check for new assets version
        CheckAssetsVersion();

        // Check for new firmware version or get the MQTT broker address
        Ota ota;
        CheckNewVersion(ota);

        // Start the OTA server
        auto& ota_server = ota::OtaServer::GetInstance();
        if (ota_server.Start() == ESP_OK) {
            ESP_LOGI(TAG, "OTA server started successfully");
        } else {
            ESP_LOGE(TAG, "Failed to start OTA server");
        }


    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    display->SetEmotion("happy");
    display->SetChatMessage("assistant", "Ta-da! Globy is here!\nLet's chat and play!");
    audio_service_.PlaySound(Lang::Sounds::OGG_WELCOME);
    vTaskDelay(pdMS_TO_TICKS(3500));

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        if (story_mode_active_) {
            Schedule([this]() {
                listening_mode_ = kListeningModeManualStop;
                SetDeviceState(kDeviceStateListening);
                if (protocol_ && protocol_->IsAudioChannelOpened()) {
                    protocol_->SendStartListening(kListeningModeManualStop);
                }
                ESP_LOGI(TAG, "Reading Book mode resumed after audio channel reopen");
            });
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            if (story_mode_active_ && protocol_) {
                ESP_LOGW(TAG, "Reading Book mode active while audio channel closed, reopening");
                SetDeviceState(kDeviceStateConnecting);
                if (protocol_->OpenAudioChannel()) {
                    return;
                }
                ESP_LOGW(TAG, "Failed to reopen audio channel in Reading Book mode");
            }
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    if (story_mode_active_) {
                        ESP_LOGI(TAG, "Reading Book mode active: suppress TTS start");
                        if (protocol_) {
                            protocol_->SendAbortSpeaking(kAbortReasonNone);
                        }
                        if (device_state_ != kDeviceStateListening) {
                            SetDeviceState(kDeviceStateListening);
                        }
                        return;
                    }
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (story_mode_active_) {
                        if (device_state_ != kDeviceStateListening) {
                            SetDeviceState(kDeviceStateListening);
                        }
                        return;
                    }
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        if (story_mode_active_) {
                            return;
                        }
                        TryCreateAlarmFromText(message);
                        std::string detected_pair_id = ExtractPairIdFromAssistantText(message);
                        if (detected_pair_id.empty() && waiting_device_id_from_ai_) {
                            detected_pair_id = ExtractLongestDigits(message);
                        }
                        if (!detected_pair_id.empty()) {
                            Settings device_settings("device", true);
                            if (device_settings.GetString("pair_id") != detected_pair_id) {
                                device_settings.SetString("pair_id", detected_pair_id);
                                ESP_LOGI(TAG, "Cached pair device ID from assistant text: %s", detected_pair_id.c_str());
                            }
                            if (waiting_device_id_from_ai_) {
                                waiting_device_id_from_ai_ = false;
                                Board::GetInstance().GetDisplay()->ShowNotification(
                                    ("Device ID: " + detected_pair_id).c_str());
                            }
                        }
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    if (!story_mode_active_ && IsReadingBookModeStartIntent(message)) {
                        story_mode_active_ = true;
                        listening_mode_ = kListeningModeManualStop;
                        if (protocol_ && protocol_->IsAudioChannelOpened()) {
                            protocol_->SendStartListening(kListeningModeManualStop);
                        }
                        display->ShowNotification("Reading Book mode ON");
                        ESP_LOGI(TAG, "Reading Book mode enabled");
                    } else if (story_mode_active_ && IsReadingBookModeEndIntent(message)) {
                        story_mode_active_ = false;
                        if (protocol_ && protocol_->IsAudioChannelOpened()) {
                            protocol_->SendStopListening();
                        }
                        if (device_state_ == kDeviceStateListening) {
                            SetDeviceState(kDeviceStateIdle);
                        }
                        display->ShowNotification("Reading Book mode OFF");
                        ESP_LOGI(TAG, "Reading Book mode disabled");
                    }
                    TryCreateAlarmFromText(message);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
        bool protocol_started = protocol_->Start();

        SystemInfo::PrintHeapStats();
        SetDeviceState(kDeviceStateIdle);

        has_server_time_ = ota.HasServerTime();
        if (!has_server_time_) {
            has_server_time_ = SyncTimeViaSntpFallback();
        }
        if (protocol_started) {
            std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
            display->ShowNotification(message.c_str());
            display->SetChatMessage("system", "");
            // Play the success sound to indicate the device is ready
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        }
    } else {
        // Offline Mode Startup
        display->SetStatus("Offline Mode");
        display->SetChatMessage("system", "");

#ifdef CONFIG_SD_CARD_ENABLE
        auto sd_card = board.GetSdCard();
        if (sd_card != nullptr) {
            if (sd_card->Initialize() == ESP_OK) {
                ESP_LOGI(TAG, "SD card mounted successfully in offline mode");
                InitSdMusic();
                InitVideo();
            } else {
                ESP_LOGW(TAG, "Failed to mount SD card in offline mode");
            }
        }
#endif
        // Update the status bar immediately to show the network state
        display->UpdateStatusBar(true);
        SetDeviceState(kDeviceStateIdle);
        ToggleMainMenu();
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

#if CONFIG_ENABLE_IDLE_SCREEN
            if (idle_screen_ && (device_state_ != kDeviceStateIdle || IsMediaPlaying())) {
                idle_screen_->ResetTimer();
            }
#endif

#if CONFIG_USE_ALARM
            static bool alarm_ui_active = false;
            constexpr time_t kAlarmAutoStopSeconds = 5 * 60;
            const bool in_login_flow =
                (device_state_ == kDeviceStateStarting) ||
                (device_state_ == kDeviceStateConnecting) ||
                (device_state_ == kDeviceStateActivating);
            if (!in_login_flow && HasAlarmEvent() && general_timer_) {
                std::string alarm_message = general_timer_->GetAlarmMessage();
                if (alarm_message.empty()) {
                    alarm_message = "Alarm";
                }

                if (device_state_ != kDeviceStateAlarm) {
                    if (device_state_ == kDeviceStateSpeaking) {
                        ESP_LOGI(TAG, "Alarm ring, abort speaking");
                        AbortSpeaking(kAbortReasonNone);
                        if (protocol_ && protocol_->IsAudioChannelOpened()) {
                            protocol_->CloseAudioChannel();
                        }
                        aborted_ = false;
                    } else if (device_state_ == kDeviceStateListening) {
                        ESP_LOGI(TAG, "Alarm ring while listening");
                    }

                    SnapshotAndPauseMediaForAlarm();
                    SetDeviceState(kDeviceStateAlarm);
                    alarm_started_at_ = time(nullptr);
                    display->SetChatMessage("system", alarm_message.c_str());
                    display->SetEmotion("neutral");
                }

                if (audio_service_.IsIdle()) {
                    if (alarm_repeat_interval_ticks_ == 0) {
                        // Loop reminder sound while alarm is active.
                        PlaySound(Lang::Sounds::OGG_ALARM_RING);
                        alarm_repeat_interval_ticks_ = 3;
                    } else {
                        --alarm_repeat_interval_ticks_;
                    }
                }
                if (alarm_started_at_ > 0) {
                    const time_t now = time(nullptr);
                    if (now > alarm_started_at_ &&
                        (now - alarm_started_at_) >= kAlarmAutoStopSeconds) {
                        ESP_LOGW(TAG, "Alarm auto-stop after %ld seconds", (long)(now - alarm_started_at_));
                        if (general_timer_ && general_timer_->isRinging()) {
                            general_timer_->ClearRinging();
                        } else {
                            ClearAlarmEvent();
                            OnAlarmDismissed();
                        }
                    }
                }
                alarm_ui_active = true;
            } else {
                alarm_repeat_interval_ticks_ = 0;
                if (alarm_ui_active && device_state_ == kDeviceStateAlarm) {
                    SetDeviceState(kDeviceStateIdle);
                    OnAlarmDismissed();
                }
                alarm_ui_active = false;
            }
#endif
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }

        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
#if CONFIG_USE_ALARM
    } else if (device_state_ == kDeviceStateAlarm) {
        if (general_timer_ && general_timer_->isRinging()) {
            general_timer_->ClearRinging();
        } else {
            ClearAlarmEvent();
            DismissAlert();
        }
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
#endif
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        ESP_LOGI(TAG, "Device state already in %s, no need to change", STATE_STRINGS[state]);
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

#ifdef CONFIG_WEATHER_IDLE_DISPLAY_ENABLE
    // --- [ADD HIDDEN/SHOWABLE LOGIC FOR IDLE SCREEN] ---
    if (state != kDeviceStateIdle) {
        // If not Idle, hide the idle screen
        ESP_LOGI(TAG, "Hiding idle screen due to state change: %s -> %s", 
                STATE_STRINGS[previous_state], STATE_STRINGS[state]);
        display->HideIdleCard();
    }
    // -----------------------------
#endif

#if CONFIG_ENABLE_IDLE_SCREEN
    if (idle_screen_ && state != kDeviceStateIdle) {
        idle_screen_->ResetTimer();
    }
#endif

    auto led = board.GetLed();
    led->OnStateChanged();
    // Stop all active media and clear display overlays when leaving idle state
    if (previous_state == kDeviceStateIdle && state != kDeviceStateIdle) {
        StopOtherMedia();
        qrcode::QRCodeDisplay::GetInstance().Clear();
    }																	   
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
#if CONFIG_USE_ALARM
        case kDeviceStateAlarm:
            audio_service_.ResetDecoder();
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            display->SetStatus("Alarm");
            break;
#endif
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";
    
    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);
    
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveMode(false);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [display](int progress, size_t speed) {
        std::thread([display, progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            display->SetChatMessage("system", buffer);
        }).detach();
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveMode(true); // Restore power save mode
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (IsMediaPlaying()) {
        return false;
    }

    if ((music_ && music_->IsDownloading()) || (radio_ && radio_->IsDownloading())) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}


// New: Receive external audio data (such as music playback)
void Application::AddAudioData(AudioStreamPacket&& packet) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (device_state_ == kDeviceStateIdle && codec->output_enabled()) {
        // packet.payload contains raw PCM data (int16_t)
        if (packet.payload.size() >= 2) {
            size_t num_samples = packet.payload.size() / sizeof(int16_t);
            std::vector<int16_t> pcm_data(num_samples);
            memcpy(pcm_data.data(), packet.payload.data(), packet.payload.size());
            
            // Check if sample rate matches, if not, perform simple resampling
            if (packet.sample_rate != codec->output_sample_rate()) {
                // ESP_LOGI(TAG, "Resampling music audio from %d to %d Hz", 
                //         packet.sample_rate, codec->output_sample_rate());
                
                // Validate sample rate parameters
                if (packet.sample_rate <= 0 || codec->output_sample_rate() <= 0) {
                    ESP_LOGE(TAG, "Invalid sample rates: %d -> %d", 
                            packet.sample_rate, codec->output_sample_rate());
                    return;
                }
                
                std::vector<int16_t> resampled;
                
                if (packet.sample_rate > codec->output_sample_rate()) {
                    ESP_LOGI(TAG, "Music playback: Switching sample rate from %d Hz to %d Hz", 
                        codec->output_sample_rate(), packet.sample_rate);

                    // Try to dynamically switch sample rate
                    if (codec->SetOutputSampleRate(packet.sample_rate)) {
                        ESP_LOGI(TAG, "Successfully switched to music playback sample rate: %d Hz", packet.sample_rate);
                    } else {
                        ESP_LOGW(TAG, "Cannot switch sample rate, continue using current sample rate: %d Hz", codec->output_sample_rate());
                    }
                } else {
                    // Upsampling: linear interpolation
                    float upsample_ratio = codec->output_sample_rate() / static_cast<float>(packet.sample_rate);
                    size_t expected_size = static_cast<size_t>(pcm_data.size() * upsample_ratio + 0.5f);
                    resampled.reserve(expected_size);
                    
                    for (size_t i = 0; i < pcm_data.size(); ++i) {
                        // Add original sample
                        resampled.push_back(pcm_data[i]);
                        
                        // Calculate number of samples to interpolate
                        int interpolation_count = static_cast<int>(upsample_ratio) - 1;
                        if (interpolation_count > 0 && i + 1 < pcm_data.size()) {
                            int16_t current = pcm_data[i];
                            int16_t next = pcm_data[i + 1];
                            for (int j = 1; j <= interpolation_count; ++j) {
                                float t = static_cast<float>(j) / (interpolation_count + 1);
                                int16_t interpolated = static_cast<int16_t>(current + (next - current) * t);
                                resampled.push_back(interpolated);
                            }
                        } else if (interpolation_count > 0) {
                            // For the last sample, simply repeat it
                            for (int j = 1; j <= interpolation_count; ++j) {
                                resampled.push_back(pcm_data[i]);
                            }
                        }
                    }
                    
                    ESP_LOGI(TAG, "Upsampled %d -> %d samples (ratio: %.2f)", 
                            pcm_data.size(), resampled.size(), upsample_ratio);
                }
                
                pcm_data = std::move(resampled);
            }
            
            // Ensure audio output is enabled
            if (!codec->output_enabled()) {
                ESP_LOGW(TAG, "%s Enabling audio output for music playback", __func__);
                codec->EnableOutput(true);
            }
            
            // Send PCM data to audio codec
            codec->OutputData(pcm_data);
            
            audio_service_.UpdateOutputTimestamp();
        }
    }
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

/* ==================================================================
 * Media Player API Implementations
 * ================================================================== */

bool Application::PlayMusic(const std::string& song_name, const std::string& artist_name) {
    if (!music_) {
        ESP_LOGW(TAG, "Music module not available");
        return false;
    }

    // Stop other media before playing music
    StopOtherMedia(MediaComponent::kMusic);

    // Ensure device is idle before starting playback
    EnsureIdleForMedia();

    ESP_LOGI(TAG, "PlayMusic: song='%s' artist='%s'", song_name.c_str(), artist_name.c_str());
    if (!music_->Download(song_name, artist_name)) {
        ESP_LOGE(TAG, "PlayMusic: failed to get music resource");
        return false;
    }
    auto result = music_->GetDownloadResult();
    ESP_LOGI(TAG, "PlayMusic: result=%s", result.c_str());
    return true;
}

bool Application::PlayRadio(const std::string& station_name) {
    if (!radio_) {
        ESP_LOGW(TAG, "Radio module not available");
        return false;
    }

    // Stop other media before playing radio
    StopOtherMedia(MediaComponent::kRadio);

    // Ensure device is idle before starting playback
    EnsureIdleForMedia();

    ESP_LOGI(TAG, "PlayRadio: station='%s'", station_name.c_str());
    return radio_->PlayStation(station_name);
}

bool Application::PlayRadioUrl(const std::string& url, const std::string& station_name) {
    if (!radio_) {
        ESP_LOGW(TAG, "Radio module not available");
        return false;
    }

    // Stop other media before playing radio URL
    StopOtherMedia(MediaComponent::kRadio);

    // Ensure device is idle before starting playback
    EnsureIdleForMedia();

    ESP_LOGI(TAG, "PlayRadioUrl: url='%s' name='%s'", url.c_str(), station_name.c_str());
    return radio_->PlayUrl(url, station_name);
}

bool Application::PlaySdMedia(const std::string& keyword, bool is_video) {
#ifdef CONFIG_SD_CARD_ENABLE
    if (is_video) {
        if (!sd_video_) {
            ESP_LOGW(TAG, "PlaySdMedia: video player not initialized");
            return false;
        }
        auto state = sd_video_->GetState();
        if (state == VideoPlayerState::Idle || state == VideoPlayerState::Stopping) {
            size_t found = sd_video_->ScanDirectory();
            if (found == 0) {
                ESP_LOGW(TAG, "PlaySdMedia: no AVI files found");
                return false;
            }
            for (const auto& entry : sd_video_->GetPlaylist()) {
                if (entry.path.find(keyword) != std::string::npos) {
                    return PlayVideo(entry.path);
                }
            }
            ESP_LOGW(TAG, "PlaySdMedia: no AVI matching '%s'", keyword.c_str());
            return false;
        }
        return false;
    }

    // Audio from SD card
    if (!sd_music_) {
        ESP_LOGW(TAG, "SD music module not available");
        return false;
    }

    // Stop other media before playing SD music
    StopOtherMedia(MediaComponent::kSdMusic);

    // Ensure device is idle before starting playback
    EnsureIdleForMedia();

    ESP_LOGI(TAG, "PlaySdMedia: keyword='%s'", keyword.c_str());
    if (sd_music_->GetTotalTracks() == 0) {
        sd_music_->LoadPlaylist();
    }
    return sd_music_->PlayByName(keyword);
#else
    ESP_LOGW(TAG, "SD card support not enabled");
    return false;
#endif
}

bool Application::PlayVideo(const std::string& file_path) {
#ifdef CONFIG_SD_CARD_ENABLE
    if (!sd_video_) {
        ESP_LOGE(TAG, "PlayVideo: video player not initialized");
        return false;
    }
    if (sd_video_->GetState() == VideoPlayerState::Error) {
        ESP_LOGE(TAG, "PlayVideo: VideoPlayer in error state");
        return false;
    }

    // Stop other media before playing video
    StopOtherMedia(MediaComponent::kVideo);

    // Ensure device is idle before starting playback
    EnsureIdleForMedia();

    ESP_LOGI(TAG, "PlayVideo: path='%s'", file_path.c_str());
    sd_video_->Play(file_path);
    return true;
#else
    ESP_LOGW(TAG, "SD card support not enabled");
    return false;
#endif
}

void Application::StopAllMedia() {
    ESP_LOGI(TAG, "StopAllMedia");
    StopOtherMedia();
}

bool Application::ToggleIdleTheme() {
#if CONFIG_ENABLE_IDLE_SCREEN
    if (idle_screen_) {
        idle_screen_->ToggleTheme();
        return true;
    }
#endif
    return false;
}

bool Application::ToggleMainMenu() {
#if CONFIG_ENABLE_IDLE_SCREEN
    if (idle_screen_) {
        if (!idle_screen_->IsMainMenuVisible()) {
            CloseMenuPicker();
        }
        return idle_screen_->ToggleMainMenu();
    }
#endif
    return false;
}

bool Application::IsMainMenuVisible() const {
#if CONFIG_ENABLE_IDLE_SCREEN
    return idle_screen_ && idle_screen_->IsMainMenuVisible();
#else
    return false;
#endif
}

bool Application::MoveMainMenu(int delta) {
#if CONFIG_ENABLE_IDLE_SCREEN
    if (idle_screen_) {
        if (menu_picker_ctx_ != MenuPickerContext::None) {
            return MoveMenuPicker(delta);
        }
        return idle_screen_->MoveMainMenu(delta);
    }
#endif
    return false;
}

std::string Application::GetSelectedMainMenuLabel() const {
#if CONFIG_ENABLE_IDLE_SCREEN
    if (!idle_screen_) {
        return "Menu";
    }
    switch (idle_screen_->GetMainMenuItem()) {
        case IdleScreen::MainMenuItem::AiTalk:
            return "AI Talk";
        case IdleScreen::MainMenuItem::SdMusic:
            return "Music";
        case IdleScreen::MainMenuItem::OnlineMusic:
            return "Online";
        case IdleScreen::MainMenuItem::Radio:
            return "Radio";
        case IdleScreen::MainMenuItem::AlarmClock:
            return "Alarm";
        case IdleScreen::MainMenuItem::Setting:
            return "Setting";
        default:
            return "Menu";
    }
#else
    return "Menu";
#endif
}

bool Application::IsSdMusicPlaybackMode() const {
#ifdef CONFIG_SD_CARD_ENABLE
    if (!sd_music_) {
        return false;
    }
    if (sd_music_->IsPlaying()) {
        return true;
    }
    auto st = sd_music_->GetState();
    // Use real player state instead of UI-only mode flag so key mapping
    // remains stable during long playback / auto-next transitions.
    if (st == Esp32SdMusic::PlayerState::Preparing) {
        return true;
    }
    return st == Esp32SdMusic::PlayerState::Playing ||
           st == Esp32SdMusic::PlayerState::Paused;
#else
    return false;
#endif
}

bool Application::IsRadioPlaybackMode() const {
    if (!radio_) {
        return false;
    }
    return radio_->IsPlaying() || radio_->IsDownloading();
}

bool Application::SdMusicNextTrackFromMenu() {
#ifdef CONFIG_SD_CARD_ENABLE
    if (!IsSdMusicPlaybackMode() || !sd_music_) {
        return false;
    }
    return sd_music_->Next();
#else
    return false;
#endif
}

bool Application::SdMusicPrevTrackFromMenu() {
#ifdef CONFIG_SD_CARD_ENABLE
    if (!IsSdMusicPlaybackMode() || !sd_music_) {
        return false;
    }
    return sd_music_->Prev();
#else
    return false;
#endif
}

bool Application::RadioNextStationFromMenu() {
    if (!IsRadioPlaybackMode() || !radio_) {
        return false;
    }
    return radio_->NextStation();
}

bool Application::RadioPrevStationFromMenu() {
    if (!IsRadioPlaybackMode() || !radio_) {
        return false;
    }
    return radio_->PrevStation();
}

bool Application::ExitSdMusicPlaybackToMainMenu() {
#ifdef CONFIG_SD_CARD_ENABLE
    if (sd_music_) {
        sd_music_->Stop();
    }
    SetSdMusicPlaybackMode(false);
#if CONFIG_ENABLE_IDLE_SCREEN
    if (idle_screen_ && !idle_screen_->IsMainMenuVisible()) {
        return idle_screen_->ToggleMainMenu();
    }
#endif
    return true;
#else
    return false;
#endif
}

bool Application::OpenMenuPicker(MenuPickerContext ctx, const std::string& title,
    const std::vector<std::string>& items, int initial_index,
    const std::vector<int>& index_map) {
#if CONFIG_ENABLE_IDLE_SCREEN
    (void)title;
    if (!idle_screen_ || !idle_screen_->IsMainMenuVisible() || items.empty()) {
        return false;
    }
    menu_picker_ctx_ = ctx;
    menu_picker_items_ = items;
    menu_picker_map_ = index_map;
    if (initial_index < 0 || initial_index >= static_cast<int>(menu_picker_items_.size())) {
        initial_index = 0;
    }
    menu_picker_index_ = initial_index;
    return RefreshMenuPickerDisplay();
#else
    return false;
#endif
}

void Application::CloseMenuPicker() {
#if CONFIG_ENABLE_IDLE_SCREEN
    if (menu_picker_ctx_ == MenuPickerContext::ThemeSettingAction && idle_screen_) {
        if (original_theme_ != -1) {
            idle_screen_->PreviewTheme(static_cast<IdleScreen::Theme>(original_theme_));
        }
    }
#endif
    menu_picker_ctx_ = MenuPickerContext::None;
    menu_picker_items_.clear();
    menu_picker_map_.clear();
    menu_picker_index_ = 0;
#if CONFIG_USE_ALARM
    alarm_action_entry_valid_ = false;
#endif
#if CONFIG_ENABLE_IDLE_SCREEN
    if (idle_screen_) {
        idle_screen_->ClearMainMenuPicker();
    }
#endif
}

bool Application::RefreshMenuPickerDisplay() {
#if CONFIG_ENABLE_IDLE_SCREEN
    if (!idle_screen_ || menu_picker_ctx_ == MenuPickerContext::None || menu_picker_items_.empty()) {
        return false;
    }
    std::string title;
    switch (menu_picker_ctx_) {
        case MenuPickerContext::SdDirectory:   title = "Music Folder"; break;
        case MenuPickerContext::SdTrack:       title = "Pick a Song"; break;
        case MenuPickerContext::RadioStation:  title = "Radio"; break;
        case MenuPickerContext::AlarmList:     title = "Alarm"; break;
        case MenuPickerContext::AlarmAction:   title = "Alarm Edit"; break;
        case MenuPickerContext::AlarmEditTime: title = "Set Time"; break;
        case MenuPickerContext::AlarmDeleteConfirm: title = "Delete alarm?"; break;
        case MenuPickerContext::SettingAction: title = "Setting"; break;
        case MenuPickerContext::LcdSettingAction: title = "LCD Setting"; break;
        case MenuPickerContext::ThemeSettingAction: title = "Theme"; break;
        case MenuPickerContext::LcdSwitchConfirm: title = "Apply LCD?"; break;
        case MenuPickerContext::SdReloadConfirm: title = "Reload Music?"; break;
        case MenuPickerContext::PowerAction: title = "Power"; break;
        case MenuPickerContext::PowerTimeoutAction: title = "Power Timer"; break;
        case MenuPickerContext::DeviceInfo: title = "About speaker"; break;
        case MenuPickerContext::RestartConfirm: title = "Restart?"; break;
        case MenuPickerContext::ShutdownConfirm: title = "Shut Down?"; break;
        case MenuPickerContext::WifiAccessPoint: title = "Wi-Fi"; break;
        case MenuPickerContext::WifiAddConfirm: title = "Add WiFi?"; break;
        case MenuPickerContext::OtaSettingAction: title = "Custom OTA"; break;
        case MenuPickerContext::WifiResetConfirm: title = "Reset WiFi?"; break;
        case MenuPickerContext::SoundScreenAction: title = "Sound & Screen"; break;
        case MenuPickerContext::SystemAction: title = "System"; break;
        case MenuPickerContext::VolumeAdjust: title = "Volume"; break;
        case MenuPickerContext::BrightnessAdjust: title = "Brightness"; break;
        default: return false;
    }
    std::string value;
    if (menu_picker_ctx_ == MenuPickerContext::SettingAction ||
        menu_picker_ctx_ == MenuPickerContext::LcdSettingAction ||
        menu_picker_ctx_ == MenuPickerContext::ThemeSettingAction ||
        menu_picker_ctx_ == MenuPickerContext::LcdSwitchConfirm ||
        menu_picker_ctx_ == MenuPickerContext::PowerAction ||
        menu_picker_ctx_ == MenuPickerContext::PowerTimeoutAction ||
        menu_picker_ctx_ == MenuPickerContext::DeviceInfo ||
        menu_picker_ctx_ == MenuPickerContext::AlarmAction ||
        menu_picker_ctx_ == MenuPickerContext::AlarmEditTime ||
        menu_picker_ctx_ == MenuPickerContext::AlarmDeleteConfirm ||
        menu_picker_ctx_ == MenuPickerContext::RestartConfirm ||
        menu_picker_ctx_ == MenuPickerContext::ShutdownConfirm ||
        menu_picker_ctx_ == MenuPickerContext::SdReloadConfirm ||
        menu_picker_ctx_ == MenuPickerContext::OtaSettingAction ||
        menu_picker_ctx_ == MenuPickerContext::WifiResetConfirm ||
        menu_picker_ctx_ == MenuPickerContext::WifiAddConfirm ||
        menu_picker_ctx_ == MenuPickerContext::SoundScreenAction ||
        menu_picker_ctx_ == MenuPickerContext::SystemAction) {
        const int count = static_cast<int>(menu_picker_items_.size());
        int local_page_size = 5;
        if (menu_picker_ctx_ == MenuPickerContext::ThemeSettingAction) {
            const bool use_paging = (count > local_page_size);
            const int page_start = use_paging ? (menu_picker_index_ / local_page_size) * local_page_size : 0;
            const int page_end = use_paging ? std::min(page_start + local_page_size, count) : count;
            for (int i = page_start; i < page_end; ++i) {
                const bool selected = (i == menu_picker_index_);
                if (i == 0) { // Orange / Tiger Cub
                    value += selected ? "#D66A2A > " : "#F08B3A   ";
                } else if (i == 1) { // Pink / Bunny Nose
                    value += selected ? "#F06292 > " : "#FF9AB7   ";
                } else if (i == 2) { // Blue / Dino Blue
                    value += selected ? "#1E7BFF > " : "#4EA9DA   ";
                } else if (i == 3) { // Yellow / Honey Bee
                    value += selected ? "#D4AC0D > " : "#F4D03F   ";
                } else { // Back
                    value += selected ? "#1E7BFF > " : "#5E7E9E   ";
                }
                value += menu_picker_items_[i];
                value += " #\n";
            }
            if (use_paging) {
                value += "#8AA8C6 Page ";
                value += std::to_string((menu_picker_index_ / local_page_size) + 1);
                value += "/";
                value += std::to_string((count + local_page_size - 1) / local_page_size);
                value += " #";
            }
        } else {
            const bool use_paging = (count > local_page_size);
            const int page_start = use_paging ? (menu_picker_index_ / local_page_size) * local_page_size : 0;
            const int page_end = use_paging ? std::min(page_start + local_page_size, count) : count;
            for (int i = page_start; i < page_end; ++i) {
                const bool selected = (i == menu_picker_index_);
                if (selected) {
                    value += "#1E7BFF > ";
                    value += menu_picker_items_[i];
                    value += " #\n";
                } else {
                    value += "#5E7E9E   ";
                    value += menu_picker_items_[i];
                    value += " #\n";
                }
            }
            if (use_paging) {
                value += "#8AA8C6 Page ";
                value += std::to_string((menu_picker_index_ / local_page_size) + 1);
                value += "/";
                value += std::to_string((count + local_page_size - 1) / local_page_size);
                value += " #";
            }
        }
    } else if (menu_picker_ctx_ == MenuPickerContext::SdDirectory ||
               menu_picker_ctx_ == MenuPickerContext::SdTrack ||
               menu_picker_ctx_ == MenuPickerContext::RadioStation ||
               menu_picker_ctx_ == MenuPickerContext::AlarmList ||
               menu_picker_ctx_ == MenuPickerContext::WifiAccessPoint) {
        int local_kPageSize = 4;
        if (menu_picker_ctx_ == MenuPickerContext::WifiAccessPoint) {
            local_kPageSize = 5;
        }
        const int count = static_cast<int>(menu_picker_items_.size());
        const int page_start = (menu_picker_index_ / local_kPageSize) * local_kPageSize;
        const int page_end = std::min(page_start + local_kPageSize, count);
        const bool is_dir = (menu_picker_ctx_ == MenuPickerContext::SdDirectory);
        const bool is_alarm = (menu_picker_ctx_ == MenuPickerContext::AlarmList);
        const bool is_wifi = (menu_picker_ctx_ == MenuPickerContext::WifiAccessPoint);
        for (int i = page_start; i < page_end; ++i) {
            const bool selected = (i == menu_picker_index_);
            if (selected) {
                value += is_alarm ? "#FF7A59 > " :
                         (is_dir ? "#1E7BFF > [DIR] " :
                         (is_wifi ? "#1E7BFF > " : "#1E7BFF > "));
                value += menu_picker_items_[i];
                value += " #\n";
            } else {
                value += is_alarm ? "#5E7E9E   " :
                         (is_dir ? "#4F6B88   [DIR] " :
                         (is_wifi ? "#4F6B88   " : "#4F6B88   "));
                value += menu_picker_items_[i];
                value += " #\n";
            }
        }
        if (count > local_kPageSize) {
            value += "#8AA8C6 Page ";
            value += std::to_string((menu_picker_index_ / local_kPageSize) + 1);
            value += "/";
            value += std::to_string((count + local_kPageSize - 1) / local_kPageSize);
            value += " #";
        }
    } else if (menu_picker_ctx_ == MenuPickerContext::VolumeAdjust) {
        auto codec = Board::GetInstance().GetAudioCodec();
        int vol_raw = codec->output_volume();
        int vol_pct = (vol_raw * 100) / 80;
        int knob_pos = (vol_pct * 14) / 100;
        std::string slider = "[";
        for (int i = 0; i < 15; ++i) {
            if (i == knob_pos) slider += "O";
            else if (i < knob_pos) slider += "=";
            else slider += "-";
        }
        slider += "]";
        value = "#1E7BFF " + slider + " " + std::to_string(vol_pct) + "% #\n\n#5E7E9E Vol+/Vol- to adjust\nPress Power to OK #\n";
    } else if (menu_picker_ctx_ == MenuPickerContext::BrightnessAdjust) {
        auto backlight = Board::GetInstance().GetBacklight();
        int bri_pct = backlight->brightness();
        int knob_pos = (bri_pct * 14) / 100;
        std::string slider = "[";
        for (int i = 0; i < 15; ++i) {
            if (i == knob_pos) slider += "O";
            else if (i < knob_pos) slider += "=";
            else slider += "-";
        }
        slider += "]";
        value = "#1E7BFF " + slider + " " + std::to_string(bri_pct) + "% #\n\n#5E7E9E Vol+/Vol- to adjust\nPress Power to OK #\n";
    } else {
        value = menu_picker_items_[menu_picker_index_];
    }
    return idle_screen_->SetMainMenuPicker(title, value);
#else
    return false;
#endif
}

bool Application::MoveMenuPicker(int delta) {
    if (menu_picker_ctx_ == MenuPickerContext::None) {
        return false;
    }

    if (menu_picker_ctx_ == MenuPickerContext::VolumeAdjust) {
        auto codec = Board::GetInstance().GetAudioCodec();
        int current_vol = codec->output_volume();
        int step = 4; // 5% of 80 is 4
        if (delta == -1) {
            current_vol = std::min(current_vol + step, 80);
        } else {
            current_vol = std::max(current_vol - step, 0);
        }
        codec->SetOutputVolume(current_vol);
        return RefreshMenuPickerDisplay();
    }

    if (menu_picker_ctx_ == MenuPickerContext::BrightnessAdjust) {
        auto backlight = Board::GetInstance().GetBacklight();
        int current_bri = backlight->brightness();
        int step = 5; // 5%
        if (delta == -1) {
            current_bri = std::min(current_bri + step, 100);
        } else {
            current_bri = std::max(current_bri - step, 5); // min 5% to keep screen visible
        }
        backlight->SetBrightness(current_bri, true);
        return RefreshMenuPickerDisplay();
    }

    if (menu_picker_items_.empty()) {
        return false;
    }
    int count = static_cast<int>(menu_picker_items_.size());
    menu_picker_index_ = (menu_picker_index_ + delta) % count;
    if (menu_picker_index_ < 0) {
        menu_picker_index_ += count;
    }
    if (menu_picker_ctx_ == MenuPickerContext::ThemeSettingAction) {
        ApplyLiveThemeSelection();
    }
    return RefreshMenuPickerDisplay();
}

bool Application::StartAiTalkFromMenu() {
    StopAllMedia();
    if (device_state_ != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }

    // Make AI Talk deterministic for English practice:
    // ask backend to respond with a friendly, child-oriented greeting.
    Settings wifi_settings("wifi", false);
    std::string child_name = wifi_settings.GetString("child_name", "Buddy");
    if (child_name.empty()) {
        child_name = "Buddy";
    }
    if (child_name.size() > 20) {
        child_name = child_name.substr(0, 20);
    }

    std::string wake_word =
        "Please greet the child exactly like this: Welcome back " + child_name +
        ", what would you like to practice today?";

    WakeWordInvoke(wake_word);
    Alert("AI Talk", "English practice mode started.", "microchip_ai");
    return true;
}

bool Application::StartRandomSdMusicFromMenu() {
#ifdef CONFIG_SD_CARD_ENABLE
    if (!sd_music_) {
        Alert("Music", "SD music is not available.", "sd_card");
        return false;
    }
    if (!sd_music_->SetDirectory("/")) {
        Alert("Music", "No SD music folder found.", "sd_card");
        return false;
    }

    const size_t total = sd_music_->GetTotalTracks();
    if (total == 0) {
        Alert("Music", "No songs found on SD card.", "sd_card");
        return false;
    }

    StopOtherMedia(MediaComponent::kSdMusic);
    EnsureIdleForMedia();
    sd_music_->SetShuffleMode(true);
    sd_music_->SetRepeatMode(Esp32SdMusic::RepeatMode::RepeatAll);

    const int random_index = static_cast<int>(esp_random() % total);
    bool ok = sd_music_->SetTrack(random_index);
    if (ok) {
        SetSdMusicPlaybackMode(true);
        Alert("Music", "Playing a random song from SD card.", "music");
    }
    return ok;
#else
    Alert("Music", "SD card is not enabled.", "sd_card");
    return false;
#endif
}

bool Application::StartOnlineMusicPromptFromMenu() {
    StopAllMedia();
    if (device_state_ != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }

    // Send a direct online-music intent so AI can respond immediately.
    WakeWordInvoke("can you play a song in online?");
    return true;
}

bool Application::StartRandomRadioFromMenu() {
    if (!radio_) {
        Alert("Radio", "Radio is not available.", "radio");
        return false;
    }
    auto stations = radio_->GetStationList();
    if (stations.empty()) {
        Alert("Radio", "No radio stations configured.", "radio");
        return false;
    }

    const size_t random_index = esp_random() % stations.size();
    bool ok = PlayRadio(stations[random_index]);
    if (ok) {
        Alert("Radio", "Playing a random radio station.", "radio");
    }
    return ok;
}

bool Application::StartAlarmPromptFromMenu() {
    StopAllMedia();
#if CONFIG_USE_ALARM
    waiting_alarm_from_ai_ = true;
#endif
    if (device_state_ != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }
    // Trigger AI with a clear alarm-setup intent to avoid generic fallback replies.
    WakeWordInvoke("please help me set up an alarm and ask only time plus label");
    Alert("Alarm", "Tell time and label, e.g. Homework at 18:30.", "alarm_clock");
    return true;
}

bool Application::StartDeviceInfoPromptFromMenu() {
    waiting_device_id_from_ai_ = true;
    if (device_state_ != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }
    WakeWordInvoke("what is your device ID?");
    Alert("About speaker", "Asking AI for Device ID...", "microchip_ai");
    return true;
}

bool Application::OpenDeviceInfoPicker() {
    std::string device_id = ResolvePairDeviceId();
    if (device_id.empty()) {
        device_id = "Syncing...";
    }
    std::string mac = SystemInfo::GetMacAddress();
    auto app_desc = esp_app_get_description();
    std::string fw_version = app_desc ? std::string(app_desc->version) : std::string("unknown");
    std::vector<std::string> items;
    items.reserve(5);
    items.push_back("Device ID: " + device_id);
    items.push_back("Mac Address: " + mac);
    items.push_back("Firmware: " + fw_version);
    items.push_back("Model: Globy Pro");
    items.push_back("Back");

    std::vector<int> map = {0, 1, 2, 3, -1};
    return OpenMenuPicker(MenuPickerContext::DeviceInfo, "About speaker", items, 0, map);
}

bool Application::OpenSdDirectoryPicker() {
#ifdef CONFIG_SD_CARD_ENABLE
    if (!sd_music_) {
        return false;
    }
    auto dirs = sd_music_->GetDirectories();
    std::vector<std::string> items;
    std::vector<int> map;
    items.reserve(dirs.size() + 1);
    map.reserve(dirs.size() + 1);
    for (size_t i = 0; i < dirs.size(); ++i) {
        const auto& d = dirs[i];
        std::string name = d;
        if (!name.empty() && name[0] == '/') {
            auto slash = name.find_last_of('/');
            if (slash != std::string::npos) {
                name = name.substr(slash + 1);
            }
        }
        if (!name.empty() && name[0] == '.') {
            continue;
        }
        items.push_back(d);
        map.push_back(static_cast<int>(i));
    }
    if (items.empty()) {
        Alert("Music", "No music folder found.", "sd_card");
        return false;
    }
    items.push_back("Reload Music Library");
    map.push_back(-3);
    items.push_back("Exit");
    map.push_back(-1);
    return OpenMenuPicker(MenuPickerContext::SdDirectory, "SD Folder", items, 0, map);
#else
    return false;
#endif
}

bool Application::OpenSdTrackPicker(const std::string& relative_dir) {
#ifdef CONFIG_SD_CARD_ENABLE
    if (!sd_music_) {
        return false;
    }
    if (!sd_music_->SetDirectory(relative_dir)) {
        return false;
    }

    auto playlist = sd_music_->GetPlaylist();
    if (playlist.empty()) {
        return false;
    }

    std::vector<std::string> items;
    std::vector<int> map;
    items.reserve(playlist.size() + 1);
    map.reserve(playlist.size() + 1);
    for (size_t i = 0; i < playlist.size(); ++i) {
        const auto& track = playlist[i];
        std::string name = !track.title.empty() ? track.title : track.name;
        // UI preference: always display as "<title>.mp3" for consistency.
        auto dot = name.find_last_of('.');
        if (dot != std::string::npos) {
            name = name.substr(0, dot);
        }
        name += ".mp3";
        items.push_back(name);
        map.push_back(static_cast<int>(i));
    }
    items.push_back("Exit");
    map.push_back(-1);
    sd_selected_directory_ = relative_dir;
    return OpenMenuPicker(MenuPickerContext::SdTrack, "SD Track", items, 0, map);
#else
    (void)relative_dir;
    return false;
#endif
}

bool Application::OpenRadioPicker() {
    if (!radio_) {
        return false;
    }
    auto stations = radio_->GetStationList();
    if (stations.empty()) {
        return false;
    }
    std::vector<std::string> items;
    std::vector<int> map;
    items.reserve(stations.size() + 1);
    map.reserve(stations.size() + 1);
    for (size_t i = 0; i < stations.size(); ++i) {
        items.push_back(stations[i]);
        map.push_back(static_cast<int>(i));
    }
    items.push_back("Exit");
    map.push_back(-1);
    return OpenMenuPicker(MenuPickerContext::RadioStation, "Radio", items, 0, map);
}

bool Application::RefreshAlarmListCache() {
#if CONFIG_USE_ALARM
    alarm_menu_entries_.clear();
    menu_picker_items_.clear();
    if (!general_timer_) {
        return false;
    }

    std::vector<alarm_snapshot_t> snapshots;
    general_timer_->ListUpcomingAlarms(snapshots, 256, false);
    // Keep alarm list concise: hide duplicate rows that have the same
    // "HH:MM + message" (common when weekday/weekend presets overlap).
    std::unordered_set<std::string> seen_active_keys;
    for (const auto& snapshot : snapshots) {
        struct tm tm_info;
        localtime_r(&snapshot.trigger_time, &tm_info);
        char time_buf[8] = {0};
        strftime(time_buf, sizeof(time_buf), "%H:%M", &tm_info);

        std::string msg = snapshot.message.empty() ? "Alarm" : snapshot.message;
        std::string dedup_key;
        dedup_key.reserve(msg.size() + 8);
        dedup_key.append(time_buf);
        dedup_key.push_back('|');
        dedup_key.append(msg);
        if (seen_active_keys.find(dedup_key) != seen_active_keys.end()) {
            continue;
        }
        seen_active_keys.insert(std::move(dedup_key));

        AlarmMenuEntry entry;
        entry.active = true;
        entry.timer_index = snapshot.index;
        entry.repeat_seconds = snapshot.repeat_seconds;
        entry.trigger_time = snapshot.trigger_time;
        entry.message = snapshot.message;
        alarm_menu_entries_.push_back(std::move(entry));
    }

    for (size_t i = 0; i < disabled_alarm_entries_.size(); ++i) {
        AlarmMenuEntry entry;
        entry.active = false;
        entry.disabled_index = static_cast<int>(i);
        entry.trigger_time = disabled_alarm_entries_[i].trigger_time;
        entry.repeat_seconds = disabled_alarm_entries_[i].repeat_seconds;
        entry.message = disabled_alarm_entries_[i].message;
        alarm_menu_entries_.push_back(std::move(entry));
    }

    std::sort(alarm_menu_entries_.begin(), alarm_menu_entries_.end(),
              [](const AlarmMenuEntry& a, const AlarmMenuEntry& b) {
                  return a.trigger_time < b.trigger_time;
              });

    for (const auto& entry : alarm_menu_entries_) {
        char time_buf[8] = {0};
        struct tm tm_info;
        localtime_r(&entry.trigger_time, &tm_info);
        strftime(time_buf, sizeof(time_buf), "%H:%M", &tm_info);

        std::string row = entry.active ? "[ON] " : "[OFF] ";
        row += time_buf;
        row += " ";
        row += entry.message.empty() ? "Alarm" : entry.message;
        menu_picker_items_.push_back(std::move(row));
    }
    menu_picker_items_.push_back("+ Add Alarm");
    menu_picker_items_.push_back("Exit");
    return true;
#else
    return false;
#endif
}

bool Application::OpenAlarmListPicker() {
#if CONFIG_USE_ALARM
    if (IsAlarmDisabledForBoard()) {
        return false;
    }
    if (!RefreshAlarmListCache()) {
        return false;
    }
    if (menu_picker_items_.empty()) {
        menu_picker_items_.push_back("+ Add Alarm");
        menu_picker_items_.push_back("Exit");
    }
    std::vector<int> map;
    map.reserve(menu_picker_items_.size());
    for (size_t i = 0; i < alarm_menu_entries_.size(); ++i) {
        map.push_back(static_cast<int>(i));
    }
    map.push_back(-1); // + Add Alarm
    map.push_back(-2); // Exit
    return OpenMenuPicker(MenuPickerContext::AlarmList, "Alarm", menu_picker_items_, 0, map);
#else
    return StartAlarmPromptFromMenu();
#endif
}

bool Application::OpenAlarmActionPicker() {
#if CONFIG_USE_ALARM
    if (!alarm_action_entry_valid_) {
        return false;
    }
    if (!alarm_action_entry_.active &&
        (alarm_action_entry_.disabled_index < 0 ||
         alarm_action_entry_.disabled_index >= static_cast<int>(disabled_alarm_entries_.size()))) {
        ESP_LOGW(TAG, "OpenAlarmActionPicker: invalid disabled_index=%d",
                 alarm_action_entry_.disabled_index);
        return false;
    }
    time_t base_time = alarm_action_entry_.trigger_time;
    if (!alarm_action_entry_.active &&
        alarm_action_entry_.disabled_index >= 0 &&
        alarm_action_entry_.disabled_index < static_cast<int>(disabled_alarm_entries_.size())) {
        base_time = disabled_alarm_entries_[alarm_action_entry_.disabled_index].trigger_time;
    }
    struct tm tm_info{};
    localtime_r(&base_time, &tm_info);
    alarm_edit_hour_ = tm_info.tm_hour;
    alarm_edit_minute_ = tm_info.tm_min;

    std::vector<std::string> items;
    if (alarm_action_entry_.active) {
        items = {"Turn Off", "Edit", "Save", "Delete", "Back"};
    } else {
        items = {"Turn On", "Edit", "Save", "Delete", "Back"};
    }
    return OpenMenuPicker(MenuPickerContext::AlarmAction, "Alarm Edit", items, 0);
#else
    return false;
#endif
}

bool Application::OpenAlarmDeleteConfirmPicker() {
#if CONFIG_USE_ALARM
    std::vector<std::string> items = {"Yes", "No"};
    return OpenMenuPicker(MenuPickerContext::AlarmDeleteConfirm, "Delete alarm?", items, 1);
#else
    return false;
#endif
}

bool Application::AddAlarmEventAt(time_t trigger_time, const std::string& message, int repeat_seconds) {
#if CONFIG_USE_ALARM
    if (!general_timer_) {
        return false;
    }
    const std::string final_message = message.empty() ? "Alarm" : message;
    char* msg = static_cast<char*>(malloc(final_message.size() + 1));
    if (msg == nullptr) {
        return false;
    }
    strcpy(msg, final_message.c_str());

    if (repeat_seconds > 0) {
        general_timer_->TimerAddTimerEventRepeat(trigger_time, E_PET_TIMER_FUNCTION, 1, msg, repeat_seconds, 0, 0);
    } else {
        general_timer_->TimerAddTimerEventAbsolute(trigger_time, E_PET_TIMER_FUNCTION, 1, msg, false);
    }
    return true;
#else
    (void)trigger_time;
    (void)message;
    (void)repeat_seconds;
    return false;
#endif
}

void Application::LoadDefaultAlarmsIfEmpty() {
#if CONFIG_USE_ALARM
    if (IsAlarmDisabledForBoard()) {
        return;
    }
    if (!general_timer_) {
        return;
    }
    std::vector<alarm_snapshot_t> existing;
    struct DailyPreset { const char* label; int hour; int minute; };
    static const DailyPreset kDailyPresets[] = {
        {"Wake up", 6, 0},
        {"Breakfast", 7, 0},
        {"Lunch", 11, 30},
        {"Dinner", 18, 30},
        {"Bedtime", 21, 30},
    };
    struct WeeklyPreset { const char* label; int hour; int minute; const char* days[5]; int count; };
    static const WeeklyPreset kWeeklyPresets[] = {
        {"School", 6, 45, {"mon", "tue", "wed", "thu", "fri"}, 5},
    };

    // If alarms already exist, normalize default preset labels to canonical times.
    // This fixes legacy shifted times (e.g. wrong timezone migration) without
    // deleting user-defined custom alarms.
    if (general_timer_->ListUpcomingAlarms(existing, 256, false) && !existing.empty()) {
        auto to_lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        auto find_daily_preset = [&](const std::string& msg, int& h, int& m) -> bool {
            std::string key = to_lower(msg);
            for (const auto& p : kDailyPresets) {
                if (key == to_lower(p.label)) {
                    h = p.hour;
                    m = p.minute;
                    return true;
                }
            }
            return false;
        };
        auto is_weekly_school = [&](const std::string& msg) -> bool {
            return to_lower(msg) == "school";
        };

        for (const auto& snap : existing) {
            if (snap.index < 0) continue;
            int h = -1;
            int m = -1;
            if (find_daily_preset(snap.message, h, m)) {
                if (!general_timer_->UpdateAlarmByIndex(
                        snap.index,
                        BuildNextLocalTime(h, m, -1),
                        snap.message.empty() ? "Alarm" : snap.message)) {
                    ESP_LOGW(TAG, "Failed to normalize daily preset '%s'", snap.message.c_str());
                }
                continue;
            }
            if (is_weekly_school(snap.message)) {
                struct tm tm_info{};
                localtime_r(&snap.trigger_time, &tm_info);
                const int weekday = tm_info.tm_wday; // 0..6
                if (!general_timer_->UpdateAlarmByIndex(
                        snap.index,
                        BuildNextLocalTime(kWeeklyPresets[0].hour, kWeeklyPresets[0].minute, weekday),
                        snap.message.empty() ? "Alarm" : snap.message)) {
                    ESP_LOGW(TAG, "Failed to normalize weekly preset '%s'", snap.message.c_str());
                }
            }
        }
        return;
    }

    for (const auto& preset : kDailyPresets) {
        AddAlarmEventAt(BuildNextLocalTime(preset.hour, preset.minute, -1),
                        preset.label, SECOND_ONE_DAY);
    }
    for (const auto& preset : kWeeklyPresets) {
        for (int i = 0; i < preset.count; ++i) {
            int weekday = ParseWeekdayShort(preset.days[i]);
            if (weekday >= 0) {
                AddAlarmEventAt(BuildNextLocalTime(preset.hour, preset.minute, weekday),
                                preset.label, SECOND_ONE_WEEK);
            }
        }
    }
#endif
}

bool Application::TryCreateAlarmFromText(const std::string& text) {
#if CONFIG_USE_ALARM
    if (IsAlarmDisabledForBoard()) {
        waiting_alarm_from_ai_ = false;
        return false;
    }
    if (!waiting_alarm_from_ai_ || text.empty()) {
        return false;
    }

    int hour = -1;
    int minute = -1;
    for (size_t i = 0; i + 4 < text.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 1])) &&
            text[i + 2] == ':' &&
            std::isdigit(static_cast<unsigned char>(text[i + 3])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 4]))) {
            hour = (text[i] - '0') * 10 + (text[i + 1] - '0');
            minute = (text[i + 3] - '0') * 10 + (text[i + 4] - '0');
            if (hour >= 0 && hour < 24 && minute >= 0 && minute < 60) {
                break;
            }
            hour = -1;
            minute = -1;
        }
    }
    if (hour < 0 || minute < 0) {
        return false;
    }

    std::string label = "Alarm";
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t for_pos = lower.find("for ");
    size_t at_pos = lower.find(" at ");
    if (for_pos != std::string::npos && at_pos != std::string::npos && at_pos > for_pos + 4) {
        label = text.substr(for_pos + 4, at_pos - (for_pos + 4));
    }
    if (label.size() > 24) {
        label.resize(24);
    }
    while (!label.empty() && (label.back() == '.' || label.back() == ',' || label.back() == ' ')) {
        label.pop_back();
    }
    if (label.empty()) {
        label = "Alarm";
    }

    if (!AddAlarmEventAt(BuildNextLocalTime(hour, minute, -1), label, SECOND_ONE_DAY)) {
        return false;
    }
    waiting_alarm_from_ai_ = false;

    char time_buf[8];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", hour, minute);
    std::string note = "Alarm added ";
    note += time_buf;
    Alert("Alarm", note.c_str(), "alarm_clock");

    if (menu_picker_ctx_ == MenuPickerContext::AlarmList) {
        RefreshAlarmListCache();
        RefreshMenuPickerDisplay();
    }
    return true;
#else
    (void)text;
    return false;
#endif
}

bool Application::ApplySelectedAlarmAction() {
#if CONFIG_USE_ALARM
    if (!alarm_action_entry_valid_ || !general_timer_) {
        return false;
    }
    if (menu_picker_index_ < 0 || menu_picker_index_ >= static_cast<int>(menu_picker_items_.size())) {
        return false;
    }

    if (menu_picker_ctx_ == MenuPickerContext::AlarmEditTime) {
        const bool hour_inc = (menu_picker_index_ == 0);
        const bool hour_dec = (menu_picker_index_ == 1);
        const bool min_inc  = (menu_picker_index_ == 2);
        const bool min_dec  = (menu_picker_index_ == 3);
        const bool save     = (menu_picker_index_ == 4);
        const bool back     = (menu_picker_index_ == 5);

        if (back) {
            return OpenAlarmActionPicker();
        }
        if (save) {
            if (!alarm_action_entry_.active &&
                (alarm_action_entry_.disabled_index < 0 ||
                 alarm_action_entry_.disabled_index >= static_cast<int>(disabled_alarm_entries_.size()))) {
                ESP_LOGW(TAG, "ApplySelectedAlarmAction(edit-save): invalid disabled_index=%d",
                         alarm_action_entry_.disabled_index);
                return false;
            }
            struct tm base_tm{};
            const time_t base_time = alarm_action_entry_.active
                                         ? alarm_action_entry_.trigger_time
                                         : disabled_alarm_entries_[alarm_action_entry_.disabled_index].trigger_time;
            localtime_r(&base_time, &base_tm);
            int h = alarm_edit_hour_;
            int m = alarm_edit_minute_;
            if (m >= 60) {
                m = 0;
                h += 1;
            }
            if (h >= 24) {
                h = 0;
                base_tm.tm_mday += 1;
            }
            base_tm.tm_hour = h;
            base_tm.tm_min = m;
            base_tm.tm_sec = 0;
            time_t new_time = mktime(&base_tm);
            const time_t now = time(nullptr);
            if (new_time <= now) {
                if (alarm_action_entry_.repeat_seconds > 0) {
                    while (new_time <= now) {
                        new_time += alarm_action_entry_.repeat_seconds;
                    }
                } else {
                    new_time = now + 60;
                }
            }
            if (alarm_action_entry_.active) {
                return general_timer_->UpdateAlarmByIndex(
                    alarm_action_entry_.timer_index,
                    new_time,
                    alarm_action_entry_.message);
            }
            auto& entry = disabled_alarm_entries_[alarm_action_entry_.disabled_index];
            entry.trigger_time = new_time;
            return true;
        }
        if (hour_inc) alarm_edit_hour_ = (alarm_edit_hour_ + 1) % 25;          // 0..24
        if (hour_dec) alarm_edit_hour_ = (alarm_edit_hour_ + 24) % 25;
        if (min_inc)  alarm_edit_minute_ = (alarm_edit_minute_ + 1) % 61;      // 0..60
        if (min_dec)  alarm_edit_minute_ = (alarm_edit_minute_ + 60) % 61;

        std::vector<std::string> items;
        char hbuf[24];
        char mbuf[24];
        snprintf(hbuf, sizeof(hbuf), "Hour: %02d", alarm_edit_hour_);
        snprintf(mbuf, sizeof(mbuf), "Minute: %02d", alarm_edit_minute_);
        items = {
            std::string(hbuf) + " (+1)",
            std::string(hbuf) + " (-1)",
            std::string(mbuf) + " (+1)",
            std::string(mbuf) + " (-1)",
            "Save",
            "Back"
        };
        return OpenMenuPicker(MenuPickerContext::AlarmEditTime, "Set Time", items, menu_picker_index_);
    }

    const bool is_turn_on_off = (menu_picker_index_ == 0);
    const bool is_edit_time = (menu_picker_index_ == 1);
    const bool is_save = (menu_picker_index_ == 2);
    const bool is_delete = (menu_picker_index_ == 3);
    const bool is_back = (menu_picker_index_ == 4);

    if (is_back) return true;

    if (is_turn_on_off) {
        if (alarm_action_entry_.active) {
            time_t removed_time = 0;
            std::string removed_message;
            if (!general_timer_->DeleteAlarmByIndex(alarm_action_entry_.timer_index, &removed_time, &removed_message)) {
                return false;
            }
            disabled_alarm_entries_.push_back({
                removed_time,
                alarm_action_entry_.repeat_seconds,
                removed_message.empty() ? "Alarm" : removed_message
            });
            return true;
        }

        if (alarm_action_entry_.disabled_index < 0 ||
            alarm_action_entry_.disabled_index >= static_cast<int>(disabled_alarm_entries_.size())) {
            return false;
        }
        auto entry = disabled_alarm_entries_[alarm_action_entry_.disabled_index];
        if (!AddAlarmEventAt(entry.trigger_time, entry.message, entry.repeat_seconds)) {
            return false;
        }
        disabled_alarm_entries_.erase(disabled_alarm_entries_.begin() + alarm_action_entry_.disabled_index);
        return true;
    }

    if (is_delete) {
        if (alarm_action_entry_.active) {
            if (alarm_action_entry_.timer_index < 0) {
                ESP_LOGW(TAG, "ApplySelectedAlarmAction(delete): invalid timer_index=%d",
                         alarm_action_entry_.timer_index);
                return false;
            }
            return general_timer_->DeleteAlarmByIndex(alarm_action_entry_.timer_index);
        }
        if (alarm_action_entry_.disabled_index < 0 ||
            alarm_action_entry_.disabled_index >= static_cast<int>(disabled_alarm_entries_.size())) {
            ESP_LOGW(TAG, "ApplySelectedAlarmAction(delete): invalid disabled_index=%d",
                     alarm_action_entry_.disabled_index);
            return false;
        }
        disabled_alarm_entries_.erase(disabled_alarm_entries_.begin() + alarm_action_entry_.disabled_index);
        return true;
    }
    if (is_save) {
        if (!alarm_action_entry_.active &&
            (alarm_action_entry_.disabled_index < 0 ||
             alarm_action_entry_.disabled_index >= static_cast<int>(disabled_alarm_entries_.size()))) {
            ESP_LOGW(TAG, "ApplySelectedAlarmAction(save): invalid disabled_index=%d",
                     alarm_action_entry_.disabled_index);
            return false;
        }
        struct tm base_tm{};
        const time_t base_time = alarm_action_entry_.active
                                     ? alarm_action_entry_.trigger_time
                                     : disabled_alarm_entries_[alarm_action_entry_.disabled_index].trigger_time;
        localtime_r(&base_time, &base_tm);
        int h = alarm_edit_hour_;
        int m = alarm_edit_minute_;
        if (m >= 60) {
            m = 0;
            h += 1;
        }
        if (h >= 24) {
            h = 0;
            base_tm.tm_mday += 1;
        }
        base_tm.tm_hour = h;
        base_tm.tm_min = m;
        base_tm.tm_sec = 0;
        time_t new_time = mktime(&base_tm);
        const time_t now = time(nullptr);
        if (new_time <= now) {
            if (alarm_action_entry_.repeat_seconds > 0) {
                while (new_time <= now) {
                    new_time += alarm_action_entry_.repeat_seconds;
                }
            } else {
                new_time = now + 60;
            }
        }
        if (alarm_action_entry_.active) {
            return general_timer_->UpdateAlarmByIndex(
                alarm_action_entry_.timer_index,
                new_time,
                alarm_action_entry_.message);
        }
        if (alarm_action_entry_.disabled_index < 0 ||
            alarm_action_entry_.disabled_index >= static_cast<int>(disabled_alarm_entries_.size())) {
            return false;
        }
        auto& entry = disabled_alarm_entries_[alarm_action_entry_.disabled_index];
        entry.trigger_time = new_time;
        return true;
    }

    if (is_edit_time) {
        std::vector<std::string> items;
        char hbuf[24];
        char mbuf[24];
        snprintf(hbuf, sizeof(hbuf), "Hour: %02d", alarm_edit_hour_);
        snprintf(mbuf, sizeof(mbuf), "Minute: %02d", alarm_edit_minute_);
        items = {
            std::string(hbuf) + " (+1)",
            std::string(hbuf) + " (-1)",
            std::string(mbuf) + " (+1)",
            std::string(mbuf) + " (-1)",
            "Save",
            "Back"
        };
        return OpenMenuPicker(MenuPickerContext::AlarmEditTime, "Set Time", items, 0);
    }
    return true;
#else
    return false;
#endif
}

bool Application::OpenSettingPicker() {
    std::vector<std::string> items;
    items.reserve(5);
    items.push_back("AI Agent");
    items.push_back("Wi-Fi");
    items.push_back("Sound & Screen");
    items.push_back("System");
    items.push_back("Exit");
    return OpenMenuPicker(MenuPickerContext::SettingAction, "Setting", items, 0);
}

std::string Application::GetSdCardStorageString() {
    auto sd_card = Board::GetInstance().GetSdCard();
    if (sd_card && sd_card->IsMounted()) {
        const char* mount_point = sd_card->GetMountPoint();
        uint64_t total_bytes = 0;
        uint64_t free_bytes = 0;
        esp_err_t err = esp_vfs_fat_info(mount_point, &total_bytes, &free_bytes);
        if (err == ESP_OK) {
            double total_gb = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);
            double free_gb = (double)free_bytes / (1024.0 * 1024.0 * 1024.0);
            
            char buf[64];
            if (total_gb >= 1.0) {
                snprintf(buf, sizeof(buf), "SD: Free %.1fG/%.1fG", free_gb, total_gb);
            } else {
                double total_mb = (double)total_bytes / (1024.0 * 1024.0);
                double free_mb = (double)free_bytes / (1024.0 * 1024.0);
                snprintf(buf, sizeof(buf), "SD: Free %.0fM/%.0fM", free_mb, total_mb);
            }
            return buf;
        }
        return "SD: Read Err";
    }
    return "No SDCard";
}

bool Application::OpenSoundScreenPicker() {
    std::vector<std::string> items;
    items.reserve(5);
    
    auto codec = Board::GetInstance().GetAudioCodec();
    int vol_pct = (codec->output_volume() * 100) / 80;
    
    auto backlight = Board::GetInstance().GetBacklight();
    int bri_pct = backlight->brightness();
    
    items.push_back("Volume: " + std::to_string(vol_pct) + "%");
    items.push_back("Brightness: " + std::to_string(bri_pct) + "%");
    items.push_back("Theme");
    if (SupportsJiuchuanLcdDriverSetting()) {
        items.push_back("LCD Setting");
    }
    items.push_back("Back");
    return OpenMenuPicker(MenuPickerContext::SoundScreenAction, "Sound & Screen", items, 0);
}

bool Application::OpenSystemPicker() {
    std::vector<std::string> items;
    items.reserve(5);
    items.push_back("Auto update");
    items.push_back("Power");
    items.push_back("About Speaker");
    items.push_back(GetSdCardStorageString());
    items.push_back("Back");
    return OpenMenuPicker(MenuPickerContext::SystemAction, "System", items, 0);
}

bool Application::OpenVolumeAdjustPicker() {
    std::vector<std::string> items = {"Volume"};
    return OpenMenuPicker(MenuPickerContext::VolumeAdjust, "Volume", items, 0);
}

bool Application::OpenBrightnessAdjustPicker() {
    std::vector<std::string> items = {"Brightness"};
    return OpenMenuPicker(MenuPickerContext::BrightnessAdjust, "Brightness", items, 0);
}

bool Application::OpenLcdSettingPicker() {
    if (!SupportsJiuchuanLcdDriverSetting()) {
        return false;
    }

    const int current_driver = GetJiuchuanLcdDriverSetting();
    std::vector<std::string> items;
    items.reserve(3);
    items.push_back((current_driver == 1 ? "* " : "  ") + std::string("Old Screen (GC9301)"));
    items.push_back((current_driver == 2 ? "* " : "  ") + std::string("New Screen (JD9853)"));
    items.push_back("Back");
    return OpenMenuPicker(MenuPickerContext::LcdSettingAction, "LCD Setting", items, 0);
}

bool Application::OpenThemeSettingPicker() {
#if CONFIG_ENABLE_IDLE_SCREEN
    if (!idle_screen_) {
        return false;
    }
    const auto current_theme = idle_screen_->GetTheme();
    original_theme_ = static_cast<int>(current_theme); // Cache original theme
    std::vector<std::string> items;
    items.reserve(5);
    
    items.push_back((current_theme == IdleScreen::Theme::Orange ? "* " : "  ") + std::string("Tiger Cub"));
    items.push_back((current_theme == IdleScreen::Theme::Pink ? "* " : "  ") + std::string("Bunny Nose"));
    items.push_back((current_theme == IdleScreen::Theme::BluePastel ? "* " : "  ") + std::string("Dino Blue"));
    items.push_back((current_theme == IdleScreen::Theme::Yellow ? "* " : "  ") + std::string("Honey Bee"));
    items.push_back("Back");

    int start_index = 0;
    if (current_theme == IdleScreen::Theme::Orange) start_index = 0;
    else if (current_theme == IdleScreen::Theme::Pink) start_index = 1;
    else if (current_theme == IdleScreen::Theme::BluePastel) start_index = 2;
    else if (current_theme == IdleScreen::Theme::Yellow) start_index = 3;

    return OpenMenuPicker(MenuPickerContext::ThemeSettingAction, "Theme", items, start_index);
#else
    return false;
#endif
}

void Application::ApplyLiveThemeSelection() {
#if CONFIG_ENABLE_IDLE_SCREEN
    if (idle_screen_) {
        if (menu_picker_index_ == 0) {
            idle_screen_->PreviewTheme(IdleScreen::Theme::Orange);
        } else if (menu_picker_index_ == 1) {
            idle_screen_->PreviewTheme(IdleScreen::Theme::Pink);
        } else if (menu_picker_index_ == 2) {
            idle_screen_->PreviewTheme(IdleScreen::Theme::BluePastel);
        } else if (menu_picker_index_ == 3) {
            idle_screen_->PreviewTheme(IdleScreen::Theme::Yellow);
        } else if (menu_picker_index_ == 4) {
            if (original_theme_ != -1) {
                idle_screen_->PreviewTheme(static_cast<IdleScreen::Theme>(original_theme_));
            }
        }
}
#endif
}

bool Application::OpenOtaSettingPicker() {
    Settings settings("wifi", false);
    std::string current_url = settings.GetString("ota_url_custom");
    if (current_url.empty()) {
        current_url = settings.GetString("ota_url");
    }
    bool is_yes = (current_url == "https://ota.globy.tech/");

    std::vector<std::string> items;
    items.reserve(3);
    items.push_back((is_yes ? "* " : "  ") + std::string("Yes"));
    items.push_back((!is_yes ? "* " : "  ") + std::string("No"));
    items.push_back("Back");
    return OpenMenuPicker(MenuPickerContext::OtaSettingAction, "Custom OTA", items, is_yes ? 0 : 1);
}

bool Application::OpenWifiResetConfirmPicker() {
    std::vector<std::string> items = {"Yes", "No"};
    return OpenMenuPicker(MenuPickerContext::WifiResetConfirm, "Reset WiFi?", items, 1);
}

bool Application::OpenLcdSwitchConfirmPicker() {
    std::vector<std::string> items = {"Yes", "No"};
    return OpenMenuPicker(MenuPickerContext::LcdSwitchConfirm, "Apply LCD?", items, 1);
}

bool Application::OpenSdReloadConfirmPicker() {
    std::vector<std::string> items = {"Yes", "No"};
    return OpenMenuPicker(MenuPickerContext::SdReloadConfirm, "Reload Music?", items, 1);
}

bool Application::StartSdLibraryReloadTask() {
#if CONFIG_SD_CARD_ENABLE
    auto* display = Board::GetInstance().GetDisplay();
    if (sd_music_ == nullptr) {
        display->ShowNotification("SD music unavailable", 2000);
        return false;
    }

    if (sd_reload_in_progress_) {
        display->ShowNotification("Reload in progress...", 1500);
        return true;
    }

    sd_reload_in_progress_ = true;
    display->ShowNotification("Reloading music library...", 2000);

    BaseType_t created = xTaskCreate(
        [](void* arg) {
            auto* app = static_cast<Application*>(arg);
            bool rebuilt = false;
            size_t old_tracks = 0;
            size_t total_tracks = 0;

            if (app->sd_music_ != nullptr) {
                old_tracks = app->sd_music_->GetTotalTracks();
                rebuilt = app->sd_music_->RebuildPlaylist();
                total_tracks = app->sd_music_->GetTotalTracks();
            }

            auto* disp = Board::GetInstance().GetDisplay();
            if (rebuilt) {
                std::string note = "Reload OK: " + std::to_string(total_tracks) +
                    " tracks (was " + std::to_string(old_tracks) + ")";
                disp->ShowNotification(note.c_str(), 4500);
            } else {
                std::string note = "Reload failed (tracks: " + std::to_string(total_tracks) + ")";
                disp->ShowNotification(note.c_str(), 4500);
            }

            app->sd_reload_in_progress_ = false;
            app->sd_reload_task_handle_ = nullptr;
            vTaskDelete(nullptr);
        },
        "sd_reload_task",
        1024 * 12,
        this,
        2,
        &sd_reload_task_handle_);

    if (created != pdPASS) {
        sd_reload_in_progress_ = false;
        sd_reload_task_handle_ = nullptr;
        display->ShowNotification("Reload start failed", 2000);
        return false;
    }

    return true;
#else
    return false;
#endif
}

bool Application::OpenRestartConfirmPicker() {
    std::vector<std::string> items = {"Yes", "No"};
    return OpenMenuPicker(MenuPickerContext::RestartConfirm, "Restart?", items, 1);
}

bool Application::OpenShutdownConfirmPicker() {
    std::vector<std::string> items = {"Yes", "No"};
    return OpenMenuPicker(MenuPickerContext::ShutdownConfirm, "Shut Down?", items, 1);
}

bool Application::OpenPowerPicker() {
    const bool save_power_enabled = GetPowerSaveEnabledSetting();
    const int timeout_seconds = GetPowerOffTimeoutSecondsSetting();

    std::vector<std::string> items;
    items.reserve(5);
    items.push_back("Restart");
    items.push_back("Shut Down");
    items.push_back(std::string("Save Power: ") + (save_power_enabled ? "Yes" : "No"));
    items.push_back(std::string("Power Off: ") + (save_power_enabled ? FormatPowerOffTimeoutLabel(timeout_seconds) : "Off"));
    items.push_back("Exit");
    return OpenMenuPicker(MenuPickerContext::PowerAction, "Power", items, 0);
}

bool Application::OpenPowerTimeoutPicker() {
    const int current_timeout = GetPowerOffTimeoutSecondsSetting();
    const auto options = GetPowerTimeoutOptions();

    std::vector<std::string> items;
    items.reserve(options.size() + 1);
    int initial_index = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        const bool selected = (options[i] == current_timeout);
        if (selected) {
            initial_index = static_cast<int>(i);
        }
        items.push_back((selected ? "* " : "  ") + FormatPowerOffTimeoutLabel(options[i]));
    }
    items.push_back("Back");
    return OpenMenuPicker(MenuPickerContext::PowerTimeoutAction, "Power Timer", items, initial_index);
}

bool Application::OpenWifiPickerFromSavedList() {
    SsidManager& ssid_mgr = SsidManager::GetInstance();
    const auto& saved = ssid_mgr.GetSsidList();
    if (saved.empty()) {
        std::vector<std::string> items = {"+ Add New WiFi", "Reset WiFi", "Exit"};
        std::vector<int> map = {-1, -3, -2};
        return OpenMenuPicker(MenuPickerContext::WifiAccessPoint, "Wi-Fi", items, 0, map);
    }
    RefreshSavedWifiRssiCache();

    auto& ws = WifiStation::GetInstance();
    const bool wifi_connected = ws.IsConnected();
    std::string current_ssid = wifi_connected ? ws.GetSsid() : "";
    int current_rssi = wifi_connected ? static_cast<int>(ws.GetRssi()) : -127;

    struct WifiSavedRow {
        std::string text;
        int saved_index = -1;
        int rssi = -127;
        bool found = false;
        bool is_current = false;
    };
    std::vector<WifiSavedRow> rows;
    rows.reserve(saved.size());
    if (wifi_saved_rssi_cache_.size() != saved.size()) {
        wifi_saved_rssi_cache_.assign(saved.size(), -127);
        wifi_saved_rssi_valid_ = false;
    }

    int initial_index = 0;
    for (size_t i = 0; i < saved.size(); ++i) {
        int rssi = wifi_saved_rssi_valid_ ? wifi_saved_rssi_cache_[i] : -127;
        bool found = (rssi > -120);
        if (!found && wifi_connected && NormalizeSsidForCompare(saved[i].ssid) == NormalizeSsidForCompare(current_ssid)) {
            rssi = current_rssi;
            found = true;
        }

        std::string row;
        row.reserve(saved[i].ssid.size() + 20);
        row += (saved[i].ssid == current_ssid) ? "*" : " ";
        row += saved[i].ssid;
        row += " (";
        if (found) {
            row += std::to_string(rssi);
            row += "dBm)";
        } else {
            row += "N/A)";
        }

        WifiSavedRow saved_row;
        saved_row.text = std::move(row);
        saved_row.saved_index = static_cast<int>(i);
        saved_row.rssi = rssi;
        saved_row.found = found;
        saved_row.is_current = (saved[i].ssid == current_ssid);
        rows.push_back(std::move(saved_row));
    }

    std::sort(rows.begin(), rows.end(), [](const WifiSavedRow& a, const WifiSavedRow& b) {
        if (a.found != b.found) return a.found > b.found;
        if (a.rssi != b.rssi) return a.rssi > b.rssi;
        return a.saved_index < b.saved_index;
    });

    std::vector<std::string> items;
    std::vector<int> map;
    items.reserve(rows.size() + 2);
    map.reserve(rows.size() + 2);
    for (size_t i = 0; i < rows.size(); ++i) {
        items.push_back(rows[i].text);
        map.push_back(rows[i].saved_index);
        if (rows[i].is_current) {
            initial_index = static_cast<int>(i);
        }
    }

    items.push_back("+ Add New WiFi");
    map.push_back(-1);
    items.push_back("Reset WiFi");
    map.push_back(-3);
    items.push_back("Exit");
    map.push_back(-2);

    if (items.empty()) {
        items = {"+ Add New WiFi", "Reset WiFi", "Exit"};
        map = {-1, -3, -2};
        initial_index = 0;
    }
    return OpenMenuPicker(MenuPickerContext::WifiAccessPoint, "Wi-Fi", items, initial_index, map);
}

void Application::RefreshSavedWifiRssiCache() {
    SsidManager& ssid_mgr = SsidManager::GetInstance();
    const auto& saved = ssid_mgr.GetSsidList();
    if (saved.empty()) {
        wifi_saved_rssi_cache_.clear();
        wifi_saved_rssi_valid_ = false;
        return;
    }

    wifi_saved_rssi_cache_.assign(saved.size(), -127);
    wifi_saved_rssi_valid_ = false;

    auto& ws = WifiStation::GetInstance();
    wifi_scan_config_t scan_config = {};
    if (!ws.Scan(&scan_config, true, false)) {
        return;
    }

    std::vector<wifi_ap_record_t> results;
    if (!ws.GetScanResults(results)) {
        return;
    }

    for (size_t i = 0; i < saved.size(); ++i) {
        int best_rssi = -127;
        const std::string saved_norm = NormalizeSsidForCompare(saved[i].ssid);
        for (const auto& ap : results) {
            const std::string ap_ssid(reinterpret_cast<const char*>(ap.ssid));
            if (NormalizeSsidForCompare(ap_ssid) == saved_norm) {
                best_rssi = std::max(best_rssi, static_cast<int>(ap.rssi));
            }
        }
        wifi_saved_rssi_cache_[i] = best_rssi;
    }
    wifi_saved_rssi_valid_ = true;
}

bool Application::OpenWifiAddConfirmPicker() {
    std::vector<std::string> items = {"Yes", "No"};
    return OpenMenuPicker(MenuPickerContext::WifiAddConfirm, "Add WiFi?", items, 1);
}

void Application::StartWifiScanRefreshTask() {
    // Disabled intentionally: active scan here conflicts with WifiStation's
    // internal reconnect/scan flow and can cause RSSI jitter or STA reset.
    wifi_scan_refresh_task_handle_ = nullptr;
}

bool Application::RestartWifiAndReconnectAsync() {
    auto display = Board::GetInstance().GetDisplay();
    xTaskCreate([](void* arg) {
        auto* display = static_cast<Display*>(arg);
        std::string note = "Switching Wi-Fi...";
        display->ShowNotification(note.c_str());
        auto& ws = WifiStation::GetInstance();
        ws.Stop();
        vTaskDelay(pdMS_TO_TICKS(300));
        ws.Start();
        bool ok = ws.WaitForConnected(25000);
        std::string result = ok ? ("Connected: " + ws.GetSsid()) : "Wi-Fi switch failed";
        display->ShowNotification(result.c_str(), 5000);
        vTaskDelete(nullptr);
    }, "wifi_switch", 4096, display, 2, nullptr);
    return true;
}

bool Application::ApplyWifiPickerSelection() {
    if (menu_picker_index_ < 0 || menu_picker_index_ >= static_cast<int>(menu_picker_map_.size())) {
        return false;
    }
    int saved_index = menu_picker_map_[menu_picker_index_];
    if (saved_index < 0) {
        if (saved_index == -2) {
            return OpenSettingPicker();
        }
        if (saved_index == -3) {
            return OpenWifiResetConfirmPicker();
        }
        return OpenWifiAddConfirmPicker();
    }
    SsidManager::GetInstance().SetDefaultSsid(saved_index);
    return RestartWifiAndReconnectAsync();
}

bool Application::TriggerWifiReconfigureMode() {
    Settings settings("wifi", true);
    settings.SetInt("force_ap", 1);
    auto display = Board::GetInstance().GetDisplay();
    display->ShowNotification("Rebooting to Wi-Fi setup...", 2500);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true;
}

bool Application::TriggerDeviceRestart() {
    auto display = Board::GetInstance().GetDisplay();
    display->ShowNotification("Restarting...", 2000);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true;
}

bool Application::TriggerDeviceShutdown() {
    auto& board = Board::GetInstance();
    auto* display = board.GetDisplay();
    display->ShowNotification("Shutting down...", 2000);
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!board.RequestShutdown()) {
        display->ShowNotification("Shutdown unavailable", 2200);
        return false;
    }
    return true;
}

bool Application::ActivateMenuPicker() {
    if (menu_picker_ctx_ == MenuPickerContext::None || menu_picker_items_.empty()) {
        return false;
    }
    bool ok = false;
    bool keep_picker_open = false;
    switch (menu_picker_ctx_) {
        case MenuPickerContext::SdDirectory: {
#ifdef CONFIG_SD_CARD_ENABLE
            if (sd_music_) {
                const int item = (menu_picker_index_ >= 0 &&
                                  menu_picker_index_ < static_cast<int>(menu_picker_map_.size()))
                                     ? menu_picker_map_[menu_picker_index_]
                                     : menu_picker_index_;
                if (item == -1) {
                    keep_main_menu_open_after_picker_ = true;
                    ok = true;
                } else if (item == -3) {
                    ok = OpenSdReloadConfirmPicker();
                    keep_picker_open = ok;
                } else if (menu_picker_index_ >= 0 &&
                           menu_picker_index_ < static_cast<int>(menu_picker_items_.size())) {
                    const std::string selected_dir = menu_picker_items_[menu_picker_index_];
                    ESP_LOGI(TAG, "Open SD folder '%s' (map=%d, idx=%d)",
                             selected_dir.c_str(), item, menu_picker_index_);
                    ok = OpenSdTrackPicker(selected_dir);
                    keep_picker_open = ok;
                }
            }
#endif
            break;
        }
        case MenuPickerContext::SdTrack: {
#ifdef CONFIG_SD_CARD_ENABLE
            if (sd_music_) {
                int track_index = (menu_picker_index_ >= 0 &&
                                   menu_picker_index_ < static_cast<int>(menu_picker_map_.size()))
                                      ? menu_picker_map_[menu_picker_index_]
                                      : menu_picker_index_;
                if (track_index < 0) {
                    keep_main_menu_open_after_picker_ = true;
                    ok = true;
                } else {
                    StopOtherMedia(MediaComponent::kSdMusic);
                    EnsureIdleForMedia();
                    sd_music_->SetShuffleMode(false);
                    sd_music_->SetRepeatMode(Esp32SdMusic::RepeatMode::RepeatAll);
                    ok = sd_music_->SetTrack(track_index);
                    if (ok) {
                        SetSdMusicPlaybackMode(true);
                    }
                }
            }
#endif
            break;
        }
        case MenuPickerContext::RadioStation:
            SetSdMusicPlaybackMode(false);
            if (menu_picker_index_ >= 0 &&
                menu_picker_index_ < static_cast<int>(menu_picker_map_.size())) {
                int station_index = menu_picker_map_[menu_picker_index_];
                if (station_index < 0) {
                    keep_main_menu_open_after_picker_ = true;
                    ok = true;
                } else {
                    auto stations = radio_ ? radio_->GetStationList() : std::vector<std::string>{};
                    if (station_index >= 0 && station_index < static_cast<int>(stations.size())) {
                        ok = PlayRadio(stations[station_index]);
                    }
                }
            }
            break;
        case MenuPickerContext::AlarmList:
#if CONFIG_USE_ALARM
            SetSdMusicPlaybackMode(false);
            if (menu_picker_index_ >= 0 &&
                menu_picker_index_ < static_cast<int>(menu_picker_items_.size())) {
                int alarm_index = menu_picker_index_;
                if (menu_picker_index_ < static_cast<int>(menu_picker_map_.size())) {
                    alarm_index = menu_picker_map_[menu_picker_index_];
                }
                if (alarm_index == -2) {
                    keep_main_menu_open_after_picker_ = true;
                    ok = true;
                } else if (alarm_index == -1) {
                    ok = StartAlarmPromptFromMenu();
                } else if (alarm_index >= 0 &&
                           alarm_index < static_cast<int>(alarm_menu_entries_.size())) {
                    alarm_action_entry_ = alarm_menu_entries_[alarm_index];
                    alarm_action_entry_valid_ = true;
                    ok = OpenAlarmActionPicker();
                    keep_picker_open = ok;
                }
            } else if (menu_picker_index_ >= 0 &&
                       menu_picker_index_ < static_cast<int>(alarm_menu_entries_.size())) {
                alarm_action_entry_ = alarm_menu_entries_[menu_picker_index_];
                alarm_action_entry_valid_ = true;
                ok = OpenAlarmActionPicker();
                keep_picker_open = ok;
            } else {
                ok = StartAlarmPromptFromMenu();
            }
#else
            ok = StartAlarmPromptFromMenu();
#endif
            break;
        case MenuPickerContext::AlarmAction:
            if (menu_picker_index_ == 3) {
                ok = OpenAlarmDeleteConfirmPicker();
                keep_picker_open = ok;
                break;
            }
            if (menu_picker_index_ == 1) {
                ok = ApplySelectedAlarmAction();
                keep_picker_open = ok;
                break;
            }
            ok = ApplySelectedAlarmAction();
            if (ok) {
                ok = OpenAlarmListPicker();
                keep_picker_open = ok;
            }
            break;
        case MenuPickerContext::AlarmEditTime:
            ok = ApplySelectedAlarmAction();
            keep_picker_open = ok;
            break;
        case MenuPickerContext::AlarmDeleteConfirm:
            if (menu_picker_index_ == 0) {
                if (alarm_action_entry_.active) {
                    if (alarm_action_entry_.timer_index < 0) {
                        ESP_LOGW(TAG, "AlarmDeleteConfirm: invalid timer_index=%d",
                                 alarm_action_entry_.timer_index);
                        ok = false;
                    } else {
                        ok = general_timer_ && general_timer_->DeleteAlarmByIndex(alarm_action_entry_.timer_index);
                    }
                } else {
                    if (alarm_action_entry_.disabled_index < 0 ||
                        alarm_action_entry_.disabled_index >= static_cast<int>(disabled_alarm_entries_.size())) {
                        ESP_LOGW(TAG, "AlarmDeleteConfirm: invalid disabled_index=%d",
                                 alarm_action_entry_.disabled_index);
                        ok = false;
                    } else {
                        disabled_alarm_entries_.erase(disabled_alarm_entries_.begin() + alarm_action_entry_.disabled_index);
                        ok = true;
                    }
                }
                if (ok) {
                    ok = OpenAlarmListPicker();
                    keep_picker_open = ok;
                }
            } else {
                ok = OpenAlarmActionPicker();
                keep_picker_open = ok;
            }
            break;
        case MenuPickerContext::SettingAction: {
            SetSdMusicPlaybackMode(false);
            if (menu_picker_index_ < 0 ||
                menu_picker_index_ >= static_cast<int>(menu_picker_items_.size())) {
                break;
            }
            const std::string selected_setting = menu_picker_items_[menu_picker_index_];
            if (selected_setting == "AI Agent") {
                auto& qr = qrcode::QRCodeDisplay::GetInstance();
                if (qr.IsDisplayed()) {
                    ESP_LOGI(TAG, "SettingAction: hide role QR");
                    qr.Clear();
                    Board::GetInstance().GetDisplay()->ShowNotification("QR Hidden");
                    ok = true;
                } else {
                    std::string portal_url = BuildRolePortalUrlFromMac();
                    ESP_LOGI(TAG, "SettingAction: show role QR, url='%s'", portal_url.c_str());
                    if (!portal_url.empty()) {
                        ok = qr.Show(portal_url, "Cấu hình Trợ lý\nKhởi động lại Loa để áp dụng Cấu hình mới.");
                    }
                    if (ok) {
                        Board::GetInstance().GetDisplay()->ShowNotification("QR Shown");
                    } else {
                        Board::GetInstance().GetDisplay()->ShowNotification("QR Unavailable");
                    }
                }
            } else if (selected_setting == "Wi-Fi") {
                ok = OpenWifiPickerFromSavedList();
                keep_picker_open = ok;
            } else if (selected_setting == "Sound & Screen") {
                ok = OpenSoundScreenPicker();
                keep_picker_open = ok;
            } else if (selected_setting == "System") {
                ok = OpenSystemPicker();
                keep_picker_open = ok;
            } else if (selected_setting == "Exit") {
                keep_main_menu_open_after_picker_ = true;
                ok = true;
            }
            break;
        }
        case MenuPickerContext::SoundScreenAction: {
            if (menu_picker_index_ < 0 ||
                menu_picker_index_ >= static_cast<int>(menu_picker_items_.size())) {
                break;
            }
            const std::string selected = menu_picker_items_[menu_picker_index_];
            if (selected.rfind("Volume", 0) == 0) {
                ok = OpenVolumeAdjustPicker();
                keep_picker_open = ok;
            } else if (selected.rfind("Brightness", 0) == 0) {
                ok = OpenBrightnessAdjustPicker();
                keep_picker_open = ok;
            } else if (selected == "Theme") {
                ok = OpenThemeSettingPicker();
                keep_picker_open = ok;
            } else if (selected == "LCD Setting") {
                ok = OpenLcdSettingPicker();
                keep_picker_open = ok;
            } else if (selected == "Back") {
                ok = OpenSettingPicker();
                keep_picker_open = ok;
            }
            break;
        }
        case MenuPickerContext::SystemAction: {
            if (menu_picker_index_ < 0 ||
                menu_picker_index_ >= static_cast<int>(menu_picker_items_.size())) {
                break;
            }
            const std::string selected = menu_picker_items_[menu_picker_index_];
            if (selected == "Auto update") {
                ok = OpenOtaSettingPicker();
                keep_picker_open = ok;
            } else if (selected == "Power") {
                ok = OpenPowerPicker();
                keep_picker_open = ok;
            } else if (selected == "About Speaker") {
                ok = OpenDeviceInfoPicker();
                keep_picker_open = ok;
                if (ResolvePairDeviceId().empty() && !waiting_device_id_from_ai_) {
                    StartDeviceInfoPromptFromMenu();
                }
            } else if (selected == "Back") {
                ok = OpenSettingPicker();
                keep_picker_open = ok;
            } else {
                Board::GetInstance().GetDisplay()->ShowNotification("Checking SD...", 1200);
                ok = OpenSystemPicker();
                keep_picker_open = ok;
            }
            break;
        }
        case MenuPickerContext::VolumeAdjust:
        case MenuPickerContext::BrightnessAdjust: {
            ok = OpenSoundScreenPicker();
            keep_picker_open = ok;
            break;
        }
        case MenuPickerContext::LcdSettingAction: {
            if (menu_picker_index_ == 0 || menu_picker_index_ == 1) {
                const int selected_driver = (menu_picker_index_ == 0) ? 1 : 2;
                auto* display = Board::GetInstance().GetDisplay();
                const int current_driver = GetJiuchuanLcdDriverSetting();
                if (selected_driver == current_driver) {
                    display->ShowNotification("Already active", 1200);
                    ok = OpenLcdSettingPicker();
                    keep_picker_open = ok;
                } else {
                    pending_lcd_driver_ = selected_driver;
                    ok = OpenLcdSwitchConfirmPicker();
                    keep_picker_open = ok;
                }
            } else {
                ok = OpenSoundScreenPicker();
                keep_picker_open = ok;
            }
            break;
        }
        case MenuPickerContext::ThemeSettingAction: {
            if (menu_picker_index_ >= 0 && menu_picker_index_ <= 3) {
#if CONFIG_ENABLE_IDLE_SCREEN
                if (idle_screen_) {
                    if (menu_picker_index_ == 0) {
                        idle_screen_->SetTheme(IdleScreen::Theme::Orange);
                    } else if (menu_picker_index_ == 1) {
                        idle_screen_->SetTheme(IdleScreen::Theme::Pink);
                    } else if (menu_picker_index_ == 2) {
                        idle_screen_->SetTheme(IdleScreen::Theme::BluePastel);
                    } else if (menu_picker_index_ == 3) {
                        idle_screen_->SetTheme(IdleScreen::Theme::Yellow);
                    }
                }
#endif
                Board::GetInstance().GetDisplay()->ShowNotification("Theme applied", 1200);
            } else {
#if CONFIG_ENABLE_IDLE_SCREEN
                if (idle_screen_ && original_theme_ != -1) {
                    idle_screen_->PreviewTheme(static_cast<IdleScreen::Theme>(original_theme_));
                }
#endif
            }
            ok = OpenSoundScreenPicker();
            keep_picker_open = ok;
            break;
        }
        case MenuPickerContext::OtaSettingAction: {
            if (menu_picker_index_ == 0) {
                Settings settings("wifi", true);
                settings.SetString("ota_url_custom", "https://ota.globy.tech/");
                settings.EraseKey("ota_url");
                Board::GetInstance().GetDisplay()->ShowNotification("OTA: Globy Tech", 1500);
            } else if (menu_picker_index_ == 1) {
                Settings settings("wifi", true);
                settings.EraseKey("ota_url_custom");
                settings.EraseKey("ota_url");
                Board::GetInstance().GetDisplay()->ShowNotification("OTA: Default", 1500);
            }
            ok = OpenSystemPicker();
            keep_picker_open = ok;
            break;
        }
        case MenuPickerContext::LcdSwitchConfirm:
            if (menu_picker_index_ == 0) {
                const int selected_driver =
                    (pending_lcd_driver_ == 1 || pending_lcd_driver_ == 2)
                        ? pending_lcd_driver_
                        : GetJiuchuanLcdDriverSetting();
                Settings display_settings("display", true);
                display_settings.SetInt("lcd_driver", selected_driver);
                auto* display = Board::GetInstance().GetDisplay();
                display->ShowNotification(
                    selected_driver == 1 ? "Apply old screen..." : "Apply new screen...",
                    1500);
                vTaskDelay(pdMS_TO_TICKS(300));
                ok = TriggerDeviceRestart();
            } else {
                ok = OpenLcdSettingPicker();
                keep_picker_open = ok;
            }
            pending_lcd_driver_ = 0;
            break;
        case MenuPickerContext::SdReloadConfirm:
            if (menu_picker_index_ == 0) {
                ok = StartSdLibraryReloadTask();
            } else {
                ok = true;
            }
            keep_picker_open = OpenSdDirectoryPicker();
            ok = keep_picker_open;
            break;
        case MenuPickerContext::PowerAction:
            if (menu_picker_index_ == 0) {
                ok = OpenRestartConfirmPicker();
                keep_picker_open = ok;
            } else if (menu_picker_index_ == 1) {
                ok = OpenShutdownConfirmPicker();
                keep_picker_open = ok;
            } else if (menu_picker_index_ == 2) {
                const bool enabled = GetPowerSaveEnabledSetting();
                SetPowerSaveEnabledSetting(!enabled);
                Board::GetInstance().SetPowerSaveMode(false);
                auto* display = Board::GetInstance().GetDisplay();
                display->ShowNotification(!enabled ? "Save Power: Yes" : "Save Power: No", 1500);
                ok = OpenPowerPicker();
                keep_picker_open = ok;
            } else if (menu_picker_index_ == 3) {
                if (!GetPowerSaveEnabledSetting()) {
                    Board::GetInstance().GetDisplay()->ShowNotification("Save Power is OFF", 1500);
                    ok = OpenPowerPicker();
                } else {
                    ok = OpenPowerTimeoutPicker();
                }
                keep_picker_open = ok;
            } else {
                ok = OpenSystemPicker();
                keep_picker_open = ok;
            }
            break;
        case MenuPickerContext::PowerTimeoutAction: {
            const auto options = GetPowerTimeoutOptions();
            if (menu_picker_index_ >= 0 && menu_picker_index_ < static_cast<int>(options.size())) {
                SetPowerOffTimeoutSecondsSetting(options[menu_picker_index_]);
                Board::GetInstance().SetPowerSaveMode(false);
                auto* display = Board::GetInstance().GetDisplay();
                std::string note = "Power Off: " + FormatPowerOffTimeoutLabel(options[menu_picker_index_]);
                display->ShowNotification(note.c_str(), 1500);
                ok = OpenPowerPicker();
            } else {
                ok = OpenPowerPicker();
            }
            keep_picker_open = ok;
            break;
        }
        case MenuPickerContext::DeviceInfo:
            if (menu_picker_index_ >= 0 &&
                menu_picker_index_ < static_cast<int>(menu_picker_map_.size()) &&
                menu_picker_map_[menu_picker_index_] == -1) {
                ok = OpenSystemPicker();
            } else {
                ok = true;
            }
            keep_picker_open = ok;
            break;
        case MenuPickerContext::RestartConfirm:
            if (menu_picker_index_ == 0) {
                ok = TriggerDeviceRestart();
            } else {
                ok = OpenPowerPicker();
                keep_picker_open = ok;
            }
            break;
        case MenuPickerContext::ShutdownConfirm:
            if (menu_picker_index_ == 0) {
                ok = TriggerDeviceShutdown();
            } else {
                ok = OpenPowerPicker();
                keep_picker_open = ok;
            }
            break;
        case MenuPickerContext::WifiAccessPoint:
            SetSdMusicPlaybackMode(false);
            ok = ApplyWifiPickerSelection();
            if (ok && (menu_picker_ctx_ == MenuPickerContext::WifiAddConfirm ||
                       menu_picker_ctx_ == MenuPickerContext::WifiResetConfirm ||
                       menu_picker_ctx_ == MenuPickerContext::SettingAction)) {
                keep_picker_open = true;
            }
            break;
        case MenuPickerContext::WifiAddConfirm:
            if (menu_picker_index_ == 0) {
                ok = TriggerWifiReconfigureMode();
            } else {
                ok = OpenWifiPickerFromSavedList();
                keep_picker_open = ok;
            }
            break;
        case MenuPickerContext::WifiResetConfirm:
            if (menu_picker_index_ == 0) {
                SsidManager::GetInstance().Clear();
                Board::GetInstance().GetDisplay()->ShowNotification("SSIDs Cleared", 2000);
            }
            ok = OpenWifiPickerFromSavedList();
            keep_picker_open = ok;
            break;
        default:
            break;
    }
    if (!keep_picker_open) {
        CloseMenuPicker();
    }
    return ok;
}

bool Application::ActivateMainMenuSelection() {
#if CONFIG_ENABLE_IDLE_SCREEN
    if (!idle_screen_ || !idle_screen_->IsMainMenuVisible()) {
        return false;
    }
    if (menu_picker_ctx_ != MenuPickerContext::None) {
        bool picker_ok = ActivateMenuPicker();
        if (menu_picker_ctx_ == MenuPickerContext::None) {
            if (keep_main_menu_open_after_picker_) {
                keep_main_menu_open_after_picker_ = false;
            } else {
                idle_screen_->SelectMainMenuItem();
            }
        }
        return picker_ok;
    }

    if (offline_mode_) {
        if (idle_screen_->GetMainMenuItem() == IdleScreen::MainMenuItem::AiTalk ||
            idle_screen_->GetMainMenuItem() == IdleScreen::MainMenuItem::OnlineMusic ||
            idle_screen_->GetMainMenuItem() == IdleScreen::MainMenuItem::Radio ||
            idle_screen_->GetMainMenuItem() == IdleScreen::MainMenuItem::AlarmClock) {
            Board::GetInstance().GetDisplay()->ShowNotification("Offline mode: unavailable");
            return false;
        }
    }

    bool ok = false;
    switch (idle_screen_->GetMainMenuItem()) {
        case IdleScreen::MainMenuItem::AiTalk:
            SetSdMusicPlaybackMode(false);
            ok = StartAiTalkFromMenu();
            break;
        case IdleScreen::MainMenuItem::SdMusic:
            SetSdMusicPlaybackMode(false);
            ok = IsSdMusicDisabledForBoard()
                     ? StartOnlineMusicPromptFromMenu()
                     : OpenSdDirectoryPicker();
            break;
        case IdleScreen::MainMenuItem::OnlineMusic:
            SetSdMusicPlaybackMode(false);
            ok = StartOnlineMusicPromptFromMenu();
            break;
        case IdleScreen::MainMenuItem::Radio:
            SetSdMusicPlaybackMode(false);
            ok = OpenRadioPicker();
            break;
        case IdleScreen::MainMenuItem::AlarmClock:
            SetSdMusicPlaybackMode(false);
            ok = IsAlarmDisabledForBoard()
                     ? StartDeviceInfoPromptFromMenu()
                     : OpenAlarmListPicker();
            break;
        case IdleScreen::MainMenuItem::Setting:
            SetSdMusicPlaybackMode(false);
            ok = OpenSettingPicker();
            break;
        default:
            break;
    }
    if (menu_picker_ctx_ == MenuPickerContext::None) {
        idle_screen_->SelectMainMenuItem();
    }
    return ok;
#else
    return false;
#endif
}

void Application::SetSdMusicPlaybackMode(bool enabled) {
    sd_music_playback_mode_ = enabled;
    if (!enabled) {
        sd_selected_directory_.clear();
    }
}

bool Application::IsMediaPlaying() const {
    if (music_ && (music_->IsPlaying() || music_->IsDownloading())) return true;
    if (radio_ && (radio_->IsPlaying() || radio_->IsDownloading())) return true;
    if (sd_music_) {
        if (sd_music_->IsPlaying()) return true;
        auto st = sd_music_->GetState();
        if (st == Esp32SdMusic::PlayerState::Preparing ||
            st == Esp32SdMusic::PlayerState::Playing ||
            st == Esp32SdMusic::PlayerState::Paused) {
            return true;
        }
    }
#ifdef CONFIG_SD_CARD_ENABLE
    if (sd_video_ && sd_video_->GetState() == VideoPlayerState::Playing) return true;
#endif
    return false;
}

/* ------------------------------------------------------------------
 * EnsureIdleForMedia — transition device to idle before media playback
 *
 * Keeps media components fully decoupled from Application state
 * management. Media players can call this function to ensure the 
 * device is in the correct state for playback without needing to know 
 * the details of the state machine.
 * ------------------------------------------------------------------ */
bool Application::EnsureIdleForMedia() {
    DeviceState ds = device_state_;

    // Already idle or in a non-interactive state — good to go
    if (ds == kDeviceStateIdle || ds == kDeviceStateUnknown) {
        return true;
    }

    // Only transition from Listening / Speaking to Idle
    if (ds != kDeviceStateListening && ds != kDeviceStateSpeaking) {
        ESP_LOGW(TAG, "EnsureIdleForMedia: unexpected state %s, forcing idle",
                 STATE_STRINGS[ds]);
        SetDeviceState(kDeviceStateIdle);
        return true;
    }

    // Toggle chat to end the conversation and return to idle
    constexpr int kMaxRetries = 10;
    constexpr int kRetryDelayMs = 200;
    for (int i = 0; i < kMaxRetries; ++i) {
        ToggleChatState();
        vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
        ds = device_state_;
        if (ds == kDeviceStateIdle) {
            ESP_LOGI(TAG, "EnsureIdleForMedia: entered idle after %d toggle(s)", i + 1);
            return true;
        }
    }

    ESP_LOGW(TAG, "EnsureIdleForMedia: timeout — forcing idle");
    SetDeviceState(kDeviceStateIdle);
    return true;
}

/* ------------------------------------------------------------------
 * StopOtherMedia — centralized media teardown
 * ------------------------------------------------------------------ */
void Application::StopOtherMedia(MediaComponent except) {
    if (except != MediaComponent::kMusic && music_ &&
        (music_->IsPlaying() || music_->IsDownloading())) {
        ESP_LOGI(TAG, "StopOtherMedia: stopping music");
        music_->StopStreaming();
    }
    if (except != MediaComponent::kRadio && radio_ &&
        (radio_->IsPlaying() || radio_->IsDownloading())) {
        ESP_LOGI(TAG, "StopOtherMedia: stopping radio");
        radio_->Stop();
    }
#ifdef CONFIG_SD_CARD_ENABLE
    if (except != MediaComponent::kSdMusic && sd_music_ &&
        sd_music_->GetState() != Esp32SdMusic::PlayerState::Stopped &&
        sd_music_->GetState() != Esp32SdMusic::PlayerState::Error) {
        ESP_LOGI(TAG, "StopOtherMedia: stopping SD music");
        sd_music_->Stop();
        SetSdMusicPlaybackMode(false);
    }
    if (except != MediaComponent::kVideo && sd_video_) {
        auto state = sd_video_->GetState();
        if (state == VideoPlayerState::Playing || state == VideoPlayerState::Paused) {
            ESP_LOGI(TAG, "StopOtherMedia: stopping video");
            sd_video_->Stop();
        }
    }
#endif
}

void Application::SetupAudioPlayerCallback(AudioStreamPlayer* player) {
    if (!player) return;

    // Forward FFT PCM data to the MusicVisualizer or OLED SpectrumManager
    player->SetFftCallback([this](int16_t* pcm_data, size_t pcm_bytes) {
        if (music_visualizer_ && music_visualizer_->IsRunning()) {
            music_visualizer_->FeedAudioData(pcm_data, pcm_bytes);
        }
        if (oled_spectrum_mgr_ && oled_spectrum_mgr_->IsRunning()) {
            oled_spectrum_mgr_->FeedAudioData(pcm_data, pcm_bytes);
        }
    });

    player->SetPcmCallback([this](int16_t* pcm_data, int total_samples, int channels, int sample_rate) {
        audio_service_.UpdateOutputTimestamp();
    });

    // Manage MusicVisualizer / OLED spectrum lifecycle via player state transitions
    player->SetStateCallback([this](AudioPlayerState old_state, AudioPlayerState new_state) {
        auto display = Board::GetInstance().GetDisplay();
        auto* disp = lv_display_get_default();
        auto cf = lv_display_get_color_format(disp);

        if (new_state == AudioPlayerState::Playing) {
            EnsureIdleForMedia();

            if (cf != LV_COLOR_FORMAT_I1) {
                Display* lcd  = display;
                // ── LCD path: full MusicVisualizer (spectrum + music UI overlay) ──
                if (!music_visualizer_) {
                    music_visualizer_ = std::make_unique<music::MusicVisualizer>();
                }
                auto* viz = music_visualizer_.get();

                // Wire callbacks (safe to call multiple times)
                viz->SetOverlayCallback([lcd](bool active) {
                    lcd->SetMediaOverlayActive(active);
                });
                viz->SetInfoProvider([this]() -> music::MusicInfo {
                    return BuildMusicInfo();
                });
                viz->SetFontProvider([lcd](const lv_font_t** text_font, const lv_font_t** icon_font) {
                    auto* theme = static_cast<LvglTheme*>(lcd->GetTheme());
                    if (theme) {
                        *text_font = theme->text_font()->font();
                        *icon_font = theme->large_icon_font()->font();
                    }
                });

                // Compute status bar height for canvas positioning
                int status_h = 0;
                if (lvgl_port_lock(1000)) {
                    lv_obj_t* container = lv_obj_get_child(lv_screen_active(), 0);
                    lv_obj_t* sb = container ? lv_obj_get_child(container, 0) : nullptr;
                    if (sb) status_h = lv_obj_get_height(sb);
                    lvgl_port_unlock();
                }

                music::VisualizerConfig cfg;
                cfg.canvas_x      = 0;
                cfg.canvas_y      = status_h;
                cfg.canvas_width  = lcd->width();
                cfg.canvas_height = lcd->height() - status_h;
                cfg.lcd_height = lcd->height();
                cfg.lcd_width = lcd->width();
                cfg.status_bar_h = status_h;
                cfg.audio_buf_size = AUDIO_PCM_OUT_BUF_SIZE;

                // Provide initial info snapshot so first UI frame is populated
                viz->Start(cfg, BuildMusicInfo());
                ESP_LOGI(TAG, "MusicVisualizer started for LCD display with status bar height %d", status_h);
            } else {
                Display* oled  = display;
                // ── OLED path: lightweight monochrome spectrum (no music UI) ──
                if (oled_spectrum_mgr_ && oled_spectrum_mgr_->IsRunning()) {
                    return;  // already running
                }

                // Compute status bar height
                int status_h = 16;  // OLED default
                if (lvgl_port_lock(1000)) {
                    lv_obj_t* container = lv_obj_get_child(lv_screen_active(), 0);
                    lv_obj_t* sb = container ? lv_obj_get_child(container, 0) : nullptr;
                    if (sb) status_h = lv_obj_get_height(sb);
                    lvgl_port_unlock();
                }

                spectrum::SpectrumConfig scfg;
                scfg.monochrome     = true;
                scfg.fft_size       = 256;
                scfg.bar_count      = 16;
                scfg.canvas_x       = 0;
                scfg.canvas_y       = status_h;
                scfg.canvas_width   = oled->width();                  // 128
                scfg.canvas_height  = oled->height() - status_h;     // 48 or 16
                scfg.lcd_width      = oled->width();
                scfg.lcd_height     = oled->height();
                scfg.status_bar_h   = status_h;
                scfg.bar_max_height = scfg.canvas_height;
                scfg.task_stack_size = 3 * 1024;
                scfg.task_priority   = 1;
                scfg.task_core       = 0;

                oled_spectrum_mgr_ = std::make_unique<spectrum::SpectrumManager>(scfg);
                oled_spectrum_mgr_->AllocateAudioBuffer(AUDIO_PCM_OUT_BUF_SIZE);
                oled_spectrum_mgr_->Start();
                oled->SetMediaOverlayActive(true);
                ESP_LOGI(TAG, "OLED SpectrumManager started with status bar height %d", status_h);
            }
        } else if (old_state == AudioPlayerState::Playing &&
                   (new_state == AudioPlayerState::Idle ||
                    new_state == AudioPlayerState::Stopping)) {
            if (music_visualizer_) {
                music_visualizer_->Stop();
            }
            if (oled_spectrum_mgr_) {
                oled_spectrum_mgr_->Stop();
                display->SetMediaOverlayActive(false);
            }
        }
    });

    ESP_LOGI(TAG, "SetupAudioPlayerCallback: MusicVisualizer callbacks installed");
}

/**
 * @brief Build a MusicInfo snapshot by auto-detecting the active player.
 *
 * Checks sd_music_, music_, and radio_ in priority order.
 * Returns a data-only struct — the MusicVisualizer never touches
 * any concrete player object.
 */
music::MusicInfo Application::BuildMusicInfo() {
    music::MusicInfo info;

    // 1. SD Card player (highest priority — has richest metadata)
    if (sd_music_ && sd_music_->IsPlaying()) {
        info.source       = music::SourceType::SD_CARD;
        info.is_playing   = true;
        info.title        = sd_music_->GetCurrentTrack();
        info.position_ms  = sd_music_->GetCurrentPositionMs();
        info.duration_ms  = sd_music_->GetDurationMs();
        info.bitrate_kbps = sd_music_->GetBitrate();
        if (info.bitrate_kbps > 1000) info.bitrate_kbps /= 1000;

        char sub[64];
        snprintf(sub, sizeof(sub), "%d kbps  •  %s",
                 info.bitrate_kbps, sd_music_->GetDurationString().c_str());
        info.sub_info = sub;

        // Find next track
        auto tracks = sd_music_->GetPlaylist();
        std::string cur_path = sd_music_->GetCurrentTrackPath();
        int idx = -1;
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].path == cur_path) { idx = static_cast<int>(i); break; }
        }
        if (idx >= 0 && idx < static_cast<int>(tracks.size()) - 1) {
            info.next_track = tracks[idx + 1].name;
        } else if (!tracks.empty()) {
            info.next_track = tracks[0].name;
        }

        // ESP_LOGI(TAG, "BuildMusicInfo: SD card track='%s' pos=%lldms dur=%lldms bitrate=%dkbps next='%s'",
        //          info.title.c_str(), info.position_ms, info.duration_ms, info.bitrate_kbps, info.next_track.c_str());

        return info;
    }

    // 2. Online music player
    if (music_ && music_->IsPlaying()) {
        info.source       = music::SourceType::ONLINE;
        info.is_playing   = true;
        info.title        = music_->GetTitle();
        info.position_ms  = music_->GetPositionMs();
        info.duration_ms  = music_->GetDurationMs();
        info.bitrate_kbps = music_->GetBitrateKbps();

        std::string artist = music_->GetArtist();
        if (!artist.empty()) {
            if (info.bitrate_kbps > 0) {
                char sub[96];
                snprintf(sub, sizeof(sub), "%s  •  %d kbps", artist.c_str(), info.bitrate_kbps);
                info.sub_info = sub;
            } else {
                info.sub_info = artist;
            }
        } else if (info.bitrate_kbps > 0) {
            info.sub_info = std::to_string(info.bitrate_kbps) + " kbps";
        } else {
            info.sub_info = "Streaming...";
        }

        // ESP_LOGI(TAG, "BuildMusicInfo: Online track='%s' artist='%s' pos=%lldms dur=%lldms bitrate=%dkbps",
        //          info.title.c_str(), artist.c_str(), info.position_ms, info.duration_ms, info.bitrate_kbps);
        return info;
    }

    // 3. Internet radio
    if (radio_ && radio_->IsPlaying()) {
        info.source     = music::SourceType::RADIO;
        info.is_playing = true;
        info.title      = radio_->GetCurrentStation();
        info.sub_info   = "Live Broadcast";
        if (info.title.empty()) info.title = "FM Radio";
        return info;
    }

    return info;  // SourceType::NONE
}

/* ==================================================================
 * Component Initializers
 * ================================================================== */

bool Application::InitMusic() {
    music_ = new Esp32Music();
    if (!music_) {
        ESP_LOGE(TAG, "InitMusic: allocation failed");
        return false;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    music_->Initialize(codec);
    SetupAudioPlayerCallback(music_);

    McpFeatureTools::RegisterMusicTools(music_);
    ESP_LOGI(TAG, "InitMusic: online music player ready");
    return true;
}

bool Application::InitRadio() {
    radio_ = new Esp32Radio();
    if (!radio_) {
        ESP_LOGE(TAG, "InitRadio: allocation failed");
        return false;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    radio_->Initialize(codec);
    SetupAudioPlayerCallback(radio_);

    McpFeatureTools::RegisterRadioTools(radio_);
    ESP_LOGI(TAG, "InitRadio: internet radio player ready");
    return true;
}

bool Application::InitAlarmClock() {
#if CONFIG_USE_ALARM
    if (IsAlarmDisabledForBoard()) {
        ESP_LOGI(TAG, "InitAlarmClock: disabled for board %s", BOARD_TYPE);
        return true;
    }
    general_timer_ = std::make_unique<GeneralTimer>();
    alarm_event_active_ = false;
    alarm_started_at_ = 0;
    alarm_media_resume_ = AlarmMediaResumeState{};
    McpFeatureTools::RegisterAlarmTools();
    LoadDefaultAlarmsIfEmpty();
    ESP_LOGI(TAG, "InitAlarmClock: alarm clock ready");
    return true;
#else
    return false;
#endif
}

#if CONFIG_USE_ALARM
void Application::OnAlarmDismissed() {
    alarm_started_at_ = 0;
    RestoreMediaAfterAlarm();
}

void Application::SnapshotAndPauseMediaForAlarm() {
    if (alarm_media_resume_.active) {
        return;
    }

    alarm_media_resume_ = AlarmMediaResumeState{};
    alarm_media_resume_.active = true;

    if (radio_ && radio_->IsPlaying()) {
        alarm_media_resume_.had_radio = true;
        alarm_media_resume_.radio_station = radio_->GetCurrentStation();
    }
#ifdef CONFIG_SD_CARD_ENABLE
    if (sd_music_ && IsSdMusicPlaybackMode()) {
        alarm_media_resume_.had_sd_music = true;
        alarm_media_resume_.sd_track_index = sd_music_->GetCurrentIndex();
    }
#endif

    StopOtherMedia();
}

void Application::RestoreMediaAfterAlarm() {
    if (!alarm_media_resume_.active) {
        return;
    }

    AlarmMediaResumeState resume = alarm_media_resume_;
    alarm_media_resume_ = AlarmMediaResumeState{};

    if (device_state_ == kDeviceStateAlarm) {
        SetDeviceState(kDeviceStateIdle);
    }

    if (resume.had_radio && radio_ && !resume.radio_station.empty()) {
        const std::string station = resume.radio_station;
        ESP_LOGI(TAG, "Alarm resume: queue restore radio '%s'", station.c_str());
        Schedule([station]() {
            auto& app = Application::GetInstance();
            if (app.HasAlarmEvent()) {
                ESP_LOGW(TAG, "Alarm resume: skip radio restore while alarm still active");
                return;
            }
            // Delay radio resume slightly to let alarm/audio teardown settle.
            auto* station_copy = new std::string(station);
            BaseType_t created = xTaskCreate(
                [](void* arg) {
                    std::unique_ptr<std::string> owned_station(static_cast<std::string*>(arg));
                    vTaskDelay(pdMS_TO_TICKS(700));
                    Application::GetInstance().Schedule([station_text = *owned_station]() {
                        auto& scheduled_app = Application::GetInstance();
                        if (scheduled_app.HasAlarmEvent()) {
                            ESP_LOGW(TAG, "Alarm resume: alarm re-activated, skip radio restore");
                            return;
                        }
                        scheduled_app.EnsureIdleForMedia();
                        ESP_LOGI(TAG, "Alarm resume: restoring radio '%s'", station_text.c_str());
                        scheduled_app.PlayRadio(station_text);
                    });
                    vTaskDelete(nullptr);
                },
                "alarm_radio_resume",
                3072,
                station_copy,
                3,
                nullptr);
            if (created != pdPASS) {
                ESP_LOGE(TAG, "Alarm resume: failed to create delayed radio resume task");
                delete station_copy;
            }
        });
        return;
    }

#ifdef CONFIG_SD_CARD_ENABLE
    if (resume.had_sd_music && sd_music_) {
        const int track_index = resume.sd_track_index;
        ESP_LOGI(TAG, "Alarm resume: queue restore SD track index=%d", track_index);
        Schedule([track_index]() {
            auto& app = Application::GetInstance();
            if (app.HasAlarmEvent()) {
                ESP_LOGW(TAG, "Alarm resume: skip SD restore while alarm still active");
                return;
            }
            auto* sd_music = app.GetSdMusic();
            if (!sd_music) {
                return;
            }
            ESP_LOGI(TAG, "Alarm resume: restoring SD track index=%d", track_index);
            if (track_index >= 0 &&
                track_index < static_cast<int>(sd_music->GetTotalTracks())) {
                sd_music->SetTrack(track_index);
            } else {
                sd_music->Play();
            }
        });
        return;
    }
#endif
}
#endif

bool Application::InitSdMusic() {
#ifdef CONFIG_SD_CARD_ENABLE
    auto sd_card = Board::GetInstance().GetSdCard();
    if (!sd_card) {
        ESP_LOGW(TAG, "InitSdMusic: no SD card available");
        return false;
    }

    sd_music_ = new Esp32SdMusic();
    if (!sd_music_) {
        ESP_LOGE(TAG, "InitSdMusic: allocation failed");
        return false;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    sd_music_->Initialize(sd_card, codec);

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        const char* msg = (std::string(Lang::CODE) == "vi-VN" || std::string(Lang::CODE) == "vi") ? "Quét thẻ nhớ..." : "Scanning SD...";
        display->ShowNotification(msg, 3000);
        display->SetStatus(msg);
    }

    sd_music_->LoadPlaylist();
    SetupAudioPlayerCallback(sd_music_);

    McpFeatureTools::RegisterSdMusicTools(sd_music_);
    ESP_LOGI(TAG, "InitSdMusic: SD card music player ready");
    return true;
#else
    return false;
#endif
}

bool Application::InitVideo() {
#ifdef CONFIG_SD_CARD_ENABLE
    auto& board = Board::GetInstance();
    auto sd_card = board.GetSdCard();
    if (!sd_card) {
        ESP_LOGW(TAG, "InitVideo: no SD card available");
        return false;
    }
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();
    // --- Get the raw LCD panel handle (bypasses LVGL for max FPS) ---
    auto* lcd = dynamic_cast<LcdDisplay*>(display);
    if (lcd != nullptr) {
        sd_video_ = &VideoPlayer::GetInstance();
        // Initialize: pass LCD panel handle, resolution, codec, SD card
        bool ok = sd_video_->Initialize(
            lcd->GetPanelHandle(),
            static_cast<uint16_t>(lcd->width()),
            static_cast<uint16_t>(lcd->height()),
            codec,
            sd_card,
            display,   // Pass Display* for LVGL canvas support
            // Choose render mode for testing:
            //   VideoRenderMode::DirectLcd  — bypass LVGL, max FPS (default not stable)
            //   VideoRenderMode::LvglCanvas — through LVGL canvas pipeline
            VideoRenderMode::LvglCanvas
        );

        if (ok) {
            // Scan /sdcard/videos/ for .avi files and build playlist
            size_t found = sd_video_->ScanDirectory();
            ESP_LOGI(TAG, "InitVideo: found %d video files on SD card", found);

            // Manage main application UI lifecycle during video playback.
            // Hide emoji/chat/idle card when video starts playing, restore
            // when stopped — mirrors SetupAudioPlayerCallback() pattern for audio.
            sd_video_->SetStateCallback([](VideoPlayerState old_state,
                                        VideoPlayerState new_state) {
                auto display = Board::GetInstance().GetDisplay();
                if (!display) return;

                if (new_state == VideoPlayerState::Playing) {
                    // Activate media overlay to hide main UI (emoji/chat/idle card)
                    display->SetMediaOverlayActive(true);
                    ESP_LOGI(TAG, "Video playing: main UI hidden via media overlay");
                } else if (old_state == VideoPlayerState::Playing &&
                        (new_state == VideoPlayerState::Idle ||
                            new_state == VideoPlayerState::Stopping)) {
                    display->SetMediaOverlayActive(false);
                    ESP_LOGI(TAG, "Video stopped: main UI restored via media overlay");
                }
            });

            sd_video_->SetClockSyncCallback([](uint32_t rate, uint8_t bits, uint8_t channels) {
                // This callback is called in the video decoding thread right before starting playback, so we need to be careful about performance.
                // so it's a good place to ensure the device is idle and ready for media without blocking the main thread.
                // When playback starts, ensure device is idle and ready for media
                ESP_LOGI(TAG, "Video clock sync callback: ensuring idle for media");
                Application::GetInstance().EnsureIdleForMedia();
            });

            sd_video_->SetAudioCallback([this](int16_t* pcm, size_t samples, int channels) {
                // This callback is called in the video decoding thread, so we need to be careful about performance.
                // We can use this callback to update the output timestamp for synchronization purposes.
                audio_service_.UpdateOutputTimestamp();
            });
        }

        McpFeatureTools::RegisterSdVideoTools(sd_video_);
        ESP_LOGI(TAG, "InitVideo: video player ready");
    } else {
        ESP_LOGW(TAG, "InitVideo: display is not LCD, video player not initialized");
    }
    return true;
#else
    return false;
#endif
}

// --- [DienBien Mod]- WEATHER SCREEN UPDATE----
#ifdef CONFIG_WEATHER_IDLE_DISPLAY_ENABLE
void Application::StartWeatherIdleTask() {
    xTaskCreate([](void* arg) {
        Application* app = static_cast<Application*>(arg);
        int tick_count = 0;
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            tick_count++;

            if (app->GetDeviceState() == kDeviceStateIdle) {
                if (app->IsMediaPlaying()) {
                    // When music/radio is playing, hide the idle screen
                    auto display = Board::GetInstance().GetDisplay();
                    display->HideIdleCard();
                } else {
                    // Update the clock every second
                    app->UpdateIdleDisplay();

                    // Weather fetch logic: call at the 5th second after boot OR every 30 minutes (1800 seconds)
                    if (tick_count == 5 || tick_count % 1800 == 0) {
                        ESP_LOGI(TAG, "Khoi tao Task lay thoi tiet...");
                        auto& weather_service = WeatherService::GetInstance();
                        if (weather_service.FetchWeatherData()) {
                            app->UpdateIdleDisplay();
                        }
                    }
                }
            }
        }
        vTaskDelete(NULL);
    }, "weather_idle_task", 1024 * 6, this, 2, &weather_idle_task_handle_);
}

void Application::UpdateIdleDisplay() {
    auto& weather_service = WeatherService::GetInstance();
    const WeatherInfo& weather_info = weather_service.GetWeatherInfo();
    
    IdleCardInfo card;
    {
        Settings wifi_settings("wifi", false);
        std::string child_name = wifi_settings.GetString("child_name", "Buddy");
        if (child_name.empty()) {
            child_name = "Buddy";
        }
        card.user_text = "Hi, " + child_name;
    }

    // 2. THÔNG TIN HỆ THỐNG
    auto& board = Board::GetInstance();
    card.network_icon = board.GetNetworkStateIcon();
    if (board.GetBoardType() == "wifi") {
        auto& wifi_station = WifiStation::GetInstance();
        card.rssi = wifi_station.GetRssi();
    } else {
        AtModem* cellular_modem = static_cast<AtModem*>(board.GetNetwork());
        card.rssi = cellular_modem ? cellular_modem->GetCsq() : 0;
    }

    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* const levels[] = {
                FONT_AWESOME_BATTERY_EMPTY,            // 0-19%
                FONT_AWESOME_BATTERY_QUARTER,          // 20-39%
                FONT_AWESOME_BATTERY_HALF,             // 40-59%
                FONT_AWESOME_BATTERY_THREE_QUARTERS,   // 60-79%
                FONT_AWESOME_BATTERY_FULL,             // 80-99%
                FONT_AWESOME_BATTERY_FULL              // 100%
            };
            icon = levels[battery_level / 20];
        }
        card.battery_level = battery_level;
        card.battery_icon = icon;
        card.is_charging = charging;
    } else {
        card.battery_icon = FONT_AWESOME_BATTERY_BOLT;
        card.battery_level = -1; // Không biết mức pin
        card.is_charging = false;
    }

    // 3. THÔNG TIN THỜI TIẾT
    if (weather_info.valid) {
        card.city = weather_info.city;
        
        char temp_buf[16];
        snprintf(temp_buf, sizeof(temp_buf), "%d°C", (int)round(weather_info.temp));
        card.temperature_text = temp_buf;

        card.description_text = weather_info.description;
        card.humidity_text = std::to_string(weather_info.humidity) + "%";

        char extra_buf[32];
        snprintf(extra_buf, sizeof(extra_buf), "%.1f m/s", weather_info.wind_speed);
        card.wind_text = extra_buf;

        // Cập nhật Forecast vào Card
        card.forecast = weather_info.forecast; // COPY DỮ LIỆU DỰ BÁO SANG UI

        card.icon = WeatherUI::GetWeatherIcon(weather_info.icon_code);
    } else {
        card.city = "Dang cap nhat...";
        card.temperature_text = "--";
        card.icon = "\uf128"; 
    }

    auto display = Board::GetInstance().GetDisplay();
    display->ShowIdleCard(card);
}
#endif
// --- [DienBien Mod]- END WEATHER SCREEN UPDATE----
