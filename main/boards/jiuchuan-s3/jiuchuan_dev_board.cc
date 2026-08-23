#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "settings.h"
#ifdef CONFIG_SD_CARD_MMC_INTERFACE
#include "sdmmc.h"
#elif defined(CONFIG_SD_CARD_SPI_INTERFACE)
#include "sdspi.h"
#endif

#include <esp_log.h>
#include <esp_system.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "esp_lcd_panel_gc9301.h"
#include "esp_lcd_panel_jd9853.h"
#include "features/QRCode/qrcode_display.h"

#include "power_save_timer.h"
#include "power_manager.h"
#include "power_controller.h"
#include "gpio_manager.h"
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <cstdio>
#include <atomic>
#include <freertos/task.h>

#define BOARD_TAG "JiuchuanDevBoard"
#define __USER_GPIO_PWRDOWN__

namespace {
constexpr int kLcdDriverGc9301 = 1;
constexpr int kLcdDriverJd9853 = 2;
constexpr int kDefaultLcdDriver = JIUCHUAN_DEFAULT_LCD_DRIVER;

int ResolveJiuchuanLcdDriverType() {
    Settings settings("display", false);
    int driver = settings.GetInt("lcd_driver", kDefaultLcdDriver);
    if (driver != kLcdDriverGc9301 && driver != kLcdDriverJd9853) {
        driver = kDefaultLcdDriver;
    }
    return driver;
}
}  // namespace

#ifdef CONFIG_JIUCHUAN_S3_V2
#include "audio/codecs/box_audio_codec.h"
#include <esp_timer.h>

class JiuchuanAudioCodec : public BoxAudioCodec {
private:
    gpio_num_t pa_pin_;
    bool pa_initialized_;

public:
    JiuchuanAudioCodec(i2c_master_bus_handle_t i2c_bus, 
                       int input_sample_rate, int output_sample_rate,
                       gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, 
                       gpio_num_t dout, gpio_num_t din,
                       gpio_num_t pa_pin, 
                       uint8_t es8311_addr, uint8_t es7210_addr, 
                       bool input_reference)
        : BoxAudioCodec(i2c_bus, input_sample_rate, output_sample_rate,
                       mclk, bclk, ws, dout, din, 
                       GPIO_NUM_NC,  // 不让ES8311驱动控制PA引脚
                       es8311_addr, es7210_addr, input_reference),
          pa_pin_(pa_pin),
          pa_initialized_(false) {
        
        ESP_LOGI(BOARD_TAG, "JiuchuanAudioCodec initialized (ES8311+ES7210)");
    }

    virtual void EnableOutput(bool enable) override {
        // 延迟初始化PA引脚（第一次调用EnableOutput时才初始化）
        if (!pa_initialized_ && pa_pin_ != GPIO_NUM_NC) {
            gpio_reset_pin(pa_pin_);  // 先复位，清除任何之前的配置
            
            gpio_config_t io_conf = {};
            io_conf.intr_type = GPIO_INTR_DISABLE;
            io_conf.mode = GPIO_MODE_OUTPUT;
            io_conf.pin_bit_mask = (1ULL << pa_pin_);
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
            gpio_config(&io_conf);
            
            pa_initialized_ = true;
            ESP_LOGI(BOARD_TAG, "PA pin GPIO%d initialized (lazy init)", pa_pin_);
        }
        
        BoxAudioCodec::EnableOutput(enable);
        
        // 控制PA引脚
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, enable ? 1 : 0);
            ESP_LOGI(BOARD_TAG, "PA pin GPIO%d set to %d", pa_pin_, enable ? 1 : 0);
        }
    }
};
#endif

// 自定义LCD显示器类，用于圆形屏幕适配
class CustomLcdDisplay : public SpiLcdDisplay
{
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy)
    {

        DisplayLockGuard lock(this);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.167, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.167, 0);
    }
};

class JiuchuanDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    Button pwr_button_;
    Button wifi_button;
    Button cmd_button;
    LcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    std::atomic<int> volume_hold_direction_{0};  // +1 up, -1 down, 0 stop
    std::atomic<bool> volume_hold_task_running_{false};
    TaskHandle_t emergency_reset_task_handle_ = nullptr;

    void MaybeForceOldLcdDriverByBootKey() {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << CMD_BUTTON_GPIO) | (1ULL << WIFI_BUTTON_GPIO);
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&io_conf);

        ESP_LOGI(TAG, "Rescue check start: CMD=%d WIFI=%d",
            gpio_get_level(CMD_BUTTON_GPIO), gpio_get_level(WIFI_BUTTON_GPIO));

        const int hold_ms = 3500;
        const int step_ms = 100;
        int pressed_ms = 0;
        while (pressed_ms < hold_ms) {
            const bool cmd_pressed = (gpio_get_level(CMD_BUTTON_GPIO) == 0);
            const bool wifi_pressed = (gpio_get_level(WIFI_BUTTON_GPIO) == 0);
            if (!cmd_pressed && !wifi_pressed) {
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(step_ms));
            pressed_ms += step_ms;
        }

        Settings display_settings("display", true);
        display_settings.SetInt("lcd_driver", kLcdDriverGc9301);
        ESP_LOGW(TAG, "Boot key rescue: force lcd_driver=%d (GC9301)", kLcdDriverGc9301);
    }

    // 音量映射函数：将内部音量(0-80)映射为显示音量(0-100%)
    int MapVolumeForDisplay(int internal_volume) {
        // 确保输入在有效范围内
        if (internal_volume < 0) internal_volume = 0;
        if (internal_volume > 80) internal_volume = 80;
        
        // 将0-80映射到0-100
        // 公式: 显示音量 = (内部音量 / 80) * 100
        return (internal_volume * 100) / 80;
    }

    void ShowVolumeNotification(int previous_volume, int current_volume) {
        const int display_volume = MapVolumeForDisplay(current_volume);
        char notification[24];
        if (current_volume <= 0) {
            std::snprintf(notification, sizeof(notification), "Vol 0%%");
        } else if (current_volume > previous_volume) {
            std::snprintf(notification, sizeof(notification), "Vol +%d%%", display_volume);
        } else if (current_volume < previous_volume) {
            std::snprintf(notification, sizeof(notification), "Vol -%d%%", display_volume);
        } else {
            std::snprintf(notification, sizeof(notification), "Vol %d%%", display_volume);
        }
        GetDisplay()->ShowNotification(notification);
    }

    void StartContinuousVolumeAdjust(int direction) {
        if (direction == 0) {
            return;
        }
        volume_hold_direction_.store(direction);
        if (volume_hold_task_running_.exchange(true)) {
            return;
        }

        auto* self = this;
        xTaskCreatePinnedToCore(
            [](void* arg) {
                auto* board = static_cast<JiuchuanDevBoard*>(arg);
                while (true) {
                    int dir = board->volume_hold_direction_.load();
                    if (dir == 0) {
                        break;
                    }

                    auto& app = Application::GetInstance();
                    if (!app.IsSdMusicPlaybackMode() && !app.IsRadioPlaybackMode()) {
                        break;
                    }

                    auto codec = board->GetAudioCodec();
                    int previous_vol = codec->output_volume();
                    // 5% display step maps to 4 internal ticks over 0..80 range.
                    int current_vol = previous_vol + (dir > 0 ? 4 : -4);
                    if (current_vol < 0) current_vol = 0;
                    if (current_vol > 80) current_vol = 80;

                    if (current_vol != previous_vol) {
                        codec->SetOutputVolume(current_vol);
                        board->ShowVolumeNotification(previous_vol, current_vol);
                    }
                    vTaskDelay(pdMS_TO_TICKS(180));
                }
                board->volume_hold_direction_.store(0);
                board->volume_hold_task_running_.store(false);
                vTaskDelete(nullptr);
            },
            "vol_hold_task",
            3072,
            self,
            3,
            nullptr,
            tskNO_AFFINITY);
    }

    void StopContinuousVolumeAdjust() {
        volume_hold_direction_.store(0);
    }
    
    void InitializePowerManager() {
#ifdef CONFIG_JIUCHUAN_S3_V2
        power_manager_ = new PowerManager(VBUS_ADC_GPIO);
#else
        power_manager_ = new PowerManager(PWR_ADC_GPIO);
#endif
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
            GetDisplay()->UpdateStatusBar(true);
        });
    }

    void InitializePowerSaveTimer() {
        #ifndef __USER_GPIO_PWRDOWN__
        RTC_DATA_ATTR static bool long_press_occurred = false;
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_EXT0) {
            ESP_LOGI(TAG, "Wake up by EXT0");
            const int64_t start = esp_timer_get_time();
            ESP_LOGI(TAG, "esp_sleep_get_wakeup_cause");
            while (gpio_get_level(PWR_BUTTON_GPIO) == 0) {
                if (esp_timer_get_time() - start > 3000000) {
                    long_press_occurred = true;
                    break;
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            
            if (long_press_occurred) {
                ESP_LOGI(TAG, "Long press wakeup");
                long_press_occurred = false;
            } else {
                ESP_LOGI(TAG, "Short press, return to sleep");
                ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 0));
                ESP_ERROR_CHECK(rtc_gpio_pullup_en(PWR_BUTTON_GPIO));  // 内部上拉
                ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(PWR_BUTTON_GPIO));
                esp_deep_sleep_start();
            }
        }
        #endif
        //一分钟进入浅睡眠，5分钟进入深睡眠关机
        power_save_timer_ = new PowerSaveTimer(-1, (60*5), -1);
        // power_save_timer_ = new PowerSaveTimer(-1, 6, 10);//test
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
#ifdef CONFIG_JIUCHUAN_S3_V2
            power_manager_->Shutdown();
