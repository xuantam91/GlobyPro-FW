/*
 * @Descripttion: 闹钟
 * @Author: Xvsenfeng helloworldjiao@163.com
 * @LastEditors: Please set LastEditors
 * Copyright (c) 2025 by helloworldjiao@163.com, All Rights Reserved. 
 */
#include "general_timer.h"
#include "esp_log.h"
#include <ctime>
#include <string>
#include <algorithm>
#include <stdio.h>
#include <cstring>
#include "stdlib.h"
#include "application.h"
#include "settings.h"
#include "display.h"
#include "assets/lang_config.h"

#ifndef CONFIG_ALARM_SD_CARD_MOUNT_POINT
#define CONFIG_ALARM_SD_CARD_MOUNT_POINT "/sdcard"
#endif


#define TAG "XvSenfengTimer"

static bool ringing_ = false;
std::string alarm_message_ = "";

static std::string event_message_from_event(const e_pet_timer_event_t& event) {
    if (event.type == E_PET_TIMER_MESSAGE) {
        if (event.message != nullptr) {
            return std::string(event.message);
        }
    } else {
        if (event.function.arg != nullptr) {
            return std::string((char*)event.function.arg);
        }
    }
    return "Alarm";
}

void alarm_deal_function(void* arg){
    if (ringing_) {
        // Already ringing; ignore duplicated alarm callbacks until user clears it.
        return;
    }
    auto& app = Application::GetInstance();
    DeviceState ds = app.GetDeviceState();
    if (ds == kDeviceStateStarting || ds == kDeviceStateConnecting || ds == kDeviceStateActivating) {
        // Skip ringing during boot/login flow. This alarm occurrence is considered missed.
        ESP_LOGI(TAG, "Skip alarm during login flow");
        return;
    }
    ESP_LOGI(TAG, "=----ringing----=");
    app.SetAlarmEvent(); // 设置闹钟事件
    ringing_ = true;
    const char* message_arg = static_cast<const char*>(arg);
    alarm_message_ = (message_arg != nullptr) ? message_arg : "Alarm";
    std::string alarm_text = alarm_message_.empty() ? "Alarm" : alarm_message_;
    app.Schedule([alarm_text]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("system", alarm_text.c_str());
    });
    ESP_LOGI(TAG, "Alarm callback message: %s", alarm_text.c_str());

}

// 回调函数的列表
callback_t callback_catalogue[20] = {
    {alarm_deal_function},
};

std::string GeneralTimer::GetAlarmMessage(){
    return alarm_message_;
}

bool GeneralTimer::GetNextAlarm(time_t& trigger_time, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (e_pet_timer_events.empty()) {
        return false;
    }

    const time_t now = time(nullptr);
    const e_pet_timer_event_t* next = nullptr;
    for (const auto& event : e_pet_timer_events) {
        if (event.trigger_time >= now) {
            next = &event;
            break;
        }
    }
    if (next == nullptr) {
        next = &e_pet_timer_events.front();
    }

    trigger_time = next->trigger_time;
    if (next->type == E_PET_TIMER_MESSAGE) {
        message = (next->message != nullptr) ? std::string(next->message) : "Alarm";
    } else {
        message = (next->function.arg != nullptr) ? std::string((char*)next->function.arg) : "Alarm";
    }
    if (message.empty()) {
        message = "Alarm";
    }
    return true;
}

