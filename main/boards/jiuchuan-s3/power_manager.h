#pragma once

#ifdef CONFIG_JIUCHUAN_S3_V2

/**
 * @file power_manager.h
 * @brief 九川开发板 V2 电源管理模块
 */

#include <vector>
#include <functional>

#include <esp_timer.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "adc_battery_estimation.h"
#include "power_controller.h"
#include "config.h"

// ============================================================================
// 硬件配置常量
// ============================================================================

// ADC 基础配置
#define JIUCHUAN_ADC_UNIT           (ADC_UNIT_1)
#define JIUCHUAN_ADC_BITWIDTH       (ADC_BITWIDTH_12)      // 12位精度 (0-4095)
#define JIUCHUAN_ADC_ATTEN          (ADC_ATTEN_DB_12)      // 12dB衰减 (0-3.3V)

// 电池电压检测 (VBAT) - GPIO4 -> ADC1_CH3
#define JIUCHUAN_BATTERY_ADC_CHANNEL        (ADC_CHANNEL_3)     // GPIO4
#define JIUCHUAN_BATTERY_RESISTOR_UPPER     (200000)            // 上臂电阻 200kΩ
#define JIUCHUAN_BATTERY_RESISTOR_LOWER     (100000)            // 下臂电阻 100kΩ
#define JIUCHUAN_BATTERY_VOLTAGE_CALIBRATION (1.0f)             // 校准系数

// USB 电压检测 (VBUS) - GPIO5 -> ADC1_CH4
#define JIUCHUAN_VBUS_ADC_CHANNEL           (ADC_CHANNEL_4)     // GPIO5
#define JIUCHUAN_VBUS_RESISTOR_UPPER        (200000)            // 上臂电阻 200kΩ
#define JIUCHUAN_VBUS_RESISTOR_LOWER        (100000)            // 下臂电阻 100kΩ
#define JIUCHUAN_VBUS_CHARGING_THRESHOLD_MV (1000)              // 充电检测阈值 1.0V

// 运行参数配置
#define JIUCHUAN_ADC_SAMPLE_COUNT           (5)                 // ADC采样次数
#define JIUCHUAN_ADC_SAMPLE_INTERVAL_MS     (10)                // 采样间隔 10ms
#define JIUCHUAN_BATTERY_CHECK_INTERVAL_MS  (2000)              // 定时器周期 2秒
#define JIUCHUAN_BATTERY_READ_INTERVAL      (3)                 // 每6秒读取电池 (2*3=6s)
#define JIUCHUAN_LOW_BATTERY_LEVEL          (20)                // 低电量阈值 20%

#undef TAG
#define TAG "PowerManager"

class PowerManager {
private:
    // 硬件句柄
    esp_timer_handle_t timer_handle_;                           // 定时器句柄
    adc_oneshot_unit_handle_t adc_handle_;                      // 共享 ADC 句柄
    adc_cali_handle_t adc_cali_handle_;                         // ADC 校准句柄
    adc_battery_estimation_handle_t adc_battery_estimation_handle_; // 电量估算句柄
    
    // 状态变量
    gpio_num_t charging_pin_;                                   // 充电检测引脚
    gpio_num_t battery_full_pin_;                               // 电池充满检测引脚(DONE)
    int32_t battery_level_;                                     // 电池电量 (0-100%)
    bool is_charging_;                                          // 充电状态
    bool is_battery_full_;                                      // 电池充满状态
    volatile bool battery_full_event_;                          // 中断事件标志
    bool is_low_battery_;                                       // 低电量标志
    bool is_empty_battery_;                                     // 电量耗尽标志
    int ticks_;                                                 // 定时器计数
    
    // 回调函数
    std::function<void(bool)> on_charging_status_changed_;      // 充电状态变化回调
    std::function<void(bool)> on_low_battery_status_changed_;   // 低电量状态变化回调
    
    // 配置常量
    const int kBatteryReadInterval = JIUCHUAN_BATTERY_READ_INTERVAL;
    const int kLowBatteryLevel = JIUCHUAN_LOW_BATTERY_LEVEL;

    static void IRAM_ATTR DonePinIsrHandler(void* arg) {
        PowerManager* self = static_cast<PowerManager*>(arg);
        self->battery_full_event_ = true;
    }