#else
            #ifndef __USER_GPIO_PWRDOWN__
            ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 0));
            ESP_ERROR_CHECK(rtc_gpio_pullup_en(PWR_BUTTON_GPIO));  // 内部上拉
            ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(PWR_BUTTON_GPIO));

            esp_lcd_panel_disp_on_off(panel, false); //关闭显示
            esp_deep_sleep_start();
            #else
            rtc_gpio_set_level(PWR_EN_GPIO, 0);
            rtc_gpio_hold_dis(PWR_EN_GPIO);
            #endif
#endif
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

    }

    void InitializeButtons() {
        static bool pwrbutton_unreleased = false;

        if (gpio_get_level(GPIO_NUM_3) == 1) {
            pwrbutton_unreleased = true;
        }
        // 配置GPIO
        ESP_LOGI(TAG, "Configuring power button GPIO");
#ifdef CONFIG_JIUCHUAN_S3_V2
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << GPIO_NUM_3),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));
#else
        GpioManager::Config(GPIO_NUM_3, GpioManager::GpioMode::INPUT_PULLDOWN);
#endif

        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Boot button clicked");
            power_save_timer_->WakeUp();
        });

        // 检查电源按钮初始状态
        ESP_LOGI(TAG, "Power button initial state: %d", GpioManager::GetLevel(PWR_BUTTON_GPIO));

        // 高电平有效长按关机逻辑
        pwr_button_.OnPressDown([this]() {
            pwrbutton_unreleased = false;
        });
        pwr_button_.OnLongPress([this]()
                                {
            auto& app = Application::GetInstance();
            app.Schedule([this]() {
                ESP_LOGI(TAG, "Power button long press detected (high-active)");

                if (pwrbutton_unreleased) {
                    ESP_LOGI(TAG, "开机后电源键未松开,取消关机");
                    return;
                }

                // 高电平有效防抖确认
                for (int i = 0; i < 5; i++) {
                    int level = GpioManager::GetLevel(PWR_BUTTON_GPIO);
                    ESP_LOGD(TAG, "Debounce check %d: GPIO%d level=%d", i + 1, PWR_BUTTON_GPIO, level);

                    if (level == 0) {
                        ESP_LOGW(TAG, "Power button inactive during confirmation - abort shutdown");
                        return;
                    }
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                }

                ESP_LOGI(TAG, "Confirmed power button pressed - initiating shutdown");
#ifdef CONFIG_JIUCHUAN_S3_V2
                // 关闭显示输出并让 LCD 控制器进入睡眠，彻底清除残影
                if (panel) {
                    esp_lcd_panel_disp_on_off(panel, false);
                    esp_lcd_panel_disp_sleep(panel, true);
                }
#endif
                power_manager_->SetPowerState(PowerState::SHUTDOWN);
            });
        });

        //单击切换状态
        pwr_button_.OnClick([this]()
                            {
            auto &app = Application::GetInstance();
            app.Schedule([this]() {
                auto &app = Application::GetInstance();
                auto current_state = app.GetDeviceState();

#if CONFIG_USE_ALARM
                if (app.HasAlarmEvent()) {
                    power_save_timer_->WakeUp();
                    auto* timer = app.GetGeneralTimer();
                    if (timer && timer->isRinging()) {
                        timer->ClearRinging();
                    } else {
                        app.ClearAlarmEvent();
                        app.DismissAlert();
                    }
                    GetDisplay()->ShowNotification("Alarm stopped");
                    return;
                }
#endif

                // Idle screen flow: power button opens/selects in main menu.
                if (current_state == kDeviceStateIdle) {
                    // Ensure display/backlight wakes before menu interactions.
                    power_save_timer_->WakeUp();

                    if (app.IsSdMusicPlaybackMode() && !app.IsMainMenuVisible()) {
                        bool back_ok = app.ExitSdMusicPlaybackToMainMenu();
                        GetDisplay()->ShowNotification(back_ok ? "Back to menu" : "Stop music failed");
                        return;
                    }

                    // While online/radio media is playing, power click should stop playback
                    // and return to main menu directly.
                    if (app.IsMediaPlaying() && !app.IsMainMenuVisible()) {
                        app.StopAllMedia();
                        app.Schedule([this]() {
                            auto& scheduled_app = Application::GetInstance();
                            if (!scheduled_app.IsMainMenuVisible()) {
                                scheduled_app.ToggleMainMenu();
                            }
                            GetDisplay()->ShowNotification("Back to menu");
                        });
                        return;
                    }

                    auto& qr = qrcode::QRCodeDisplay::GetInstance();
                    if (qr.IsDisplayed()) {
                        qr.Clear();
                        if (!app.IsMainMenuVisible()) {
                            app.ToggleMainMenu();
                        }
                        GetDisplay()->ShowNotification("Back to menu");
                        return;
                    }

                    if (app.IsMainMenuVisible()) {
                        const std::string selected_menu_label = app.GetSelectedMainMenuLabel();
                        bool ok = app.ActivateMainMenuSelection();
                        GetDisplay()->ShowNotification(ok ? selected_menu_label.c_str() : "Menu unavailable");
                    } else {
                        bool open = app.ToggleMainMenu();
                        GetDisplay()->ShowNotification(open ? "Main menu" : "Menu unavailable");
                    }
                    return;
                }

                ESP_LOGI(TAG, "当前设备状态: %d", current_state);
                
                if (current_state == kDeviceStateListening) {
                    ESP_LOGI(TAG, "从聆听状态切换到待命状态");
                    app.ToggleChatState();
                } else if (current_state == kDeviceStateSpeaking) {
                    ESP_LOGI(TAG, "从说话状态切换到待命状态");
                    app.ToggleChatState();
                } else {
                    ESP_LOGI(TAG, "唤醒设备");
                    power_save_timer_->WakeUp();
                }
            });
        });

        // 电源键三击：重置WiFi
        pwr_button_.OnMultipleClick([this]()
                                    {
            auto& app = Application::GetInstance();
            app.Schedule([this]() {
                ESP_LOGI(TAG, "Power button triple click: reset WiFi and reboot to AP config mode");
                power_save_timer_->WakeUp();
                ResetWifiConfiguration();
            });
        }, 3);

        // 电源键双击：屏幕救援，强制切回旧屏驱动并重启 (V1) / 切换 AEC 模式 (V2)
        pwr_button_.OnDoubleClick([this]()
                                  {
#ifdef CONFIG_JIUCHUAN_S3_V2
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
#if CONFIG_USE_DEVICE_AEC
                AecMode current_mode = app.GetAecMode();
                AecMode new_mode = (current_mode == kAecOff) ? kAecOnDeviceSide : kAecOff;
                app.SetAecMode(new_mode);
                ESP_LOGI(BOARD_TAG, "AEC mode: %s", new_mode == kAecOnDeviceSide ? "ON" : "OFF");
#endif
            }
#else
            auto& app = Application::GetInstance();
            app.Schedule([this]() {
                ESP_LOGW(TAG, "Power button double click: force old LCD driver and reboot");
                power_save_timer_->WakeUp();
                Settings display_settings("display", true);
                display_settings.SetInt("lcd_driver", kLcdDriverGc9301);
                GetDisplay()->ShowNotification("LCD rescue: old panel");
                vTaskDelay(pdMS_TO_TICKS(800));
                esp_restart();
            });
#endif
        });

        wifi_button.OnPressDown([this]()
                            {
            auto& app = Application::GetInstance();
            app.Schedule([this]() {
                ESP_LOGI(TAG, "Volume up button pressed (OnPressDown)");
                power_save_timer_->WakeUp();

                auto& app = Application::GetInstance();
                if (app.IsMainMenuVisible()) {
                    app.MoveMainMenu(-1);
                    return;
                }
                if (app.IsSdMusicPlaybackMode()) {
                    if (app.SdMusicPrevTrackFromMenu()) {
                        GetDisplay()->ShowNotification("Prev");
                    } else {
                        GetDisplay()->ShowNotification("Prev");
                    }
                    return;
                }
                if (app.IsRadioPlaybackMode()) {
                    if (app.RadioPrevStationFromMenu()) {
                        GetDisplay()->ShowNotification("Radio Prev");
                    } else {
                        GetDisplay()->ShowNotification("Radio skip failed");
                    }
                    return;
                }

                auto codec = GetAudioCodec();
                int previous_vol = codec->output_volume();
                int current_vol = (previous_vol + 8 > 80) ? 80 : previous_vol + 8;
                codec->SetOutputVolume(current_vol);

                ESP_LOGI(TAG, "Current volume: %d", current_vol);
                ShowVolumeNotification(previous_vol, current_vol);
            });
        });

        wifi_button.OnPressUp([this]() {
            StopContinuousVolumeAdjust();
        });

        // Long-press vol-up: next SD track if available, otherwise random radio station.
        wifi_button.OnLongPress([this]() {
            auto& app = Application::GetInstance();
            app.Schedule([this]() {
                power_save_timer_->WakeUp();
                auto& app = Application::GetInstance();
                if (app.IsSdMusicPlaybackMode() || app.IsRadioPlaybackMode()) {
                    StartContinuousVolumeAdjust(+1);
                    return;
                }

#ifdef CONFIG_SD_CARD_ENABLE
                auto* sd_music = app.GetSdMusic();
                if (sd_music) {
                    if (sd_music->GetTotalTracks() == 0) {
                        sd_music->LoadPlaylist();
                    }
                    if (sd_music->GetTotalTracks() > 0) {
                        bool ok = false;
                        auto st = sd_music->GetState();
                        if (st == Esp32SdMusic::PlayerState::Playing ||
                            st == Esp32SdMusic::PlayerState::Paused) {
                            ok = sd_music->Next();
                        } else {
                            ok = sd_music->Play();
                        }
                        if (ok) {
                            GetDisplay()->ShowNotification("Next");
                            return;
                        }
                    }
                }
#endif

                auto* radio = app.GetRadio();
                if (radio && radio->IsPlaying()) {
                    auto stations = radio->GetStationList();
                    if (!stations.empty()) {
                        auto idx = static_cast<size_t>(esp_random() % stations.size());
                        if (radio->PlayStation(stations[idx])) {
                            GetDisplay()->ShowNotification("Next station");
                            return;
                        }
                    }
                }

                GetDisplay()->ShowNotification("No media to skip");
            });
        });

        cmd_button.OnPressDown([this]()
                           {
            auto& app = Application::GetInstance();
            app.Schedule([this]() {
                ESP_LOGI(TAG, "Volume down button pressed (OnPressDown)");
                power_save_timer_->WakeUp();

                auto& app = Application::GetInstance();
                if (app.IsMainMenuVisible()) {
                    app.MoveMainMenu(1);
                    return;
                }
                if (app.IsSdMusicPlaybackMode()) {
                    if (app.SdMusicNextTrackFromMenu()) {
                        GetDisplay()->ShowNotification("Next");
                    } else {
                        GetDisplay()->ShowNotification("Next");
                    }
                    return;
                }
                if (app.IsRadioPlaybackMode()) {
                    if (app.RadioNextStationFromMenu()) {
                        GetDisplay()->ShowNotification("Radio Next");
                    } else {
                        GetDisplay()->ShowNotification("Radio skip failed");
                    }
                    return;
                }

                auto codec = GetAudioCodec();
                int previous_vol = codec->output_volume();
                int current_vol = (previous_vol - 8 < 0) ? 0 : previous_vol - 8;
                codec->SetOutputVolume(current_vol);

                ESP_LOGI(TAG, "Current volume: %d", current_vol);
                ShowVolumeNotification(previous_vol, current_vol);
            });
        });

        cmd_button.OnPressUp([this]() {
            StopContinuousVolumeAdjust();
        });

        // Long-press vol-down: previous SD track; if not active, cycle idle color theme.
        cmd_button.OnLongPress([this]() {
            auto& app = Application::GetInstance();
            app.Schedule([this]() {
                power_save_timer_->WakeUp();
                auto& app = Application::GetInstance();
                if (app.IsSdMusicPlaybackMode() || app.IsRadioPlaybackMode()) {
                    StartContinuousVolumeAdjust(-1);
                    return;
                }

#ifdef CONFIG_SD_CARD_ENABLE
                auto* sd_music = app.GetSdMusic();
                if (sd_music) {
                    auto st = sd_music->GetState();
                    if (st == Esp32SdMusic::PlayerState::Playing ||
                        st == Esp32SdMusic::PlayerState::Paused) {
                        if (sd_music->Prev()) {
                            GetDisplay()->ShowNotification("Prev");
                            return;
                        }
                    }
                }
#endif

                if (app.ToggleIdleTheme()) {
                    GetDisplay()->ShowNotification("Idle theme changed");
                } else {
                    GetDisplay()->ShowNotification("Idle theme unavailable");
                }
            });
        });
    }

    /**
     * Start a high-priority background task that polls CMD + WIFI buttons.
     * If both are held LOW for 5 continuous seconds, force esp_restart().
     * Uses direct GPIO reads (not the Button framework) so it works even
     * when the scheduler is heavily loaded or the main loop is frozen.
     */
    void StartEmergencyResetTask() {
        if (emergency_reset_task_handle_ != nullptr) {
            return;
        }
        xTaskCreatePinnedToCore(
            [](void* /*arg*/) {
                // Configure GPIOs as input with pull-up (idempotent if already done)
                gpio_config_t io_conf = {};
                io_conf.intr_type = GPIO_INTR_DISABLE;
                io_conf.mode = GPIO_MODE_INPUT;
                io_conf.pin_bit_mask = (1ULL << CMD_BUTTON_GPIO) | (1ULL << WIFI_BUTTON_GPIO);
                io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
                io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
                gpio_config(&io_conf);

                constexpr int kHoldMs = 5000;
                constexpr int kPollMs = 100;
                int pressed_ms = 0;

                while (true) {
                    const bool cmd_pressed  = (gpio_get_level(CMD_BUTTON_GPIO) == 0);
                    const bool wifi_pressed = (gpio_get_level(WIFI_BUTTON_GPIO) == 0);

                    if (cmd_pressed && wifi_pressed) {
                        pressed_ms += kPollMs;
                        if (pressed_ms >= kHoldMs) {
                            ESP_LOGW("EmergencyReset",
                                "CMD + WIFI held for %d ms -> Emergency restart!", pressed_ms);
                            vTaskDelay(pdMS_TO_TICKS(50));  // flush log
                            esp_restart();
                        }
                    } else {
                        pressed_ms = 0;
                    }
                    vTaskDelay(pdMS_TO_TICKS(kPollMs));
                }
            },
            "emg_reset",
            2048,
            nullptr,
            24,          // very high priority — runs even when other tasks are starved
            &emergency_reset_task_handle_,
            tskNO_AFFINITY);
        ESP_LOGI(BOARD_TAG, "Emergency reset task started (CMD+WIFI hold 5s)");
    }

    void ResetPanelHandles() {
        if (panel != nullptr) {
            esp_lcd_panel_del(panel);
            panel = nullptr;
        }
        if (panel_io != nullptr) {
            esp_lcd_panel_io_del(panel_io);
            panel_io = nullptr;
        }
    }

    bool InitializeGC9301Display() {
            ResetPanelHandles();
            // 液晶屏控制IO初始化
            ESP_LOGI(TAG, "test Install panel IO");
            spi_bus_config_t buscfg = {};
            buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
            buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
            buscfg.miso_io_num = GPIO_NUM_NC;
            buscfg.quadwp_io_num = GPIO_NUM_NC;
            buscfg.quadhd_io_num = GPIO_NUM_NC;
            buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
            esp_err_t err = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "GC9301: spi_bus_initialize failed: %s", esp_err_to_name(err));
                return false;
            }

            // 初始化SPI总线
            esp_lcd_panel_io_spi_config_t io_config = {};
            io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
            io_config.dc_gpio_num = DISPLAY_DC_PIN;
            io_config.spi_mode = 3;
            io_config.pclk_hz = 80 * 1000 * 1000;
            io_config.trans_queue_depth = 10;
            io_config.lcd_cmd_bits = 8;
            io_config.lcd_param_bits = 8;
            err = esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "GC9301: new_panel_io_spi failed: %s", esp_err_to_name(err));
                return false;
            }

            // 初始化液晶屏驱动芯片9309
            ESP_LOGI(TAG, "Install LCD driver");
            esp_lcd_panel_dev_config_t panel_config = {};
            panel_config.reset_gpio_num = GPIO_NUM_NC;
            panel_config.rgb_ele_order = LCD_RGB_ENDIAN_BGR;
            panel_config.bits_per_pixel = 16;
            err = esp_lcd_new_panel_gc9309na(panel_io, &panel_config, &panel);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "GC9301: new_panel_gc9309na failed: %s", esp_err_to_name(err));
                ResetPanelHandles();
                return false;
            }

            err = esp_lcd_panel_reset(panel);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "GC9301: panel_reset failed: %s", esp_err_to_name(err));
                ResetPanelHandles();
                return false;
            }
            err = esp_lcd_panel_init(panel);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "GC9301: panel_init failed: %s", esp_err_to_name(err));
                ResetPanelHandles();
                return false;
            }
            ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
            ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
            ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
            display_ = new CustomLcdDisplay(panel_io, panel,
                                            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
            return true;
    }

    bool InitializeJd9853Display() {
        ResetPanelHandles();
        ESP_LOGI(TAG, "Install panel IO");
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        esp_err_t err = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "JD9853: spi_bus_initialize failed: %s", esp_err_to_name(err));
            return false;
        }

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        err = esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JD9853: new_panel_io_spi failed: %s", esp_err_to_name(err));
            return false;
        }

        ESP_LOGI(TAG, "Install JD9853 LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        // Jiuchuan JD9853 on this branch renders correctly with BGR order.
        panel_config.rgb_ele_order = LCD_RGB_ENDIAN_BGR;
        panel_config.bits_per_pixel = 16;
        err = esp_lcd_new_panel_jd9853(panel_io, &panel_config, &panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JD9853: new_panel_jd9853 failed: %s", esp_err_to_name(err));
            ResetPanelHandles();
            return false;
        }

        err = esp_lcd_panel_reset(panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JD9853: panel_reset failed: %s", esp_err_to_name(err));
            ResetPanelHandles();
            return false;
        }
        err = esp_lcd_panel_init(panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JD9853: panel_init failed: %s", esp_err_to_name(err));
            ResetPanelHandles();
            return false;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        // Avoid mirrored text on some Jiuchuan JD9853 panel batches.
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                        false, false, DISPLAY_SWAP_XY);
        return true;
    }