bool GeneralTimer::GetUpcomingAlarmsToday(std::vector<std::pair<time_t, std::string>>& alarms, size_t max_count) {
    alarms.clear();
    if (max_count == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (e_pet_timer_events.empty()) {
        return false;
    }

    const time_t now = time(nullptr);
    struct tm now_tm;
    localtime_r(&now, &now_tm);

    for (const auto& event : e_pet_timer_events) {
        if (event.trigger_time <= now) {
            continue;
        }

        struct tm event_tm;
        localtime_r(&event.trigger_time, &event_tm);
        if (event_tm.tm_year != now_tm.tm_year || event_tm.tm_yday != now_tm.tm_yday) {
            continue;
        }

        std::string message;
        if (event.type == E_PET_TIMER_MESSAGE) {
            message = (event.message != nullptr) ? std::string(event.message) : "Alarm";
        } else {
            message = (event.function.arg != nullptr) ? std::string((char*)event.function.arg) : "Alarm";
        }
        if (message.empty()) {
            message = "Alarm";
        }
        alarms.emplace_back(event.trigger_time, std::move(message));
    }

    std::sort(alarms.begin(), alarms.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    if (alarms.size() > max_count) {
        alarms.resize(max_count);
    }
    return !alarms.empty();
}

bool GeneralTimer::ListUpcomingAlarms(std::vector<alarm_snapshot_t>& alarms, size_t max_count, bool today_only) {
    alarms.clear();
    if (max_count == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (e_pet_timer_events.empty()) {
        return false;
    }

    const time_t now = time(nullptr);
    struct tm now_tm;
    localtime_r(&now, &now_tm);

    for (size_t i = 0; i < e_pet_timer_events.size(); ++i) {
        const auto& event = e_pet_timer_events[i];
        if (event.trigger_time <= now) {
            continue;
        }
        if (today_only) {
            struct tm event_tm;
            localtime_r(&event.trigger_time, &event_tm);
            if (event_tm.tm_year != now_tm.tm_year || event_tm.tm_yday != now_tm.tm_yday) {
                continue;
            }
        }

        alarm_snapshot_t snapshot;
        snapshot.index = static_cast<int>(i);
        snapshot.trigger_time = event.trigger_time;
        snapshot.message = event_message_from_event(event);
        snapshot.repeat_seconds = event.repeat_time;
        if (snapshot.message.empty()) {
            snapshot.message = "Alarm";
        }
        alarms.push_back(std::move(snapshot));
    }

    std::sort(alarms.begin(), alarms.end(), [](const alarm_snapshot_t& a, const alarm_snapshot_t& b) {
        return a.trigger_time < b.trigger_time;
    });
    if (alarms.size() > max_count) {
        alarms.resize(max_count);
    }
    return !alarms.empty();
}

bool GeneralTimer::DeleteAlarmByIndex(int index, time_t* trigger_time, std::string* message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || static_cast<size_t>(index) >= e_pet_timer_events.size()) {
        return false;
    }

    auto it = e_pet_timer_events.begin() + index;
    if (trigger_time != nullptr) {
        *trigger_time = it->trigger_time;
    }
    if (message != nullptr) {
        *message = event_message_from_event(*it);
    }

    if (it->type == E_PET_TIMER_FUNCTION) {
        free(it->function.arg);
        it->function.arg = nullptr;
    } else {
        free(it->message);
        it->message = nullptr;
    }

    e_pet_timer_events.erase(it);
    TimerEventSort();
#if CONFIG_ALARM_USE_SD_CARD
    TimerUpdateCsvTimer();
#endif
    return true;
}

bool GeneralTimer::UpdateAlarmByIndex(int index, time_t trigger_time, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || static_cast<size_t>(index) >= e_pet_timer_events.size()) {
        return false;
    }

    const std::string new_message = message.empty() ? "Alarm" : message;
    char* msg_buf = static_cast<char*>(malloc(new_message.size() + 1));
    if (msg_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for updated alarm message");
        return false;
    }
    strcpy(msg_buf, new_message.c_str());

    auto& event = e_pet_timer_events[index];
    if (event.type == E_PET_TIMER_FUNCTION) {
        free(event.function.arg);
        event.function.arg = msg_buf;
    } else {
        free(event.message);
        event.message = msg_buf;
    }
    event.trigger_time = trigger_time;

    TimerEventSort();
#if CONFIG_ALARM_USE_SD_CARD
    TimerUpdateCsvTimer();
#endif
    return true;
}

void GeneralTimer::ClearRinging(){
    ESP_LOGI(TAG, "clear");
    ringing_ = false;
    auto &app = Application::GetInstance();
    app.ClearAlarmEvent(); // 清除闹钟事件
    app.Schedule([]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->SetChatMessage("system", "");
        }
        Application::GetInstance().DismissAlert();
    });
}

bool GeneralTimer::isRinging(){
    return ringing_;
}