    void UpdateBatteryFullStatus() {
        if (battery_full_pin_ != GPIO_NUM_NC) {
            int done_level = gpio_get_level(battery_full_pin_);
            bool new_battery_full = (done_level == 0);  // 低电平表示充满

            if (new_battery_full != is_battery_full_) {
                is_battery_full_ = new_battery_full;
                ESP_LOGI(TAG, "电池充满状态变化: %s (DONE引脚=%d)",
                        is_battery_full_ ? "已充满" : "未充满", done_level);
            }
        }
    }

    void CheckBatteryStatus() {
        if (battery_full_event_) {
            battery_full_event_ = false;  // 清除标志
            UpdateBatteryFullStatus();
            ReadBatteryData();  // 立即读取电池 data
            ESP_LOGI(TAG, "检测到充满中断事件，立即更新状态");
        }

        UpdateBatteryFullStatus();

        bool new_charging_status = false;
        esp_err_t ret = adc_battery_estimation_get_charging_state(
            adc_battery_estimation_handle_, &new_charging_status);

        if (new_charging_status && is_battery_full_) {
            new_charging_status = false;
        }

        if (ret == ESP_OK && new_charging_status != is_charging_) {
            is_charging_ = new_charging_status;
            ESP_LOGI(TAG, "充电状态变化: %s", is_charging_ ? "充电中" : "未充电");

            if (on_charging_status_changed_) {
                on_charging_status_changed_(is_charging_);
            }

            ReadBatteryData();  // 充电状态变化时立即读取
            return;
        }

        ticks_++;
        if (ticks_ % kBatteryReadInterval == 0) {
            ReadBatteryData();
        }
    }
    
    void ReadBatteryData() {
        int total_voltage_mv = 0;
        int sample_count = JIUCHUAN_ADC_SAMPLE_COUNT;
        
        for (int i = 0; i < sample_count; i++) {
            int adc_raw = 0;
            int voltage_mv = 0;
            
            esp_err_t ret = adc_oneshot_read(adc_handle_, JIUCHUAN_BATTERY_ADC_CHANNEL, &adc_raw);
            if (ret == ESP_OK) {
                if (adc_cali_handle_) {
                    adc_cali_raw_to_voltage(adc_cali_handle_, adc_raw, &voltage_mv);
                } else {
                    voltage_mv = (adc_raw * 3100) / 4095;
                }
                total_voltage_mv += voltage_mv;
            }
            
            vTaskDelay(pdMS_TO_TICKS(JIUCHUAN_ADC_SAMPLE_INTERVAL_MS));
        }
        
        int avg_voltage_mv = total_voltage_mv / sample_count;
        float actual_battery_voltage = (avg_voltage_mv * 3 * JIUCHUAN_BATTERY_VOLTAGE_CALIBRATION) / 1000.0f;
        
        float battery_capacity = 0;
        adc_battery_estimation_get_capacity(adc_battery_estimation_handle_, &battery_capacity);
        
        if (battery_capacity > -10 && battery_capacity <= 0) {
            battery_level_ = 0;
        } else {
            battery_level_ = static_cast<int32_t>(battery_capacity);
        }
        
        bool charging = false;
        adc_battery_estimation_get_charging_state(adc_battery_estimation_handle_, &charging);

        if (charging && is_battery_full_) {
            charging = false;
        }

        ESP_LOGI(TAG, "电池: %.2fV, %.1f%% %s%s",
                 actual_battery_voltage,
                 battery_capacity,
                 charging ? "[充电中]" : "",
                 is_battery_full_ ? " [已充满]" : "");
    }
    
