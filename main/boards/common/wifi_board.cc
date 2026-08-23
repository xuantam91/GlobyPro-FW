#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "features/QRCode/qrcode_display.h"
#include "application.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <font_awesome.h>
#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>
#include "afsk_demod.h"

// 添加蓝牙相关头文件
#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_blufi.h>
#endif

static const char *TAG = "WifiBoard";

namespace {

// RSSI weak threshold: only start roaming scans once the current AP drops below this level.
constexpr int8_t kRoamingWeakRssiThreshold = -66;
// Minimum RSSI improvement threshold: candidate AP must beat the current AP by at least this much.
constexpr int8_t kRoamingMinImprovementDb = 8;
// Cross-SSID improvement threshold: switching to another saved WiFi should require a larger gain.
constexpr int8_t kRoamingSavedSsidMinImprovementDb = 12;
// Roaming cooldown: minimum time between two AP switches to prevent ping-pong.
constexpr int64_t kRoamingCooldownMs = 60000;
// Critical RSSI threshold: allow breaking cooldown early if the current AP becomes unusably weak.
constexpr int8_t kRoamingCriticalRssiThreshold = -74;
// Check interval when signal is strong: keep monitoring light to save battery.
constexpr uint32_t kRoamingStrongCheckIntervalMs = 30000;
// Check interval when signal is weak: react faster, but still avoid continuous scanning.
constexpr uint32_t kRoamingWeakCheckIntervalMs = 10000;
constexpr uint32_t kRoamingReconnectCheckIntervalMs = 5000;
// When media (music/radio) is playing, extend intervals to avoid scan-blocking
// the WiFi driver and starving the HTTP audio pipeline.
constexpr uint32_t kRoamingMediaPlayingCheckIntervalMs = 60000;
constexpr uint32_t kRoamingTaskStackSize = 6144;
constexpr UBaseType_t kRoamingTaskPriority = 2;

bool UseLuxiaobanWifiSetupText() {
    const std::string_view board_type = BOARD_TYPE;
    return board_type == "luxiaoban-xiaozhi-1.54tft" || board_type == "jiuchuan-s3";
}

std::string FormatBssid(const uint8_t* bssid) {
    char buffer[18];
    snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    return buffer;
}

bool IsSameBssid(const uint8_t* left, const uint8_t* right) {
    return memcmp(left, right, 6) == 0;
}

struct RoamingCandidate {
    wifi_ap_record_t ap = {};
    std::string password;
    bool same_ssid = false;
    bool found = false;
};

struct StartupApCandidate {
    wifi_ap_record_t ap = {};
    std::string password;
    bool found = false;
};

bool FindBestStartupCandidate(const std::vector<wifi_ap_record_t>& scan_results,
    StartupApCandidate& best_candidate) {
    auto& ssid_manager = SsidManager::GetInstance();
    const auto& ssid_list = ssid_manager.GetSsidList();

    for (const auto& ap : scan_results) {
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [&ap](const SsidItem& item) {
            return strcmp(reinterpret_cast<const char*>(ap.ssid), item.ssid.c_str()) == 0;
        });
        if (it == ssid_list.end()) {
            continue;
        }

        best_candidate.ap = ap;
        best_candidate.password = it->password;
        best_candidate.found = true;
        return true;
    }
    return false;
}

bool FindBestRoamingCandidate(const std::vector<wifi_ap_record_t>& scan_results,
    const wifi_ap_record_t& current_ap, RoamingCandidate& best_same_ssid,
    RoamingCandidate& best_saved_ssid) {
    auto& ssid_manager = SsidManager::GetInstance();
    const auto& ssid_list = ssid_manager.GetSsidList();

    for (const auto& ap : scan_results) {
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [&ap](const SsidItem& item) {
            return strcmp(reinterpret_cast<const char*>(ap.ssid), item.ssid.c_str()) == 0;
        });
        if (it == ssid_list.end()) {
            continue;
        }

        const bool same_ssid =
            strcmp(reinterpret_cast<const char*>(ap.ssid), reinterpret_cast<const char*>(current_ap.ssid)) == 0;
        if (same_ssid && IsSameBssid(ap.bssid, current_ap.bssid)) {
            continue;
        }

        RoamingCandidate* target = same_ssid ? &best_same_ssid : &best_saved_ssid;
        if (!target->found || ap.rssi > target->ap.rssi) {
            target->ap = ap;
            target->password = it->password;
            target->same_ssid = same_ssid;
            target->found = true;
        }
    }

    return best_same_ssid.found || best_saved_ssid.found;
}

}  // namespace

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