GeneralTimer::GeneralTimer(){
    ESP_LOGI(TAG, "***************************************");
    ESP_LOGI(TAG, "*    General Timer initialized.       *");
    ESP_LOGI(TAG, "*    Author : XvSenfeng               *");
    ESP_LOGI(TAG, "*    QQ群: 719932592                  *");
    ESP_LOGI(TAG, "*    移植代码需要标明作者信息         *");
    ESP_LOGI(TAG, "***************************************");

    // 读取定时器事件列表
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            GeneralTimer*timer = (GeneralTimer*)arg;
            timer->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "e_pet_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &electromic_prt_timer_);
    esp_timer_start_periodic(electromic_prt_timer_, 1000000); // 1 second
#if CONFIG_ALARM_USE_SD_CARD
    TimerReadCsvTimer();
#endif
}

GeneralTimer::~GeneralTimer(){
    if (electromic_prt_timer_ != nullptr) {
        esp_timer_stop(electromic_prt_timer_);
        esp_timer_delete(electromic_prt_timer_);
        electromic_prt_timer_ = nullptr;
    }
    
    // 清理定时器事件列表中的内存
    for (auto& event : e_pet_timer_events) {
        if (event.type == E_PET_TIMER_MESSAGE && event.message != nullptr) {
            free(event.message);
        }
    }
    e_pet_timer_events.clear();
}

void GeneralTimer::OnClockTimer(){
    
    // 保存时钟计数到设置中
    
    // 处理定时器事件
    TimerEventProcess();
}



#if CONFIG_ALARM_USE_SD_CARD

/// @brief 处理单条CSV消息
/// @param csv_info 获取的数据
/// @param delta_sec 返回参数, 第一个触发时间
/// @param interval_sec 返回参数, 周期时间
void GeneralTimer::CalculationNextAndRepeat(timer_info_t *csv_info, long *delta_sec, long *interval_sec){
    *delta_sec = -1;
    *interval_sec = csv_info->repeat_second;
    // 使用csv_info里面的数据计算出对应的time_t
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_sec  = csv_info->tm_sec;
    tm_val.tm_min  = csv_info->tm_min;
    tm_val.tm_hour = csv_info->tm_hour;
    tm_val.tm_mday = csv_info->re_mday;
    tm_val.tm_mon  = csv_info->re_mon - 1;  // struct tm的月份是0-11
    tm_val.tm_year = csv_info->re_year - 1900; // struct tm的年份从1900开始

    tm_val.tm_isdst = -1; // 让mktime自动判断夏令时
    time_t now = time(nullptr);

    time_t target_time = mktime(&tm_val);

    if (target_time > now) {
        *delta_sec = target_time - now;
    } else {
        if(csv_info->repeat_second > 0){
            while(target_time < now){
                target_time += csv_info->repeat_second;
            }
            *delta_sec = target_time - now;
        }
    }
    ESP_LOGI(TAG, "Delta sec: %ld, Interval sec: %ld", *delta_sec, *interval_sec);
}

bool GeneralTimer::DealTimerInfo(timer_info_t *timer_info){
    long delta_sec = 0;
    long interval_sec = 0;
    CalculationNextAndRepeat(timer_info, &delta_sec, &interval_sec);
    if(delta_sec <= 0){
        ESP_LOGE(TAG, "Delta sec is less than 0");
        return false;
    }
    char *message = (char*)malloc(strlen(timer_info->message) + 1);
    if (message == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for message");
        return false;;
    }
    strcpy(message, timer_info->message);
    if(timer_info->function_id == 0){
        // 设置定时器事件
        TimerAddTimerEventRepeat(
            time(nullptr) + delta_sec, E_PET_TIMER_MESSAGE, 
            0, 
            (void*)message, interval_sec, 0, 0);
    }else{
        // 设置定时器事件
        ESP_LOGI(TAG, "Function ID: %d", timer_info->function_id);
        TimerAddTimerEventRepeat(
            time(nullptr) + delta_sec, E_PET_TIMER_FUNCTION, 
            timer_info->function_id, 
            (void*)message, interval_sec, 0, 0);
    }
    ESP_LOGI(TAG, "Timer event added: %s, delta_sec: %ld, interval_sec: %ld", 
             timer_info->message, delta_sec, interval_sec);
    return true;
}