    static bool ChargingDetectCallback(void* user_data) {
        PowerManager* self = static_cast<PowerManager*>(user_data);
        
        int adc_raw = 0;
        esp_err_t ret = adc_oneshot_read(self->adc_handle_, JIUCHUAN_VBUS_ADC_CHANNEL, &adc_raw);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "读取 VBUS(GPIO5) ADC 失败: %s", esp_err_to_name(ret));
            return false;
        }
        
        int voltage_mv = 0;
        if (self->adc_cali_handle_) {
            adc_cali_raw_to_voltage(self->adc_cali_handle_, adc_raw, &voltage_mv);
        } else {
            voltage_mv = (adc_raw * 3100) / 4095;
        }
        
        bool is_charging = (voltage_mv > JIUCHUAN_VBUS_CHARGING_THRESHOLD_MV);
        int actual_vbus_mv = voltage_mv * 3;
        ESP_LOGD(TAG, "VBUS: %.2fV, %s", actual_vbus_mv / 1000.0f, 
                 is_charging ? "充电" : "未充电");
        
        return is_charging;
    }
    
    void InitializeADC() {
        adc_oneshot_unit_init_cfg_t adc_cfg = {
            .unit_id = JIUCHUAN_ADC_UNIT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, &adc_handle_));
        
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = JIUCHUAN_ADC_ATTEN,
            .bitwidth = JIUCHUAN_ADC_BITWIDTH,
        };
        
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, JIUCHUAN_BATTERY_ADC_CHANNEL, &chan_cfg));
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, JIUCHUAN_VBUS_ADC_CHANNEL, &chan_cfg));
        
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = JIUCHUAN_ADC_UNIT,
            .atten = JIUCHUAN_ADC_ATTEN,
            .bitwidth = JIUCHUAN_ADC_BITWIDTH,
        };
        
        esp_err_t cali_ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle_);
        if (cali_ret == ESP_OK) {
            ESP_LOGI(TAG, "ADC 校准成功");
        } else {
            ESP_LOGW(TAG, "ADC 校准失败，使用原始值: %s", esp_err_to_name(cali_ret));
            adc_cali_handle_ = nullptr;
        }
        
        ESP_LOGI(TAG, "共享 ADC 初始化完成: GPIO4(电池)=ADC1_CH3, GPIO5(VBUS)=ADC1_CH4");
    }
    
    void InitializeBatteryEstimation() {
        static const battery_point_t battery_curve_table[] = {
            {4.2,  100},    // 4.2V = 100%
            {4.06, 80},     // 4.06V = 80%
            {3.82, 60},     // 3.82V = 60%
            {3.58, 40},     // 3.58V = 40%
            {3.34, 20},     // 3.34V = 20%
            {3.1,  0},      // 3.1V = 0%
            {3.0,  -10}     // 3.0V = -10%
        };
        
        adc_battery_estimation_t config = {
            .external = {
                .adc_handle = adc_handle_,              // 使用共享 ADC
                .adc_cali_handle = adc_cali_handle_,    // 使用共享校准
            },
            .adc_channel = JIUCHUAN_BATTERY_ADC_CHANNEL,
            .upper_resistor = JIUCHUAN_BATTERY_RESISTOR_UPPER,
            .lower_resistor = JIUCHUAN_BATTERY_RESISTOR_LOWER,
            .battery_points = battery_curve_table,
            .battery_points_count = sizeof(battery_curve_table) / sizeof(battery_curve_table[0]),
            .charging_detect_cb = ChargingDetectCallback,   // 充电检测回调
            .charging_detect_user_data = this               // 传递 this 指针
        };
        
        adc_battery_estimation_handle_ = adc_battery_estimation_create(&config);
        if (!adc_battery_estimation_handle_) {
            ESP_LOGE(TAG, "创建 adc_battery_estimation 失败");
        } else {
            ESP_LOGI(TAG, "电池电量估算初始化完成（含 VBUS 充电检测）");
        }
    }
    
    void InitializePowerControl() {
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
        ESP_LOGD(TAG, "Wakeup cause: %d %s", wakeup_reason,
                 wakeup_reason == ESP_SLEEP_WAKEUP_EXT0 ? "(ext0)" : "");

        rtc_gpio_init(PWR_EN_GPIO);
        rtc_gpio_set_direction(PWR_EN_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_hold_dis(PWR_EN_GPIO);  // 释放可能存在的 hold
        rtc_gpio_set_level(PWR_EN_GPIO, 1);

        if (battery_full_pin_ != GPIO_NUM_NC) {
            gpio_config_t io_conf = {};
            io_conf.pin_bit_mask = (1ULL << battery_full_pin_);
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_conf.intr_type = GPIO_INTR_NEGEDGE;      // 仅下降沿触发中断
            ESP_ERROR_CHECK(gpio_config(&io_conf));

            esp_err_t isr_ret = gpio_install_isr_service(0);
            if (isr_ret == ESP_ERR_INVALID_STATE) {
                ESP_LOGD(TAG, "GPIO ISR服务已安装，跳过");
            } else if (isr_ret != ESP_OK) {
                ESP_LOGE(TAG, "安装GPIO ISR服务失败: %s", esp_err_to_name(isr_ret));
                return;
            }

            ESP_ERROR_CHECK(gpio_isr_handler_add(battery_full_pin_, DonePinIsrHandler, this));

            int initial_level = gpio_get_level(battery_full_pin_);
            is_battery_full_ = (initial_level == 0);

            ESP_LOGI(TAG, "电池充满检测引脚 GPIO%d 初始化完成 (当前状态: %s, 下降沿中断)",
                     battery_full_pin_,
                     is_battery_full_ ? "已充满" : "未充满");
        }

        if (rtc_gpio_is_valid_gpio(GPIO_NUM_3)) {
            rtc_gpio_deinit(GPIO_NUM_3);
        }

        ESP_LOGI(TAG, "电源控制初始化完成");
    }
    
    void HandleShutdown() {
        ESP_ERROR_CHECK(rtc_gpio_init(PWR_BUTTON_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_set_direction(PWR_BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(PWR_BUTTON_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_pullup_dis(PWR_BUTTON_GPIO));

        vTaskDelay(pdMS_TO_TICKS(100));
        int wait_count = 0;
        while (rtc_gpio_get_level(PWR_BUTTON_GPIO) == 1 && wait_count < 50) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count++;
        }

        if (rtc_gpio_get_level(PWR_BUTTON_GPIO) == 0) {
            ESP_LOGI(TAG, "电源键已释放，开始关机流程");
        } else {
            ESP_LOGW(TAG, "等待电源键释放超时，继续执行关机流程");
        }

        vTaskDelay(pdMS_TO_TICKS(200));

        ESP_LOGI(TAG, "拉低 PWR_EN 断电关机");
        ESP_ERROR_CHECK(rtc_gpio_init(PWR_EN_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_set_direction(PWR_EN_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY));
        rtc_gpio_hold_dis(PWR_EN_GPIO);
        rtc_gpio_set_level(PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(PWR_EN_GPIO);

        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGW(TAG, "仍未掉电，进入深度睡眠作为兜底");
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 1));
        esp_deep_sleep_start();
    }

public:
    PowerManager(gpio_num_t pin)
        : timer_handle_(nullptr)
        , adc_handle_(nullptr)
        , adc_cali_handle_(nullptr)
        , adc_battery_estimation_handle_(nullptr)
        , charging_pin_(pin)
        , battery_full_pin_(BATTERY_FULL_PIN)
        , battery_level_(100)
        , is_charging_(false)
        , is_battery_full_(false)
        , battery_full_event_(false)
        , is_low_battery_(false)
        , is_empty_battery_(false)
        , ticks_(0) {
        
        InitializePowerControl();
        InitializeADC();
        InitializeBatteryEstimation();
        
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                PowerManager* self = static_cast<PowerManager*>(arg);
                self->CheckBatteryStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, JIUCHUAN_BATTERY_CHECK_INTERVAL_MS * 1000));
        
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "PowerManager 初始化完成");
        ReadBatteryData();
    }
    
    ~PowerManager() {
        if (battery_full_pin_ != GPIO_NUM_NC) {
            gpio_isr_handler_remove(battery_full_pin_);
        }

        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }

        if (adc_battery_estimation_handle_) {
            adc_battery_estimation_destroy(adc_battery_estimation_handle_);
        }

        if (adc_cali_handle_) {
            adc_cali_delete_scheme_curve_fitting(adc_cali_handle_);
        }

        if (adc_handle_) {
            adc_oneshot_del_unit(adc_handle_);
        }
    }
    
    bool IsCharging() {
        if (is_battery_full_) {
            return false;
        }
        return is_charging_;
    }
    
    bool IsUsbConnected() {
        bool usb_connected = false;
        adc_battery_estimation_get_charging_state(
            adc_battery_estimation_handle_, &usb_connected);
        return usb_connected;
    }
    
    bool IsDischarging() {
        return !is_charging_;
    }
    
    int32_t GetBatteryLevel() {
        return battery_level_;
    }
    
    void Shutdown() {
        HandleShutdown();
    }

    void SetPowerState(PowerState newState) {
        if (newState == PowerState::SHUTDOWN) {
            Shutdown();
        }
    }
    
    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }
    
    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = callback;
    }
};

