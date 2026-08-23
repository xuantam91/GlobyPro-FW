#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#ifdef CONFIG_SD_CARD_MMC_INTERFACE
#include "sdmmc.h"
#elif defined(CONFIG_SD_CARD_SPI_INTERFACE)
#include "sdspi.h"
#endif
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "power_manager.h"
#include "settings.h"
#include "system_info.h"
#include "features/QRCode/qrcode_display.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_station.h>

#include <driver/rtc_io.h>
#include <cctype>
#include <string>

#define TAG "LUXIAOBAN_XIAOZHI_1_54TFT"

namespace {
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

}  // namespace

class LUXIAOBAN_XIAOZHI_1_54TFT : public WifiBoard {
private:
    static constexpr gpio_num_t kSpeakerPowerGpio = GPIO_NUM_21;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    SpiLcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    bool speaker_auto_off_ = false;

    void SetSpeakerPower(bool enabled) {
        rtc_gpio_hold_dis(kSpeakerPowerGpio);
        rtc_gpio_set_level(kSpeakerPowerGpio, enabled ? 1 : 0);
        rtc_gpio_hold_en(kSpeakerPowerGpio);
    }

    void WakeFromUserActivity() {
        power_save_timer_->WakeUp();
        if (speaker_auto_off_) {
            SetSpeakerPower(true);
            auto codec = GetAudioCodec();
            codec->EnableOutput(true);
            speaker_auto_off_ = false;
        }
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_38);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            // Keep inactivity timer running regardless of charging state so
            // "auto speaker off after timeout" remains effective.
            (void)is_charging;
            power_save_timer_->SetEnabled(true);
        });
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(kSpeakerPowerGpio);
        rtc_gpio_set_direction(kSpeakerPowerGpio, RTC_GPIO_MODE_OUTPUT_ONLY);
        SetSpeakerPower(true);

        power_save_timer_ = new PowerSaveTimer(-1, SECONDS_TO_SLEEP_MODE, SECONDS_TO_SHUTDOWN);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            if (speaker_auto_off_) {
                return;
            }
            ESP_LOGI(TAG, "No activity for 1 hour: speaker output off");
            auto codec = GetAudioCodec();
            codec->EnableOutput(false);
            SetSpeakerPower(false);
            speaker_auto_off_ = true;
            GetDisplay()->ShowNotification("Speaker auto off");
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnMultipleClick([this]() {
            ResetWifiConfiguration();
        }, 5);

        boot_button_.OnClick([this]() {
            WakeFromUserActivity();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            WakeFromUserActivity();
            auto& qr = qrcode::QRCodeDisplay::GetInstance();
            if (qr.IsDisplayed()) {
                qr.Clear();
                GetDisplay()->ShowNotification("Role QR hidden");
                return;
            }

            std::string portal_url = BuildRolePortalUrlFromMac();
            bool ok = !portal_url.empty() &&
                      qr.Show(portal_url, "Cau hinh Tro ly\nKhoi dong lai Loa de ap dung Cau hinh moi.");
            GetDisplay()->ShowNotification(ok ? "Role QR shown" : "Role QR unavailable");
        });

        volume_up_button_.OnClick([this]() {
            WakeFromUserActivity();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            WakeFromUserActivity();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            WakeFromUserActivity();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            WakeFromUserActivity();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                if (app.ToggleIdleTheme()) {
                    GetDisplay()->ShowNotification("Idle theme changed");
                } else {
                    GetDisplay()->ShowNotification("Idle theme unavailable");
                }
                return;
            }
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeSt7789Display() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));

        display_ = new SpiLcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

public:
    LUXIAOBAN_XIAOZHI_1_54TFT() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplexPdm audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_PDM_MIC_GPIO_SCK, AUDIO_PDM_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }

    virtual SdCard* GetSdCard() override {
        // Luxiaoban 1.54 does not provide SD card hardware.
        return nullptr;
    }
};

DECLARE_BOARD(LUXIAOBAN_XIAOZHI_1_54TFT);