public:
    JiuchuanDevBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        pwr_button_(PWR_BUTTON_GPIO,true),
        wifi_button(WIFI_BUTTON_GPIO),
        cmd_button(CMD_BUTTON_GPIO) {

        MaybeForceOldLcdDriverByBootKey();
        InitializeI2c();
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeButtons();
        StartEmergencyResetTask();
        const int lcd_driver = ResolveJiuchuanLcdDriverType();
        bool ok = false;
        if (lcd_driver == kLcdDriverJd9853) {
            ESP_LOGI(TAG, "Using LCD driver: JD9853");
            ok = InitializeJd9853Display();
            if (!ok) {
                ESP_LOGW(TAG, "JD9853 init failed, fallback to GC9301/GC9309NA");
                ok = InitializeGC9301Display();
                if (ok) {
                    Settings display_settings("display", true);
                    display_settings.SetInt("lcd_driver", kLcdDriverGc9301);
                    ESP_LOGW(TAG, "Fallback applied and saved lcd_driver=%d", kLcdDriverGc9301);
                }
            }
        } else {
            ESP_LOGI(TAG, "Using LCD driver: GC9301/GC9309NA");
            ok = InitializeGC9301Display();
            if (!ok) {
                ESP_LOGW(TAG, "GC9301 init failed, fallback to JD9853");
                ok = InitializeJd9853Display();
                if (ok) {
                    Settings display_settings("display", true);
                    display_settings.SetInt("lcd_driver", kLcdDriverJd9853);
                    ESP_LOGW(TAG, "Fallback applied and saved lcd_driver=%d", kLcdDriverJd9853);
                }
            }
        }
        if (!ok) {
            ESP_LOGE(TAG, "Both LCD drivers failed to initialize");
            abort();
        }
        GetBacklight()->RestoreBrightness();

    }

    virtual bool PromptOfflineMode() override {
        auto display = GetDisplay();
        if (display) {
            display->SetStatus("Booting...");
            display->SetChatMessage("system", "Nhấn Vol+ : Offline Mode\nNhấn Vol- : Kết nối Wifi");
        }

        // Wait up to 4 seconds for a single press of Vol+ or Vol-
        int triggered_mode = 0; // 0 = none, 1 = offline (Vol+), 2 = wifi config (Vol-)
        for (int i = 0; i < 40; ++i) {
            if (GpioManager::GetLevel(WIFI_BUTTON_GPIO) == 0) { // Vol+
                triggered_mode = 1;
                break;
            }
            if (GpioManager::GetLevel(CMD_BUTTON_GPIO) == 0) { // Vol-
                triggered_mode = 2;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (triggered_mode == 0) {
            if (display) {
                display->SetChatMessage("system", "");
            }
            return false;
        }

        if (triggered_mode == 2) {
            // User pressed Vol- to configure WiFi
            while(GpioManager::GetLevel(CMD_BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            ResetWifiConfiguration();
            return false;
        }

        // Triggered_mode == 1: User pressed Vol+ to enter Offline Mode. Ask for confirmation
        if (display) {
            display->SetStatus("No-Wifi?");
            display->SetChatMessage("system", "Sử dụng chế độ Offline?\n\nVol+: CÓ    Vol-: KHÔNG");
        }

        // Wait for user to release the button first (debounce)
        while(GpioManager::GetLevel(WIFI_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Wait for confirmation
        while (true) {
            if (GpioManager::GetLevel(WIFI_BUTTON_GPIO) == 0) {
                // wait release
                while(GpioManager::GetLevel(WIFI_BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(10));
                if (display) {
                    display->SetChatMessage("system", "");
                }
                return true;
            }
            if (GpioManager::GetLevel(CMD_BUTTON_GPIO) == 0) {
                // wait release
                while(GpioManager::GetLevel(CMD_BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(10));
                if (display) {
                    display->SetChatMessage("system", "");
                }
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef CONFIG_JIUCHUAN_S3_V2
        static JiuchuanAudioCodec audio_codec(
            codec_i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
#else
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_, 
            I2C_NUM_0, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR);
#endif
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
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual bool RequestShutdown() override {
        if (power_manager_ == nullptr) {
            return false;
        }
        power_manager_->SetPowerState(PowerState::SHUTDOWN);
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }

#ifdef CONFIG_SD_CARD_MMC_INTERFACE
    virtual SdCard* GetSdCard() override {
#ifdef CARD_SDMMC_BUS_WIDTH_4BIT
        static SdMMC sdmmc(CARD_SDMMC_CLK_GPIO,
                           CARD_SDMMC_CMD_GPIO,
                           CARD_SDMMC_D0_GPIO,
                           CARD_SDMMC_D1_GPIO,
                           CARD_SDMMC_D2_GPIO,
                           CARD_SDMMC_D3_GPIO);
#else
        static SdMMC sdmmc(CARD_SDMMC_CLK_GPIO,
                           CARD_SDMMC_CMD_GPIO,
                           CARD_SDMMC_D0_GPIO,
                           1,
                           "/sdcard",
                           false,
                           5,
                           16 * 1024,
                           SDMMC_FREQ_DEFAULT);
#endif
        return &sdmmc;
    }
#endif
#ifdef CONFIG_SD_CARD_SPI_INTERFACE
    virtual SdCard* GetSdCard() override {
        static SdSPI sdspi(CARD_SPI_MISO_GPIO,
                           CARD_SPI_MOSI_GPIO,
                           CARD_SPI_SCLK_GPIO,
                           CARD_SPI_CS_GPIO);
        return &sdspi;
    }
#endif
};

DECLARE_BOARD(JiuchuanDevBoard);