#else

// === Jiuchuan-S3 V1 PowerManager ===
#include <vector>
#include <functional>

#include <esp_timer.h>
#include <driver/gpio.h>
#include "adc_battery_estimation.h"
#include "power_controller.h"
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#define JIUCHUAN_ADC_UNIT (ADC_UNIT_1)
#define JIUCHUAN_ADC_BITWIDTH (ADC_BITWIDTH_12)
#define JIUCHUAN_ADC_ATTEN (ADC_ATTEN_DB_12)
#define JIUCHUAN_ADC_CHANNEL (ADC_CHANNEL_3)
#define JIUCHUAN_RESISTOR_UPPER (200000)
#define JIUCHUAN_RESISTOR_LOWER (100000)

#undef TAG
#define TAG "PowerManager"
class PowerManager {
private:
    esp_timer_handle_t timer_handle_;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;
    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::vector<uint16_t> adc_values_;
    int32_t battery_level_ = 100;
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    bool is_empty_battery_ = false;
    int ticks_ = 0;
    const int kBatteryAdcInterval = 60;
    const int kBatteryAdcDataCount = 3;
    const int kLowBatteryLevel = 20;

    adc_battery_estimation_handle_t adc_battery_estimation_handle;
    PowerController* power_controller_;

    void CheckBatteryStatus() {
        bool new_charging_status = gpio_get_level(charging_pin_) == 1;
        if (new_charging_status != is_charging_) {
            is_charging_ = new_charging_status;
            if (on_charging_status_changed_) {
                on_charging_status_changed_(is_charging_);
            }
            ReadBatteryAdcData();
            return;
        }

        if (adc_values_.size() < kBatteryAdcDataCount) {
            ReadBatteryAdcData();
            return;
        }

        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0) {
            ReadBatteryAdcData();
        }
    }

    void ReadBatteryAdcData() {
        float battery_capacity_temp = 0;
        adc_battery_estimation_get_capacity(adc_battery_estimation_handle, &battery_capacity_temp);
        ESP_LOGI("PowerManager", "Battery level: %.1f%%", battery_capacity_temp);
        if(battery_capacity_temp > -10 && battery_capacity_temp <= 0){
            battery_level_ = 0;
        }else{
            battery_level_ = battery_capacity_temp;
        }
    }