/// @brief 读取定时器配置文件
void GeneralTimer::TimerReadCsvTimer(){
    const char *file_hello = CONFIG_ALARM_SD_CARD_MOUNT_POINT"/general_timer.csv";
    ESP_LOGI(TAG, "Reading file: %s", file_hello);
    // 打开文件
    FILE *f = fopen(file_hello, "r");  // 以只读方式打开文件
    if (f == NULL) {
        ESP_LOGW(TAG, "Alarm persistence file not available (SD may be absent), skip loading");
        return;
    }
    loading_from_csv_ = true;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        timer_info_t csv_info;

        int ret = sscanf(line, "%d,%d,%d,%d,%d,%d,%d,%d,%[^\n]", 
            &csv_info.tm_sec, &csv_info.tm_min, &csv_info.tm_hour, 
            &csv_info.re_mday, &csv_info.re_mon, &csv_info.re_year,
            &csv_info.repeat_second, 
            &csv_info.function_id,
            csv_info.message);
        if (ret == 9) {
            DealTimerInfo(&csv_info);
        } else {
            ESP_LOGE(TAG, "Failed to parse line: %s", line);
        }
    }
    fclose(f);
    loading_from_csv_ = false;
    TimerUpdateCsvTimer();
}
void GeneralTimer::TimerUpdateCsvTimer(){
        // 用e_pet_timer_events重写CSV文件（覆盖整个文件内容）
    const char *file_path = CONFIG_ALARM_SD_CARD_MOUNT_POINT"/general_timer.csv";
    FILE *f_w = fopen(file_path, "w");
    if (f_w == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }
    int num = 0;
    for (const auto& event : e_pet_timer_events) {
        num++;
        int tm_sec = 0, tm_min = 0, tm_hour = 0, re_mday = 0, re_mon = 0, re_year = 0, repeat_second = 0, function_id = 0;
        char message[1024] = {0};

        // 根据类型拼装内容
        if (event.type == E_PET_TIMER_MESSAGE) {
            // 解析trigger_time为时间字段
            time_t t = event.trigger_time;
            struct tm tm_buf;
            localtime_r(&t, &tm_buf);
            tm_sec = tm_buf.tm_sec;
            tm_min = tm_buf.tm_min;
            tm_hour = tm_buf.tm_hour;
            re_mday = tm_buf.tm_mday;
            re_mon = tm_buf.tm_mon + 1; // struct tm: [0,11]
            re_year = tm_buf.tm_year + 1900;
            repeat_second = event.repeat_time;
            function_id = 0;
            if(event.message != nullptr) {
                strncpy(message, event.message, sizeof(message)-1);
                message[sizeof(message)-1] = 0;
            } else {
                message[0] = '\0';
            }
        } else if (event.type == E_PET_TIMER_FUNCTION) {
            // 解析trigger_time为时间字段
            time_t t = event.trigger_time;
            struct tm tm_buf;
            localtime_r(&t, &tm_buf);
            tm_sec = tm_buf.tm_sec;
            tm_min = tm_buf.tm_min;
            tm_hour = tm_buf.tm_hour;
            re_mday = tm_buf.tm_mday;
            re_mon = tm_buf.tm_mon + 1;
            re_year = tm_buf.tm_year + 1900;
            repeat_second = event.repeat_time;

            // 查找function_id
            function_id = 0;
            int j;
            for (j = 0; j < 20; ++j) {
                if (callback_catalogue[j].callback == event.function.callback) {
                    function_id = j + 1; // function_id为下标+1
                    break;
                }
            }
            if(j == 20){
                ESP_LOGE(TAG, "Function ID not found");
                continue;
            }

            if(event.function.arg != nullptr) {
                strncpy(message, (char*)event.function.arg, sizeof(message)-1);
                message[sizeof(message)-1] = '\0';
            } else {
                message[0] = '\0';
            }
        }
        // 写入一条记录
        fprintf(f_w, "%d,%d,%d,%d,%d,%d,%d,%d,%s\n",
            tm_sec, tm_min, tm_hour,
            re_mday, re_mon, re_year,
            repeat_second, function_id, message);
    }
    ESP_LOGI(TAG, "Updated %d timer events to CSV file", num);
    fclose(f_w);
}