#ifdef CONFIG_BLUFICONFIG_ENABLE
#include "boards/jiuchuan-s3/jiuchuan_s3_blufi_config.h"
void WifiBoard::EnterWifiConfigMode() {
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    //初始化
    JiuChuanS3BlufiConfigurationAp::GetInstance().EnterBluFiConfigMode();
    
    // 播报配置 WiFi 的提示
    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, "", "", Lang::Sounds::OGG_WIFICONFIG);
    std::string mac = SystemInfo::GetMacAddress();
    std::string serial_number = SystemInfo::GetSerialNumber();
    if (serial_number.empty()) {
        ESP_LOGW(TAG, "Serial number not available; QR code serialnumber will be empty");
    }
    std::string url = "https://app.boilon.cn/?mac=" + mac + "&serialnumber=" + serial_number;
    qrcode::QRCodeDisplay::GetInstance().Show(url, "请使用微信扫描二维码");
    // Wait forever until reset after configuration
    while (true) {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#else
void WifiBoard::EnterWifiConfigMode() {
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage(Lang::CODE);
    wifi_ap.SetSsidPrefix("GLOBY");
    wifi_ap.Start();

    // 等待 1.5 秒显示开发板信息
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Hiển thị SSID cấu hình WiFi và URL máy chủ web
    std::string hint;
    if (std::string(Lang::CODE) == "vi-VN" || std::string(Lang::CODE) == "vi") {
        hint = "Kết nối Wi-Fi: ";
        hint += wifi_ap.GetSsid();
        hint += "\nQuét QR code để kết nối Wifi.";
    } else if (UseLuxiaobanWifiSetupText()) {
        hint = "Kết nối Wi-Fi ";
        hint += wifi_ap.GetSsid();
        hint += "  |  Quét mã QR bằng camera hoặc vào 192.168.4.1 để cài đặt Wi-Fi";
    } else {
        hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_ap.GetSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_ap.GetWebServerUrl();
    }
    hint += "\n\n";
    
    // Phát âm thanh và hiển thị thông báo cấu hình WiFi
    const char* title = (std::string(Lang::CODE) == "vi-VN" || std::string(Lang::CODE) == "vi") ? "Cài đặt Wi-Fi" : (UseLuxiaobanWifiSetupText() ? "WiFi setup" : Lang::Strings::WIFI_CONFIG_MODE);
    application.Alert(title, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);

    // Hiển thị mã QR kết nối WiFi tự động cho điện thoại quét bằng Camera/Zalo
    auto& qr = qrcode::QRCodeDisplay::GetInstance();
    std::string wifi_qr_payload = "WIFI:S:" + wifi_ap.GetSsid() + ";;";
    std::string qr_text;
    if (std::string(Lang::CODE) == "vi-VN" || std::string(Lang::CODE) == "vi") {
        qr_text = "Quét QR code để kết nối Wifi\n" + wifi_ap.GetSsid();
    } else if (UseLuxiaobanWifiSetupText()) {
        qr_text = "WiFi setup\nKết nối Wi-Fi ";
        qr_text += wifi_ap.GetSsid();
        qr_text += " | Quét QR code để kết nối Wifi";
    } else {
        qr_text = "Wi-Fi Setup\nScan QR to connect to " + wifi_ap.GetSsid();
    }
    const bool qr_ok = qr.Show(wifi_qr_payload, qr_text);
    ESP_LOGI(TAG, "Wi-Fi config QR %s: %s", qr_ok ? "shown" : "failed", wifi_qr_payload.c_str());

    #if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    auto display = Board::GetInstance().GetDisplay();
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = 1;
    if (codec) {
        channel = codec->input_channels();
    }
    ESP_LOGI(TAG, "Start receiving WiFi credentials from audio, input channels: %d", channel);
    audio_wifi_config::ReceiveWifiCredentialsFromAudio(&application, &wifi_ap, display, channel);
    #endif
    
    // Wait forever until reset after configuration
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#endif

void WifiBoard::StartNetwork() {
    // User can press BOOT button while starting to enter WiFi configuration mode
    if (wifi_config_mode_) {
        EnterWifiConfigMode();
        return;
    }

    // If no WiFi SSID is configured, enter WiFi configuration mode
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
    });
    wifi_station.OnConnect([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECT_TO;
        notification += ssid;
        notification += "...";
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.OnConnected([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 30000);
        
        // Debug log: Print IP address when WiFi connected
        std::string ip_address = WifiStation::GetInstance().GetIpAddress();
        ESP_LOGI(TAG, "WiFi connected successfully - SSID: %s, IP Address: %s", ssid.c_str(), ip_address.c_str());
    });
    wifi_station.Start();

    // Force one startup scan and immediately connect to the strongest saved AP.
    // This makes initial association deterministic for mesh deployments with multiple nodes.
    bool startup_connect_triggered = false;
    if (wifi_station.Scan(nullptr, true, true)) {
        std::vector<wifi_ap_record_t> startup_scan_results;
        if (wifi_station.GetScanResults(startup_scan_results)) {
            StartupApCandidate best_startup_ap;
            if (FindBestStartupCandidate(startup_scan_results, best_startup_ap)) {
                const std::string target_ssid = reinterpret_cast<const char*>(best_startup_ap.ap.ssid);
                ESP_LOGI(TAG,
                    "Startup best AP selected: SSID=%s RSSI=%d CH=%d BSSID=%s",
                    target_ssid.c_str(), best_startup_ap.ap.rssi, best_startup_ap.ap.primary,
                    FormatBssid(best_startup_ap.ap.bssid).c_str());
                startup_connect_triggered = wifi_station.ConnectToAp(
                    target_ssid,
                    best_startup_ap.password,
                    best_startup_ap.ap.primary,
                    best_startup_ap.ap.bssid,
                    true);
                if (!startup_connect_triggered) {
                    ESP_LOGW(TAG, "Startup best AP handoff failed, fallback to default connect flow");
                }
            } else {
                ESP_LOGI(TAG, "Startup scan found no saved SSID, fallback to default connect flow");
            }
        } else {
            ESP_LOGW(TAG, "Startup scan results unavailable, fallback to default connect flow");
        }
    } else {
        ESP_LOGW(TAG, "Startup scan failed, fallback to default connect flow");
    }

    // Try to connect to WiFi, if failed, launch the WiFi configuration AP
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    StartRoamingMonitor();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }
    int8_t rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = R"({)";
    board_json += R"("type":")" + std::string(BOARD_TYPE) + R"(",)";
    board_json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    if (!wifi_config_mode_) {
        board_json += R"("ssid":")" + wifi_station.GetSsid() + R"(",)";
        board_json += R"("rssi":)" + std::to_string(wifi_station.GetRssi()) + R"(,)";
        board_json += R"("channel":)" + std::to_string(wifi_station.GetChannel()) + R"(,)";
        board_json += R"("ip":")" + wifi_station.GetIpAddress() + R"(",)";
    }
    board_json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    board_json += R"(})";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::StartRoamingMonitor() {
    if (roaming_task_handle_ != nullptr || wifi_config_mode_) {
        return;
    }
    initial_roaming_probe_pending_ = true;
    TaskHandle_t task_handle = nullptr;
    xTaskCreate(&WifiBoard::RoamingTask, "wifi_roam", kRoamingTaskStackSize, this,
        kRoamingTaskPriority, &task_handle);
    roaming_task_handle_ = task_handle;
}

void WifiBoard::RoamingTask(void* arg) {
    static_cast<WifiBoard*>(arg)->RoamingLoop();
}

void WifiBoard::RoamingLoop() {
    auto& wifi_station = WifiStation::GetInstance();

    while (true) {
        uint32_t delay_ms = kRoamingReconnectCheckIntervalMs;

        if (!wifi_config_mode_ && wifi_station.IsConnected()) {
            wifi_ap_record_t current_ap;
            if (wifi_station.GetCurrentApInfo(current_ap)) {
                const int8_t current_rssi = current_ap.rssi;

                // Check if audio media (music/radio) is actively playing.
                // When media is active, WiFi scans block the driver for 1-3s,
                // starving the HTTP audio pipeline and causing freezes.
                const auto& app = Application::GetInstance();
                const bool media_active = app.IsSdMusicPlaybackMode() || app.IsRadioPlaybackMode();

                if (media_active) {
                    // During media playback: only roam at critical RSSI, use long intervals
                    delay_ms = kRoamingMediaPlayingCheckIntervalMs;
                    if (current_rssi <= kRoamingCriticalRssiThreshold) {
                        // Signal is critically weak — allow scan but with extended cooldown
                        delay_ms = kRoamingWeakCheckIntervalMs;
                        ESP_LOGI(TAG, "Media playing but RSSI=%d is critical, allowing roaming scan", current_rssi);
                    } else {
                        ESP_LOGD(TAG, "Skip roaming: media playing, RSSI=%d above critical threshold", current_rssi);
                        vTaskDelay(pdMS_TO_TICKS(delay_ms));
                        continue;
                    }
                } else {
                    delay_ms = current_rssi > kRoamingWeakRssiThreshold ?
                        kRoamingStrongCheckIntervalMs : kRoamingWeakCheckIntervalMs;
                }

                if (initial_roaming_probe_pending_ && !media_active) {
                    delay_ms = std::min<uint32_t>(delay_ms, 3000);
                }

                const int64_t now_us = esp_timer_get_time();
                if (now_us < roaming_cooldown_until_us_) {
                    const int64_t cooldown_remaining_ms =
                        (roaming_cooldown_until_us_ - now_us + 999) / 1000;
                    if (current_rssi > kRoamingCriticalRssiThreshold) {
                        ESP_LOGI(TAG, "Skip roaming scan: cooldown active for %lld ms, current RSSI=%d BSSID=%s",
                            static_cast<long long>(cooldown_remaining_ms), current_rssi,
                            FormatBssid(current_ap.bssid).c_str());
                        delay_ms = std::min<uint32_t>(delay_ms, std::max<int64_t>(cooldown_remaining_ms, 1000));
                    } else {
                        ESP_LOGI(TAG, "Break roaming cooldown early: current RSSI=%d is below critical threshold %d dBm on BSSID=%s",
                            current_rssi, kRoamingCriticalRssiThreshold, FormatBssid(current_ap.bssid).c_str());
                    }
                }

                const bool do_initial_probe = initial_roaming_probe_pending_ && !media_active;
                if ((current_rssi <= kRoamingWeakRssiThreshold || do_initial_probe) &&
                    (now_us >= roaming_cooldown_until_us_ || current_rssi <= kRoamingCriticalRssiThreshold)) {
                    if (do_initial_probe) {
                        ESP_LOGI(TAG, "Initial roaming probe: SSID=%s BSSID=%s RSSI=%d channel=%d",
                            reinterpret_cast<const char*>(current_ap.ssid), FormatBssid(current_ap.bssid).c_str(),
                            current_ap.rssi, current_ap.primary);
                    } else {
                        ESP_LOGI(TAG, "Roaming check: current SSID=%s BSSID=%s RSSI=%d channel=%d media=%s",
                            reinterpret_cast<const char*>(current_ap.ssid), FormatBssid(current_ap.bssid).c_str(),
                            current_ap.rssi, current_ap.primary, media_active ? "yes" : "no");
                    }

                    wifi_scan_config_t scan_config = {};

                    if (wifi_station.Scan(&scan_config, true, false)) {
                        std::vector<wifi_ap_record_t> scan_results;
                        if (wifi_station.GetScanResults(scan_results)) {
                            RoamingCandidate same_ssid_candidate;
                            RoamingCandidate saved_ssid_candidate;
                            if (FindBestRoamingCandidate(scan_results, current_ap, same_ssid_candidate, saved_ssid_candidate)) {
                                bool roaming_started = false;
                                if (same_ssid_candidate.found) {
                                    const int improvement = same_ssid_candidate.ap.rssi - current_ap.rssi;
                                    ESP_LOGI(TAG, "Roaming candidate: SSID=%s BSSID=%s RSSI=%d channel=%d improvement=%d dBm mode=same-ssid",
                                        reinterpret_cast<const char*>(same_ssid_candidate.ap.ssid),
                                        FormatBssid(same_ssid_candidate.ap.bssid).c_str(),
                                        same_ssid_candidate.ap.rssi, same_ssid_candidate.ap.primary, improvement);
                                    if (improvement >= kRoamingMinImprovementDb) {
                                        const std::string target_ssid(reinterpret_cast<const char*>(same_ssid_candidate.ap.ssid));
                                        if (wifi_station.ConnectToAp(target_ssid, same_ssid_candidate.password,
                                                same_ssid_candidate.ap.primary, same_ssid_candidate.ap.bssid, true)) {
                                            roaming_cooldown_until_us_ = now_us + kRoamingCooldownMs * 1000;
                                            ESP_LOGI(TAG, "Roaming to SSID=%s BSSID=%s channel=%d, cooldown %lld ms",
                                                target_ssid.c_str(), FormatBssid(same_ssid_candidate.ap.bssid).c_str(),
                                                same_ssid_candidate.ap.primary, static_cast<long long>(kRoamingCooldownMs));
                                            roaming_started = true;
                                        } else {
                                            ESP_LOGW(TAG, "Skip roaming: cannot connect to same-SSID candidate AP");
                                        }
                                    } else {
                                        ESP_LOGI(TAG, "Skip same-SSID roaming: improvement %d dBm is below threshold %d dBm",
                                            improvement, kRoamingMinImprovementDb);
                                    }
                                }

                                if (!roaming_started && saved_ssid_candidate.found) {
                                    const int improvement = saved_ssid_candidate.ap.rssi - current_ap.rssi;
                                    ESP_LOGI(TAG, "Roaming candidate: SSID=%s BSSID=%s RSSI=%d channel=%d improvement=%d dBm mode=saved-ssid",
                                        reinterpret_cast<const char*>(saved_ssid_candidate.ap.ssid),
                                        FormatBssid(saved_ssid_candidate.ap.bssid).c_str(),
                                        saved_ssid_candidate.ap.rssi, saved_ssid_candidate.ap.primary, improvement);
                                    if (improvement >= kRoamingSavedSsidMinImprovementDb) {
                                        const std::string target_ssid(reinterpret_cast<const char*>(saved_ssid_candidate.ap.ssid));
                                        if (wifi_station.ConnectToAp(target_ssid, saved_ssid_candidate.password,
                                                saved_ssid_candidate.ap.primary, saved_ssid_candidate.ap.bssid, true)) {
                                            roaming_cooldown_until_us_ = now_us + kRoamingCooldownMs * 1000;
                                            ESP_LOGI(TAG, "Roaming to SSID=%s BSSID=%s channel=%d, cooldown %lld ms",
                                                target_ssid.c_str(), FormatBssid(saved_ssid_candidate.ap.bssid).c_str(),
                                                saved_ssid_candidate.ap.primary, static_cast<long long>(kRoamingCooldownMs));
                                            roaming_started = true;
                                        } else {
                                            ESP_LOGW(TAG, "Skip roaming: cannot connect to saved-SSID candidate AP");
                                        }
                                    } else {
                                        ESP_LOGI(TAG, "Skip cross-SSID roaming: improvement %d dBm is below threshold %d dBm",
                                            improvement, kRoamingSavedSsidMinImprovementDb);
                                    }
                                } else if (!roaming_started && !same_ssid_candidate.found) {
                                    ESP_LOGI(TAG, "Skip roaming: no better saved AP found for current SSID=%s",
                                        reinterpret_cast<const char*>(current_ap.ssid));
                                }
                            }
                        } else {
                            ESP_LOGW(TAG, "Roaming scan completed but results were unavailable");
                        }
                    } else {
                        ESP_LOGW(TAG, "Roaming scan could not start");
                    }
                    if (do_initial_probe) {
                        initial_roaming_probe_pending_ = false;
                    }
                } else if (current_rssi > kRoamingWeakRssiThreshold) {
                    ESP_LOGD(TAG, "Skip roaming scan: current RSSI=%d is above weak threshold %d dBm on BSSID=%s",
                        current_rssi, kRoamingWeakRssiThreshold, FormatBssid(current_ap.bssid).c_str());
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void WifiBoard::ResetWifiConfiguration() {
    // Set a flag and reboot the device to enter the network configuration mode
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    esp_restart();
}

std::string WifiBoard::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "wifi",
     *         "ssid": "Xiaozhi",
     *         "rssi": -60
     *     },
     *     "chip": {
     *         "temperature": 25
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    auto& wifi_station = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi_station.GetSsid().c_str());
    int rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        cJSON_AddStringToObject(network, "signal", "strong");
    } else if (rssi >= -70) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else {
        cJSON_AddStringToObject(network, "signal", "weak");
    }
    cJSON_AddItemToObject(root, "network", network);

    // Chip
    float esp32temp = 0.0f;
    if (board.GetTemperature(esp32temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", esp32temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