public:
    PowerManager(gpio_num_t pin) : charging_pin_(pin) {
        power_controller_ = &PowerController::Instance();
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; 
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;     
        gpio_config(&io_conf);

        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                PowerManager* self = static_cast<PowerManager*>(arg);
                self->CheckBatteryStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));

        static const battery_point_t battery_ponint_table[]={
            { 4.2 ,  100},
            { 4.06 ,  80},
            { 3.82 ,  60},
            { 3.58 ,  40},
            { 3.34 ,  20},
            { 3.1 ,  0},
            { 3.0 ,  -10}
        };

        adc_battery_estimation_t config = {
            .internal = {
                .adc_unit = JIUCHUAN_ADC_UNIT,
                .adc_bitwidth = JIUCHUAN_ADC_BITWIDTH,
                .adc_atten = JIUCHUAN_ADC_ATTEN,
            },
            .adc_channel = JIUCHUAN_ADC_CHANNEL,
            .upper_resistor = JIUCHUAN_RESISTOR_UPPER,
            .lower_resistor = JIUCHUAN_RESISTOR_LOWER,
            .battery_points = battery_ponint_table,
            .battery_points_count = sizeof(battery_ponint_table) / sizeof(battery_ponint_table[0])
        };

        adc_battery_estimation_handle = adc_battery_estimation_create(&config);
        
        RegisterAllCallbacks();
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_battery_estimation_handle) {
            adc_battery_estimation_destroy(adc_battery_estimation_handle);
        }
    }

    bool IsCharging() {
        return false;
    }

    bool IsDischarging() {
        return true;
    }

    int32_t GetBatteryLevel() {
        return battery_level_;
    }

    void RegisterAllCallbacks() {
        power_controller_->OnStateChange([this](PowerState newState) {
            switch(newState) {
                case PowerState::SHUTDOWN: {
                    ESP_LOGD(TAG, "关机");
                    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 0));
                    ESP_ERROR_CHECK(rtc_gpio_pulldown_en(PWR_BUTTON_GPIO)); // 内部下拉
                    ESP_ERROR_CHECK(rtc_gpio_pullup_dis(PWR_BUTTON_GPIO));
                    rtc_gpio_set_level(PWR_EN_GPIO, 0);
                    rtc_gpio_hold_dis(PWR_EN_GPIO);
                    vTaskDelay(200 / portTICK_PERIOD_MS);
                    ESP_LOGI(TAG, "Initiating deep sleep");
                    esp_deep_sleep_start();
                    break;
                }   
                default:
                    ESP_LOGD(TAG, "State changed to %d", static_cast<int>(newState));
                    break;
            }
        });  
    }
    void SetPowerState(PowerState newState) {
        power_controller_->SetState(newState);
    }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = callback;
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }
};

#endif