/// @brief 将定时器信息追加到CSV文件
/// @param event 定时器事件
void GeneralTimer::TimerAddTimerEventToCsv(
    e_pet_timer_event_t event
){
    // 将定时器信息追加到CSV文件
    const char *file_timer = CONFIG_ALARM_SD_CARD_MOUNT_POINT"/general_timer.csv";
    FILE *f = fopen(file_timer, "a");  // 以追加方式打开文件
    if (f != NULL) {
        // 计算触发时间的各个字段
        struct tm *trigger_tm = localtime(&event.trigger_time);
        
        // 格式化定时器信息并写入文件
        // 格式: tm_sec,tm_min,tm_hour,re_mday,re_mon,re_year,re_wday,random_l,random_h,function_id,message
        if (event.type == E_PET_TIMER_MESSAGE) {
            fprintf(f, "%d,%d,%d,%d,%d,%d,%d,%d,%s\n",
                trigger_tm->tm_sec, trigger_tm->tm_min, trigger_tm->tm_hour,
                trigger_tm->tm_mday, trigger_tm->tm_mon + 1, trigger_tm->tm_year + 1900,  // re_mday, re_mon, re_year, re_wday (单次事件设为0)
                event.repeat_time,
                0,           // function_id (消息类型为0)
                event.message);
        } else if (event.type == E_PET_TIMER_FUNCTION) {
            // 对于函数类型，需要找到对应的function_id
            int function_id = 0;
            for (int i = 0; i < 20; i++) {
                if (callback_catalogue[i].callback == event.function.callback) {
                    function_id = i + 1;  // function_id从1开始
                    break;
                }
            }
            fprintf(f, "%d,%d,%d,%d,%d,%d,%d,%d,%s\n",
                trigger_tm->tm_sec, trigger_tm->tm_min, trigger_tm->tm_hour,
                trigger_tm->tm_mday, trigger_tm->tm_mon + 1, trigger_tm->tm_year + 1900,  // re_mday, re_mon, re_year, re_wday (单次事件设为0)
                event.repeat_time,        
                function_id, // function_id
                (char*)event.function.arg);  // 函数调用消息
        }
        
        fclose(f);
        ESP_LOGI(TAG, "Timer event appended to CSV file");
    } else {
        ESP_LOGW(TAG, "Cannot persist alarm to SD (card not mounted?)");
    }
}

#endif // CONFIG_ALARM_USE_SD_CARD
void GeneralTimer::TimerEventSort(){
    std::sort(e_pet_timer_events.begin(), e_pet_timer_events.end(), [](const e_pet_timer_event_t& a, const e_pet_timer_event_t& b) {
        return a.trigger_time < b.trigger_time;
    });
}
/// @brief 设置定时器事件(相对时间)
/// @param seconds 多长时间以后
/// @param type 设置的事件类型
/// @param callback 回调函数
/// @param arg 参数(MESAGEE类型时，传入消息字符串的地址, FUNCTION类型时，传入函数的参数)
void GeneralTimer::TimerAddTimerEventRelative(
    int seconds, 
    e_pet_timer_type_e type, 
    int function_id, 
    void* arg,
    bool repeat
){
    std::lock_guard<std::mutex> lock(mutex_);
    e_pet_timer_event_t event;
    time_t current_time = time(nullptr);
    event.trigger_time = current_time + seconds;
    event.type = type;
    if (type == E_PET_TIMER_FUNCTION) {
        event.function.callback = (callback_f)callback_catalogue[function_id - 1].callback;
        event.function.arg = arg;
    } else {
        event.message = (char*)arg;
    }
    if(repeat){
        event.repeat_time = seconds;
    }else{
        event.repeat_time = 0;
    }
    e_pet_timer_events.push_back(event);
    TimerEventSort();
    ESP_LOGI(TAG, "Added timer event: %lld, type: %d", event.trigger_time, type);
#if CONFIG_ALARM_USE_SD_CARD
    if (!loading_from_csv_) {
        TimerAddTimerEventToCsv(event);
    }
#endif

}



