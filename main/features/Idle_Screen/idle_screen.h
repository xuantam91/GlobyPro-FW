#pragma once

#include <functional>
#include <esp_timer.h>
#include <string>
#include <chrono>
#include <ctime>
#include <array>

#ifdef CONFIG_ENABLE_IDLE_SCREEN
#include <lvgl.h>
#endif

class Display;

class IdleScreen {
public:
    enum class Theme {
        Pink = 0,
        Orange = 1,
        BluePastel = 2,
        Yellow = 3,
    };

    IdleScreen(Display* display);
    ~IdleScreen();

    void Start();
    void Stop();
    void ResetTimer();  // 重置待机计时器（有用户活动时调用）
    void ToggleTheme();
    Theme GetTheme() const {
#ifdef CONFIG_ENABLE_IDLE_SCREEN
        return current_theme_;
#else
        return Theme::Pink;
#endif
    }
    void SetTheme(Theme theme);
    void PreviewTheme(Theme theme);
    void RestoreSavedTheme();
    bool IsActive() const { return is_active_; }

    enum class MainMenuItem {
        AiTalk = 0,
        SdMusic = 1,
        OnlineMusic = 2,
        Radio = 3,
        AlarmClock = 4,
        Setting = 5,
    };

    bool ToggleMainMenu();
    bool IsMainMenuVisible() const { return menu_visible_; }
    bool MoveMainMenu(int delta);
    MainMenuItem GetMainMenuItem() const;
    bool SelectMainMenuItem();
    void SetMainMenuSelectCallback(std::function<void(MainMenuItem)> cb);
    bool SetMainMenuPicker(const std::string& title, const std::string& value);
    void ClearMainMenuPicker();
    bool IsMainMenuPickerActive() const { return picker_visible_; }

private:
    void CheckIdleTimeout();
    void ShowIdleScreen();
    void HideIdleScreen();
    void UpdateDisplay();
    
    void CreateIdleScreenUI();
    void DestroyIdleScreenUI();
    
    std::string GetCurrentTime();
    std::string GetCurrentDate();
    std::string GetWeekDay();
    void ApplyThemeStyles();
    std::string BuildGreetingText(int second) const;
    std::string BuildAlarmText(time_t trigger_time, const std::string& message) const;
    void SelectRandomGreetingTemplate();
    void UpdateMainMenuStyles();
#ifdef CONFIG_ENABLE_IDLE_SCREEN
    void AnimateFlip(lv_obj_t* label, const char* new_text);
#endif
    const char* GetBatteryIconByLevel(int level, bool charging) const;

    Display* display_;
    esp_timer_handle_t idle_timer_;
    esp_timer_handle_t update_timer_;
    
    bool is_active_;
    bool is_enabled_;
    Theme current_theme_;
    std::chrono::system_clock::time_point last_activity_time_;  // 上次用户活动时间
    
    static constexpr int kIdleTimeoutSeconds = 10;  // 固定 10 秒超时，与状态栏时间显示一致
    static constexpr int kMainMenuTimeoutSeconds = 15;
    
    // LVGL UI objects
#ifdef CONFIG_ENABLE_IDLE_SCREEN
    lv_obj_t* idle_container_;
    lv_obj_t* background_img_;    // 背景图片（从 Assets 下载）
    lv_obj_t* logo_img_;          // Logo 图片
    lv_obj_t* slogan_label_;
    lv_obj_t* time_label_;
    lv_obj_t* weekday_label_;
    lv_obj_t* date_label_;
    lv_obj_t* greeting_label_;
    lv_obj_t* wifi_icon_label_;
    lv_obj_t* battery_icon_label_;
    lv_obj_t* hour_label_;
    lv_obj_t* minute_label_;
    lv_obj_t* second_label_;
    lv_obj_t* clock_row_;
    lv_obj_t* info_row_;
    lv_obj_t* weekday_chip_;
    lv_obj_t* date_chip_;
    lv_obj_t* menu_panel_;
    lv_obj_t* menu_stage_;
    lv_obj_t* menu_title_;
    lv_obj_t* menu_hint_label_;
    lv_obj_t* menu_wifi_guide_label_;
    std::array<lv_obj_t*, 5> picker_row_labels_;
    std::array<lv_obj_t*, 6> menu_rows_;
    std::array<lv_obj_t*, 6> menu_icon_labels_;
    std::array<lv_obj_t*, 6> menu_text_labels_;
#else
    void* idle_container_;
    void* background_img_;
    void* logo_img_;
    void* slogan_label_;
    void* time_label_;
    void* weekday_label_;
    void* date_label_;
    void* greeting_label_;
    void* wifi_icon_label_;
    void* battery_icon_label_;
    void* hour_label_;
    void* minute_label_;
    void* second_label_;
    void* clock_row_;
    void* info_row_;
    void* weekday_chip_;
    void* date_chip_;
    void* menu_panel_;
    void* menu_stage_;
    void* menu_title_;
    void* menu_hint_label_;
    void* menu_wifi_guide_label_;
    std::array<void*, 5> picker_row_labels_;
    std::array<void*, 6> menu_rows_;
    std::array<void*, 6> menu_icon_labels_;
    std::array<void*, 6> menu_text_labels_;
#endif
    std::array<int, 3> last_hms_;
    std::string greeting_name_;
    std::string greeting_template_;
    int greeting_template_index_ = -1;
    bool greeting_mode_cache_ = true;
    int bottom_line_phase_cache_ = -1;
    std::string bottom_line_text_cache_;
    bool menu_visible_ = false;
    int menu_index_ = 0;
    bool picker_visible_ = false;
    std::string picker_title_;
    std::string picker_value_;
    std::function<void(MainMenuItem)> main_menu_select_cb_;
};