/// @brief 设置定时器事件(绝对时间)
/// @param trigger_time 
/// @param type 
/// @param callback 
/// @param arg 
void GeneralTimer::TimerAddTimerEventAbsolute(
    time_t trigger_time, 
    e_pet_timer_type_e type, 
    int function_id, 
    void* arg,
    bool repeat
){
    std::lock_guard<std::mutex> lock(mutex_);
    e_pet_timer_event_t event;
    event.trigger_time = trigger_time;
    event.type = type;
    if (type == E_PET_TIMER_FUNCTION) {
        event.function.callback = callback_catalogue[function_id - 1].callback;
        event.function.arg = arg;
    } else {
        event.message = (char*)arg;
    }
    if(repeat){
        time_t current_time = time(nullptr);
        event.repeat_time = trigger_time - current_time;
    }else{
        event.repeat_time = 0;
    }
    e_pet_timer_events.push_back(event);
#if CONFIG_ALARM_USE_SD_CARD
    if (!loading_from_csv_) {
        TimerAddTimerEventToCsv(event);
    }
#endif
    TimerEventSort();
}

void GeneralTimer::TimerAddTimerEventRepeat(
    time_t trigger_time, 
    e_pet_timer_type_e type, 
    int function_id, 
    void* arg,
    int repeat_time,
    int random_l,
    int random_h
){
    std::lock_guard<std::mutex> lock(mutex_);
    e_pet_timer_event_t event;
    event.trigger_time = trigger_time;
    event.type = type;
    if (type == E_PET_TIMER_FUNCTION) {
        event.function.callback = callback_catalogue[function_id - 1].callback;
        event.function.arg = arg;
    } else {
        event.message = (char*)arg;
    }
    event.repeat_time = repeat_time;
    event.random_l = random_l;
    event.random_h = random_h;
    e_pet_timer_events.push_back(event);
#if CONFIG_ALARM_USE_SD_CARD
    if (!loading_from_csv_) {
        TimerAddTimerEventToCsv(event);
    }
#endif
    TimerEventSort();
}

void GeneralTimer::TimerEventProcess(){
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr time_t kAlarmFireGraceSeconds = 2;  // allow tiny scheduler drift
    time_t current_time = time(nullptr);
    for (auto it = e_pet_timer_events.begin(); it != e_pet_timer_events.end();) {
        if (it->trigger_time <= current_time) {
            const time_t late_seconds = current_time - it->trigger_time;
            const bool should_fire = (late_seconds <= kAlarmFireGraceSeconds);

            if (should_fire) {
                if (it->type == E_PET_TIMER_FUNCTION && it->function.callback != nullptr) {
                    it->function.callback(it->function.arg);
                } else {
                    ESP_LOGI(TAG, "Message: %s", it->message);
                    std::string message = it->message;
                    auto &app = Application::GetInstance();
                    app.Schedule([message]() {
                        Application::GetInstance().Alert("Alarm", message.c_str(), "alarm_clock");
                    });
                }
            } else {
                ESP_LOGI(TAG, "Skip overdue alarm (late=%llds)", static_cast<long long>(late_seconds));
            }
            
            if(it->repeat_time > 0){
                // Advance repeating alarms directly to a future time so we don't
                // spam callbacks when trigger_time is far behind current_time.
                if (it->random_l) {
                    int guard = 0;
                    do {
                        int random_step = (rand() % (it->random_h - it->random_l + 1)) + it->random_l;
                        it->trigger_time += random_step * it->repeat_time;
                        ++guard;
                    } while (it->trigger_time <= current_time && guard < 64);
                    if (it->trigger_time <= current_time) {
                        it->trigger_time = current_time + it->repeat_time;
                    }
                } else {
                    const time_t step = static_cast<time_t>(it->repeat_time);
                    const time_t overdue = current_time - it->trigger_time;
                    const time_t jumps = overdue / step + 1;
                    it->trigger_time += jumps * step;
                }
                ++it;
            }else{
                if(it->type == E_PET_TIMER_FUNCTION){
                    free(it->function.arg); // 释放函数参数
                }else{
                    free(it->message); // 释放消息字符串
                }

                it = e_pet_timer_events.erase(it); // 删除已处理的事件
#if CONFIG_ALARM_USE_SD_CARD
                TimerUpdateCsvTimer();
#endif
            }
        } else {
            ++it;
        }
    }
    TimerEventSort();
}
