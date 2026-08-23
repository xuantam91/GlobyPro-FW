#include "idle_screen.h"
#include "display.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include <font_awesome.h>

#include <esp_log.h>
#include <esp_random.h>
#include <cstring>  // for strcmp
#include <cctype>
#include <array>
#include <vector>
#include <sstream>
#include <string_view>

#ifdef CONFIG_ENABLE_IDLE_SCREEN
#include <time.h>
#include <sys/time.h>

#ifdef HAVE_LVGL
#include "lvgl_theme.h"
#endif

// 包含项目字体作为后备
extern "C" {
    extern const lv_font_t BUILTIN_TEXT_FONT;
    extern const lv_font_t BUILTIN_ICON_FONT;
    extern const lv_font_t font_awesome_30_4;
}
#endif

#define TAG "IdleScreen"

#ifdef CONFIG_ENABLE_IDLE_SCREEN

namespace {
struct ThemePalette {
    uint32_t bg_top;
    uint32_t bg_bottom;
    uint32_t logo;
    uint32_t slogan;
    uint32_t card_bg;
    uint32_t card_border;
    uint32_t time_text;
    uint32_t weekday_chip_bg;
    uint32_t date_chip_bg;
    uint32_t greeting_text;
    uint32_t greeting_name;
};

ThemePalette GetThemePalette(IdleScreen::Theme theme) {
    if (theme == IdleScreen::Theme::Orange) {
        return {
            .bg_top = 0xFFD39F,
            .bg_bottom = 0xFFB067,
            .logo = 0xFFFFFF,
            .slogan = 0xFFF8EC,
            .card_bg = 0xFFF7EE,
            .card_border = 0xF6CFA8,
            .time_text = 0xD66A2A,
            .weekday_chip_bg = 0xF08B3A,
            .date_chip_bg = 0x4EA9DA,
            .greeting_text = 0xFFF8EC,
            .greeting_name = 0xFFFFFF,
        };
    }
    if (theme == IdleScreen::Theme::BluePastel) {
        return {
            .bg_top = 0x9EC5E6,
            .bg_bottom = 0x7CA6C9,
            .logo = 0xFFFFFF,
            .slogan = 0xEEF7FF,
            .card_bg = 0xF7FCFF,
            .card_border = 0xB8D8F8,
            .time_text = 0x4A90C2,
            .weekday_chip_bg = 0x3A6A8F,
            .date_chip_bg = 0xEA9CA3,
            .greeting_text = 0xEEF7FF,
            .greeting_name = 0xFFFFFF,
        };
    }
    if (theme == IdleScreen::Theme::Yellow) {
        return {
            .bg_top = 0xFCE89B,
            .bg_bottom = 0xE6C35C,
            .logo = 0xFFFFFF,
            .slogan = 0xFFFFF0,
            .card_bg = 0xFFFFF5,
            .card_border = 0xFFEB99,
            .time_text = 0xB38600,
            .weekday_chip_bg = 0xA6833D,
            .date_chip_bg = 0x71C5A1,
            .greeting_text = 0xFFFFF0,
            .greeting_name = 0xFFFFFF,
        };
    }

    return {
        .bg_top = 0xFFB5C8,
        .bg_bottom = 0xFF9AB7,
        .logo = 0xFFFFFF,
        .slogan = 0xFFF4FA,
        .card_bg = 0xFFFFFF,
        .card_border = 0xFFD3E2,
        .time_text = 0xF06292,
        .weekday_chip_bg = 0xF06292,
        .date_chip_bg = 0x4FC3F7,
        .greeting_text = 0xFFF4FA,
        .greeting_name = 0xFFFFFF,
    };
}

IdleScreen::Theme NextTheme(IdleScreen::Theme theme) {
    switch (theme) {
        case IdleScreen::Theme::Pink:
            return IdleScreen::Theme::Orange;
        case IdleScreen::Theme::Orange:
            return IdleScreen::Theme::BluePastel;
        case IdleScreen::Theme::BluePastel:
            return IdleScreen::Theme::Yellow;
        case IdleScreen::Theme::Yellow:
        default:
            return IdleScreen::Theme::Pink;
    }
}

const char* ThemeName(IdleScreen::Theme theme) {
    switch (theme) {
        case IdleScreen::Theme::Pink:
            return "pink";
        case IdleScreen::Theme::Orange:
            return "orange";
        case IdleScreen::Theme::BluePastel:
            return "blue";
        case IdleScreen::Theme::Yellow:
            return "yellow";
        default:
            return "unknown";
    }
}

const std::array<const char*, 4> kGreetingTemplates = {
    "Hi, {name}! Let's Chat",
    "Welcome back, {name}!",
    "Hi, {name}! Ready for fun?",
    "Hi, {name}! Ready for learn?",
};

bool IsLuxiaoban154Board() {
    return std::string_view(BOARD_TYPE) == "luxiaoban-xiaozhi-1.54tft";
}
}  // namespace

// ============= 启用待机界面功能时的完整实现 =============

IdleScreen::IdleScreen(Display* display)
    : display_(display),
      is_active_(false),
      is_enabled_(false),
      current_theme_(static_cast<Theme>(DEFAULT_THEME_VAL)),
      last_activity_time_(std::chrono::system_clock::now()),
      idle_container_(nullptr),
      background_img_(nullptr),
      logo_img_(nullptr),
      slogan_label_(nullptr),
      time_label_(nullptr),
      weekday_label_(nullptr),
      date_label_(nullptr),
      greeting_label_(nullptr),
      wifi_icon_label_(nullptr),
      battery_icon_label_(nullptr),
      hour_label_(nullptr),
      minute_label_(nullptr),
      second_label_(nullptr),
      clock_row_(nullptr),
      info_row_(nullptr),
      weekday_chip_(nullptr),
      date_chip_(nullptr),
      menu_panel_(nullptr),
      menu_stage_(nullptr),
      menu_title_(nullptr),
      menu_hint_label_(nullptr),
      menu_wifi_guide_label_(nullptr),
      picker_row_labels_{},
      menu_rows_{},
      menu_icon_labels_{},
      menu_text_labels_{},
      last_hms_{-1, -1, -1},
      greeting_name_(),
      greeting_template_("Hi, {name}! Let's Chat"),
      greeting_template_index_(-1),
      greeting_mode_cache_(true),
      bottom_line_phase_cache_(-1),
      bottom_line_text_cache_() {
    {
        Settings settings("idle_screen", false);
        int saved_theme = settings.GetInt("theme", DEFAULT_THEME_VAL);
        if (saved_theme < static_cast<int>(Theme::Pink) || saved_theme > static_cast<int>(Theme::Yellow)) {
            saved_theme = DEFAULT_THEME_VAL;
        }
        current_theme_ = static_cast<Theme>(saved_theme);
    }
    
    // 创建待机检测定时器（每秒检查一次）
    esp_timer_create_args_t idle_timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<IdleScreen*>(arg);
            self->CheckIdleTimeout();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "idle_screen_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&idle_timer_args, &idle_timer_));
    
    // 创建界面更新定时器（每秒更新一次）
    esp_timer_create_args_t update_timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<IdleScreen*>(arg);
            if (self->is_active_) {
                self->UpdateDisplay();
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "idle_screen_update",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&update_timer_args, &update_timer_));
    
    ESP_LOGI(TAG, "IdleScreen initialized, timeout: %d seconds (auto-tracked)", kIdleTimeoutSeconds);
}

IdleScreen::~IdleScreen() {
    Stop();
    
    if (idle_timer_ != nullptr) {
        esp_timer_stop(idle_timer_);
        esp_timer_delete(idle_timer_);
    }
    
    if (update_timer_ != nullptr) {
        esp_timer_stop(update_timer_);
        esp_timer_delete(update_timer_);
    }
    
    DestroyIdleScreenUI();
}

void IdleScreen::Start() {
    if (!is_enabled_) {
        is_enabled_ = true;
        last_activity_time_ = std::chrono::system_clock::now();  // 初始化活动时间
        ESP_ERROR_CHECK(esp_timer_start_periodic(idle_timer_, 1000000));  // 每秒
        ESP_LOGI(TAG, "IdleScreen started");
    }
}

void IdleScreen::Stop() {
    if (is_enabled_) {
        is_enabled_ = false;
        esp_timer_stop(idle_timer_);
        esp_timer_stop(update_timer_);
        HideIdleScreen();
        ESP_LOGI(TAG, "IdleScreen stopped");
    }
}

void IdleScreen::ResetTimer() {
    last_activity_time_ = std::chrono::system_clock::now();
    
    if (is_active_) {
        HideIdleScreen();
    }
}

void IdleScreen::ToggleTheme() {
    current_theme_ = NextTheme(current_theme_);
    {
        Settings settings("idle_screen", true);
        settings.SetInt("theme", static_cast<int>(current_theme_));
    }
    ESP_LOGI(TAG, "Idle theme switched to %s", ThemeName(current_theme_));
    if (!is_active_ || idle_container_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    ApplyThemeStyles();
}

void IdleScreen::SetTheme(Theme theme) {
    current_theme_ = theme;
    {
        Settings settings("idle_screen", true);
        settings.SetInt("theme", static_cast<int>(current_theme_));
    }
    ESP_LOGI(TAG, "Idle theme set to %s", ThemeName(current_theme_));
    if (!is_active_ || idle_container_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    ApplyThemeStyles();
}

void IdleScreen::PreviewTheme(Theme theme) {
    current_theme_ = theme;
    ESP_LOGI(TAG, "Idle theme preview set to %s", ThemeName(current_theme_));
    if (!is_active_ || idle_container_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    ApplyThemeStyles();
}

void IdleScreen::RestoreSavedTheme() {
    Settings settings("idle_screen", false);
    int saved_theme = settings.GetInt("theme", DEFAULT_THEME_VAL);
    current_theme_ = static_cast<Theme>(saved_theme);
    ESP_LOGI(TAG, "Idle theme restored to saved: %s", ThemeName(current_theme_));
    if (!is_active_ || idle_container_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    ApplyThemeStyles();
}

bool IdleScreen::ToggleMainMenu() {
    if (!is_active_) {
        ShowIdleScreen();
    }
    if (idle_container_ == nullptr || menu_panel_ == nullptr) {
        return false;
    }

    DisplayLockGuard lock(display_);
    menu_visible_ = !menu_visible_;
    if (menu_visible_) {
        picker_visible_ = false;
        picker_title_.clear();
        picker_value_.clear();
        lv_obj_clear_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);
        if (menu_stage_ != nullptr) {
            lv_obj_add_flag(menu_stage_, LV_OBJ_FLAG_HIDDEN);
        }
        for (auto* row : menu_rows_) {
            if (row != nullptr) {
                lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (clock_row_ != nullptr) {
            lv_obj_add_flag(clock_row_, LV_OBJ_FLAG_HIDDEN);
        }
        if (info_row_ != nullptr) {
            lv_obj_add_flag(info_row_, LV_OBJ_FLAG_HIDDEN);
        }
        if (greeting_label_ != nullptr) {
            lv_obj_add_flag(greeting_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (menu_hint_label_ != nullptr) {
            lv_obj_add_flag(menu_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        UpdateMainMenuStyles();
        last_activity_time_ = std::chrono::system_clock::now();
    } else {
        lv_obj_add_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);
        if (menu_stage_ != nullptr) {
            lv_obj_add_flag(menu_stage_, LV_OBJ_FLAG_HIDDEN);
        }
        if (clock_row_ != nullptr) {
            lv_obj_clear_flag(clock_row_, LV_OBJ_FLAG_HIDDEN);
        }
        if (info_row_ != nullptr) {
            lv_obj_clear_flag(info_row_, LV_OBJ_FLAG_HIDDEN);
        }
        if (greeting_label_ != nullptr) {
            lv_obj_clear_flag(greeting_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (menu_hint_label_ != nullptr) {
            lv_obj_add_flag(menu_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    return true;
}

bool IdleScreen::MoveMainMenu(int delta) {
    if (!menu_visible_ || menu_panel_ == nullptr) {
        return false;
    }
    constexpr int kMenuCount = 6;
    menu_index_ = (menu_index_ + delta) % kMenuCount;
    if (menu_index_ < 0) {
        menu_index_ += kMenuCount;
    }
    last_activity_time_ = std::chrono::system_clock::now();
    DisplayLockGuard lock(display_);
    UpdateMainMenuStyles();
    return true;
}

IdleScreen::MainMenuItem IdleScreen::GetMainMenuItem() const {
    return static_cast<MainMenuItem>(menu_index_);
}

bool IdleScreen::SelectMainMenuItem() {
    if (!menu_visible_) {
        return false;
    }
    if (main_menu_select_cb_) {
        main_menu_select_cb_(GetMainMenuItem());
    }
    last_activity_time_ = std::chrono::system_clock::now();
    menu_visible_ = false;
    if (menu_panel_ != nullptr) {
        DisplayLockGuard lock(display_);
        lv_obj_add_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);
        if (clock_row_ != nullptr) {
            lv_obj_clear_flag(clock_row_, LV_OBJ_FLAG_HIDDEN);
        }
        if (info_row_ != nullptr) {
            lv_obj_clear_flag(info_row_, LV_OBJ_FLAG_HIDDEN);
        }
        if (greeting_label_ != nullptr) {
            lv_obj_clear_flag(greeting_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    return true;
}

void IdleScreen::SetMainMenuSelectCallback(std::function<void(MainMenuItem)> cb) {
    main_menu_select_cb_ = std::move(cb);
}

bool IdleScreen::SetMainMenuPicker(const std::string& title, const std::string& value) {
    if (!menu_visible_ || menu_panel_ == nullptr || menu_hint_label_ == nullptr || menu_stage_ == nullptr || menu_title_ == nullptr) {
        return false;
    }
    last_activity_time_ = std::chrono::system_clock::now();
    picker_visible_ = true;
    picker_title_ = title;
    picker_value_ = value;
    const bool is_setting_picker = (picker_title_ == "Setting") ||
                                   (picker_title_ == "Sound & Screen") ||
                                   (picker_title_ == "System") ||
                                   (picker_title_ == "Power") ||
                                   (picker_title_ == "Power Timer") ||
                                   (picker_title_ == "About speaker") ||
                                   (picker_title_ == "Device Info") ||
                                   (picker_title_ == "Device Information") ||
                                   (picker_title_ == "Restart?") ||
                                   (picker_title_ == "Shut Down?") ||
                                   (picker_title_ == "Alarm Edit") ||
                                   (picker_title_ == "Custom OTA") ||
                                   (picker_title_ == "Reset WiFi?") ||
                                   (picker_title_ == "Theme");
    const bool is_wifi_picker = (picker_title_ == "Wi-Fi");
    const bool is_list_picker = (value.find("[DIR]") != std::string::npos) ||
                                (value.find(".mp3") != std::string::npos) ||
                                (picker_title_ == "Radio") ||
                                (picker_title_ == "Alarm") ||
                                is_wifi_picker ||
                                is_setting_picker;
    DisplayLockGuard lock(display_);
    lv_label_set_text(menu_title_, picker_title_.c_str());
    for (auto* row : menu_rows_) {
        if (row != nullptr) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_clear_flag(menu_stage_, LV_OBJ_FLAG_HIDDEN);
    if (!is_list_picker) {
        for (auto* row_label : picker_row_labels_) {
            if (row_label != nullptr) {
                lv_obj_add_flag(row_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_set_width(menu_hint_label_, 188);
        lv_obj_align(menu_hint_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_text_align(menu_hint_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(menu_hint_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(menu_hint_label_, picker_value_.c_str());
        lv_obj_clear_flag(menu_hint_label_, LV_OBJ_FLAG_HIDDEN);
        if (menu_wifi_guide_label_ != nullptr) {
            lv_obj_add_flag(menu_wifi_guide_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return true;
    }

    std::vector<std::string> lines;
    {
        std::stringstream ss(picker_value_);
        std::string line;
        while (std::getline(ss, line, '\n')) {
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
    }

    std::string footer;
    if (!lines.empty() && lines.back().find("Page ") != std::string::npos) {
        footer = lines.back();
        lines.pop_back();
    }

    int row_start_y = 42;
    int row_step_y = 20;
    if (is_setting_picker) {
        const size_t visible = lines.size();
        if (visible <= 2) {
            row_start_y = 62;
            row_step_y = 30;
        } else if (visible == 3) {
            row_start_y = 52;
            row_step_y = 24;
        } else if (visible == 4) {
            row_start_y = 44;
            row_step_y = 22;
        } else {
            row_start_y = 36;
            row_step_y = 22;
        }
    } else if (is_wifi_picker) {
        row_start_y = 36;
        row_step_y = 22;
    }

    for (size_t i = 0; i < picker_row_labels_.size(); ++i) {
        if (picker_row_labels_[i] == nullptr) {
            continue;
        }
        lv_obj_align(picker_row_labels_[i], LV_ALIGN_TOP_LEFT, 0, row_start_y + static_cast<int>(i) * row_step_y);
        if (i < lines.size()) {
            lv_label_set_text(picker_row_labels_[i], lines[i].c_str());
            const bool is_selected_line = (lines[i].find("> ") != std::string::npos);
            lv_label_set_long_mode(
                picker_row_labels_[i],
                is_selected_line ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP);
            lv_obj_set_style_anim_duration(
                picker_row_labels_[i],
                is_selected_line ? 12000 : 0, 0);
            lv_obj_clear_flag(picker_row_labels_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(picker_row_labels_[i], "");
            lv_label_set_long_mode(picker_row_labels_[i], LV_LABEL_LONG_CLIP);
            lv_obj_add_flag(picker_row_labels_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!footer.empty()) {
        lv_obj_set_width(menu_hint_label_, 86);
        lv_obj_align(menu_hint_label_, LV_ALIGN_TOP_RIGHT, -2, 28);
        lv_obj_set_style_text_align(menu_hint_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(menu_hint_label_, LV_LABEL_LONG_CLIP);
        lv_label_set_text(menu_hint_label_, footer.c_str());
        lv_obj_clear_flag(menu_hint_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(menu_hint_label_, LV_OBJ_FLAG_HIDDEN);
    }

    if (menu_wifi_guide_label_ != nullptr) {
        if (is_wifi_picker) {
            lv_label_set_text(menu_wifi_guide_label_, "Chọn Wi-Fi có sóng mạnh hơn, ưu tiên số gần 0 hơn");
            lv_obj_clear_flag(menu_wifi_guide_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(menu_wifi_guide_label_);
        } else {
            lv_obj_add_flag(menu_wifi_guide_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    return true;
}

void IdleScreen::ClearMainMenuPicker() {
    picker_visible_ = false;
    picker_title_.clear();
    picker_value_.clear();
    if (menu_hint_label_ != nullptr) {
        DisplayLockGuard lock(display_);
        lv_obj_add_flag(menu_hint_label_, LV_OBJ_FLAG_HIDDEN);
        for (auto* row_label : picker_row_labels_) {
            if (row_label != nullptr) {
                lv_obj_add_flag(row_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (menu_stage_ != nullptr) {
            lv_obj_add_flag(menu_stage_, LV_OBJ_FLAG_HIDDEN);
        }
        if (menu_wifi_guide_label_ != nullptr) {
            lv_obj_add_flag(menu_wifi_guide_label_, LV_OBJ_FLAG_HIDDEN);
        }
        for (auto* row : menu_rows_) {
            if (row != nullptr) {
                lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void IdleScreen::CheckIdleTimeout() {
    auto& app = Application::GetInstance();
    
    // Never show idle UI while any media is active.
    if (app.IsMediaPlaying()) {
        last_activity_time_ = std::chrono::system_clock::now();
        if (is_active_) {
            HideIdleScreen();
        }
        return;
    }

    // 检查设备是否处于空闲状态
    if (app.GetDeviceState() != kDeviceStateIdle) {
        last_activity_time_ = std::chrono::system_clock::now();
        if (is_active_) {
            HideIdleScreen();
        }
        return;
    }
    
    // 计算距离上次活动的时间（参考状态栏时间显示的实现）
    auto now = std::chrono::system_clock::now();
    auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity_time_).count();
    
    // 达到超时时间（10秒），显示待机界面
    // 与状态栏时间显示条件保持一致
    if (!is_active_ && idle_duration >= kIdleTimeoutSeconds) {
        // 检查系统时间是否已同步（年份 >= 2025）
        time_t now_time;
        struct tm timeinfo;
        time(&now_time);
        localtime_r(&now_time, &timeinfo);
        
        const bool time_synced = (timeinfo.tm_year >= 2025 - 1900);
        if (time_synced || IsLuxiaoban154Board()) {
            ShowIdleScreen();
        } else {
            ESP_LOGD(TAG, "System time not synced yet, skip showing idle screen (idle: %lld seconds)", idle_duration);
        }
    }
}

void IdleScreen::ShowIdleScreen() {
    if (is_active_) {
        return;
    }
    
    ESP_LOGI(TAG, "Showing idle screen");
    is_active_ = true;
    
    CreateIdleScreenUI();
    UpdateDisplay();
    
    // 启动界面更新定时器
    ESP_ERROR_CHECK(esp_timer_start_periodic(update_timer_, 1000000));  // 每秒更新
}

void IdleScreen::HideIdleScreen() {
    if (!is_active_) {
        return;
    }
    
    ESP_LOGI(TAG, "Hiding idle screen");
    is_active_ = false;
    last_activity_time_ = std::chrono::system_clock::now();
    
    // 停止更新定时器
    esp_timer_stop(update_timer_);
    
    DestroyIdleScreenUI();
}

void IdleScreen::CreateIdleScreenUI() {
    if (idle_container_ != nullptr) {
        return;  // 已经创建
    }

    DisplayLockGuard lock(display_);

    auto screen = lv_screen_active();
    const bool is_luxiaoban_154 = IsLuxiaoban154Board();

    // Board-specific spacing for Luxiaoban 1.54 so the clock does not overlap slogan.
    const int slogan_top_y = is_luxiaoban_154 ? 56 : 58;
    const int clock_center_y = is_luxiaoban_154 ? 6 : -8;
    const int info_row_center_y = is_luxiaoban_154 ? 74 : 74;
    const int greeting_bottom_y = is_luxiaoban_154 ? -12 : -40;

    // Create full-screen container with a soft vertical gradient.
    idle_container_ = lv_obj_create(screen);
    lv_obj_set_size(idle_container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(idle_container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(idle_container_, lv_color_hex(0xFFB5C8), 0);
    lv_obj_set_style_bg_grad_color(idle_container_, lv_color_hex(0xFF9AB7), 0);
    lv_obj_set_style_bg_grad_dir(idle_container_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(idle_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(idle_container_, 0, 0);
    lv_obj_set_style_pad_all(idle_container_, 0, 0);
    lv_obj_set_style_radius(idle_container_, 0, 0);
    lv_obj_clear_flag(idle_container_, LV_OBJ_FLAG_SCROLLABLE);

    // 1) Top status icons
    wifi_icon_label_ = lv_label_create(idle_container_);
    lv_obj_set_style_text_font(wifi_icon_label_, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(wifi_icon_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(wifi_icon_label_, LV_ALIGN_TOP_LEFT, 18, 16);
    lv_label_set_text(wifi_icon_label_, FONT_AWESOME_WIFI);

    battery_icon_label_ = lv_label_create(idle_container_);
    lv_obj_set_style_text_font(battery_icon_label_, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(battery_icon_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(battery_icon_label_, LV_ALIGN_TOP_RIGHT, -18, 16);
    lv_label_set_text(battery_icon_label_, FONT_AWESOME_BATTERY_FULL);

    // 2) Brand logo (moved down to leave clean top status area)
    logo_img_ = lv_label_create(idle_container_);
    lv_obj_set_size(logo_img_, 220, LV_SIZE_CONTENT);
    lv_obj_align(logo_img_, LV_ALIGN_TOP_MID, 0, 22);
    lv_label_set_text(logo_img_, "GLOBY");
    lv_obj_set_style_text_font(logo_img_, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(logo_img_, 1, 0);
    lv_obj_set_style_text_color(logo_img_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(logo_img_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(logo_img_, LV_OPA_COVER, 0);

    // 2.1) Slogan under logo
    slogan_label_ = lv_label_create(idle_container_);
    lv_obj_set_size(slogan_label_, 220, LV_SIZE_CONTENT);
    lv_obj_align(slogan_label_, LV_ALIGN_TOP_MID, 0, slogan_top_y);
    lv_label_set_text(slogan_label_, "Little Learners, Big Futures");
    lv_obj_set_style_text_font(slogan_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(slogan_label_, lv_color_hex(0xFFF4FA), 0);
    lv_obj_set_style_text_opa(slogan_label_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(slogan_label_, LV_TEXT_ALIGN_CENTER, 0);

    // 3) Static flip-look clock row (HH MM SS, no flip animation)
    clock_row_ = lv_obj_create(idle_container_);
    lv_obj_set_size(clock_row_, 220, 88);
    lv_obj_align(clock_row_, LV_ALIGN_CENTER, 0, clock_center_y);
    lv_obj_set_style_bg_opa(clock_row_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clock_row_, 0, 0);
    lv_obj_set_style_pad_all(clock_row_, 0, 0);
    lv_obj_set_style_pad_gap(clock_row_, 6, 0);
    lv_obj_set_flex_flow(clock_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(clock_row_, LV_OBJ_FLAG_SCROLLABLE);

    auto create_flip_card = [&](int w, int h, lv_obj_t** out_label) {
        lv_obj_t* card = lv_obj_create(clock_row_);
        lv_obj_set_size(card, w, h);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0xFFD3E2), 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* label = lv_label_create(card);
        lv_obj_center(label);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xF06292), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
        lv_label_set_text(label, "00");
        *out_label = label;
    };

    create_flip_card(66, 84, &hour_label_);
    create_flip_card(66, 84, &minute_label_);
    create_flip_card(66, 84, &second_label_);

    time_label_ = minute_label_;

    // 4) Info row container: weekday + date chips.
    info_row_ = lv_obj_create(idle_container_);
    lv_obj_set_size(info_row_, 220, 36);
    lv_obj_align(info_row_, LV_ALIGN_CENTER, 0, info_row_center_y);
    lv_obj_set_style_bg_opa(info_row_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_row_, 0, 0);
    lv_obj_set_style_pad_all(info_row_, 0, 0);
    lv_obj_set_style_pad_gap(info_row_, 8, 0);
    lv_obj_set_flex_flow(info_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(info_row_, LV_OBJ_FLAG_SCROLLABLE);

    // 4.1) Weekday chip
    weekday_chip_ = lv_obj_create(info_row_);
    lv_obj_set_size(weekday_chip_, 120, 32);
    lv_obj_set_style_bg_color(weekday_chip_, lv_color_hex(0xF06292), 0);
    lv_obj_set_style_bg_grad_color(weekday_chip_, lv_color_hex(0xF06292), 0);
    lv_obj_set_style_bg_grad_dir(weekday_chip_, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(weekday_chip_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(weekday_chip_, 16, 0);
    lv_obj_set_style_border_width(weekday_chip_, 0, 0);
    lv_obj_set_style_shadow_width(weekday_chip_, 0, 0);
    lv_obj_set_style_pad_all(weekday_chip_, 0, 0);
    lv_obj_clear_flag(weekday_chip_, LV_OBJ_FLAG_SCROLLABLE);

    weekday_label_ = lv_label_create(weekday_chip_);
    lv_obj_set_width(weekday_label_, 108);
    lv_label_set_long_mode(weekday_label_, LV_LABEL_LONG_CLIP);
    lv_obj_center(weekday_label_);
    lv_obj_set_style_text_color(weekday_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(weekday_label_, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_set_style_text_font(weekday_label_, &lv_font_montserrat_14, 0);
    lv_label_set_text(weekday_label_, "Thursday");

    // 4.2) Date chip
    date_chip_ = lv_obj_create(info_row_);
    lv_obj_set_size(date_chip_, 92, 32);
    lv_obj_set_style_bg_color(date_chip_, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_bg_grad_color(date_chip_, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_bg_grad_dir(date_chip_, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(date_chip_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(date_chip_, 16, 0);
    lv_obj_set_style_border_width(date_chip_, 0, 0);
    lv_obj_set_style_shadow_width(date_chip_, 0, 0);
    lv_obj_set_style_pad_all(date_chip_, 0, 0);
    lv_obj_clear_flag(date_chip_, LV_OBJ_FLAG_SCROLLABLE);

    date_label_ = lv_label_create(date_chip_);
    lv_obj_set_width(date_label_, 84);
    lv_label_set_long_mode(date_label_, LV_LABEL_LONG_CLIP);
    lv_obj_center(date_label_);
    lv_obj_set_style_text_font(date_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(date_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(date_label_, "12-03-26");

    // 5) Personalized greeting (replace progress bar area)
    greeting_label_ = lv_label_create(idle_container_);
    lv_obj_set_size(greeting_label_, 220, LV_SIZE_CONTENT);
    lv_obj_align(greeting_label_, LV_ALIGN_BOTTOM_MID, 0, greeting_bottom_y);
    lv_obj_set_style_text_font(greeting_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(greeting_label_, 1, 0);
    lv_obj_set_style_text_align(greeting_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(greeting_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(greeting_label_, 9000, 0);
    lv_label_set_recolor(greeting_label_, true);
    Settings wifi_settings("wifi", false);
    std::string child_name = wifi_settings.GetString("child_name", "Buddy");
    if (child_name.empty()) {
        child_name = "Buddy";
    }
    if (child_name.size() > 18) {
        child_name = child_name.substr(0, 18);
    }
    greeting_name_ = child_name;
    for (char& ch : greeting_name_) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    SelectRandomGreetingTemplate();
    std::string greeting_text = BuildGreetingText(0);
    lv_label_set_text(greeting_label_, greeting_text.c_str());
    bottom_line_text_cache_ = greeting_text;
    bottom_line_phase_cache_ = 0;

    // 6) Main menu controls directly on idle background (no extra white frame/title).
    menu_panel_ = lv_obj_create(idle_container_);
    lv_obj_set_size(menu_panel_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(menu_panel_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(menu_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu_panel_, 0, 0);
    lv_obj_set_style_pad_all(menu_panel_, 0, 0);
    lv_obj_set_style_radius(menu_panel_, 0, 0);
    lv_obj_clear_flag(menu_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);

    menu_stage_ = lv_obj_create(menu_panel_);
    lv_obj_set_size(menu_stage_, 206, 162);
    lv_obj_align(menu_stage_, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_set_style_bg_color(menu_stage_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(menu_stage_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu_stage_, 2, 0);
    lv_obj_set_style_border_color(menu_stage_, lv_color_hex(0xC8DFF5), 0);
    lv_obj_set_style_radius(menu_stage_, 14, 0);
    lv_obj_set_style_shadow_color(menu_stage_, lv_color_hex(0xA8C8E6), 0);
    lv_obj_set_style_shadow_width(menu_stage_, 10, 0);
    lv_obj_set_style_shadow_opa(menu_stage_, LV_OPA_30, 0);
    lv_obj_set_style_pad_hor(menu_stage_, 10, 0);
    lv_obj_set_style_pad_ver(menu_stage_, 7, 0);
    lv_obj_clear_flag(menu_stage_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(menu_stage_, LV_OBJ_FLAG_HIDDEN);

    menu_title_ = lv_label_create(menu_stage_);
    lv_obj_set_width(menu_title_, 188);
    lv_obj_align(menu_title_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(menu_title_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(menu_title_, lv_color_hex(0x2E6FB0), 0);
    lv_obj_set_style_text_align(menu_title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(menu_title_, "Setting");

    for (size_t i = 0; i < picker_row_labels_.size(); ++i) {
        picker_row_labels_[i] = lv_label_create(menu_stage_);
        lv_obj_set_width(picker_row_labels_[i], 188);
        lv_obj_align(picker_row_labels_[i], LV_ALIGN_TOP_LEFT, 0, 42 + static_cast<int>(i) * 20);
        lv_obj_set_style_text_font(picker_row_labels_[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(picker_row_labels_[i], lv_color_hex(0x3A6289), 0);
        lv_obj_set_style_text_align(picker_row_labels_[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(picker_row_labels_[i], LV_LABEL_LONG_CLIP);
        lv_label_set_recolor(picker_row_labels_[i], true);
        lv_obj_set_style_anim_duration(picker_row_labels_[i], 12000, 0);
        lv_obj_add_flag(picker_row_labels_[i], LV_OBJ_FLAG_HIDDEN);
    }

#if defined(CONFIG_SD_CARD_ENABLE) && CONFIG_SD_CARD_ENABLE
    constexpr bool kSdEnabled = true;
#else
    constexpr bool kSdEnabled = false;
#endif
#if defined(CONFIG_USE_ALARM) && CONFIG_USE_ALARM
    constexpr bool kAlarmEnabled = true;
#else
    constexpr bool kAlarmEnabled = false;
#endif
    const bool disable_sd_menu = is_luxiaoban_154 || !kSdEnabled;
    const bool disable_alarm_menu = is_luxiaoban_154 || !kAlarmEnabled;
    static const char* kMenuLabels[6] = {
        "AI Talk", "Music", "Online",
        "Radio", "Alarm", "Setting"
    };
    static const char* kMenuIcons[6] = {
        FONT_AWESOME_MICROCHIP_AI,
        FONT_AWESOME_SD_CARD,
        FONT_AWESOME_MUSIC,
        FONT_AWESOME_VOLUME_HIGH,
        FONT_AWESOME_BELL,
        FONT_AWESOME_GEAR
    };

    for (int i = 0; i < 6; ++i) {
        lv_obj_t* row = lv_obj_create(menu_panel_);
        menu_rows_[i] = row;
        lv_obj_set_size(row, 66, 70);
        int col = i % 3;
        int grid_row = i / 3;
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 13 + col * 74, 106 + grid_row * 78);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0xD6E4F2), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_grad_color(row, lv_color_hex(0xF7FBFF), 0);
        lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_color(row, lv_color_hex(0xA9C3DC), 0);
        lv_obj_set_style_shadow_width(row, 8, 0);
        lv_obj_set_style_shadow_opa(row, LV_OPA_40, 0);
        lv_obj_set_style_shadow_ofs_y(row, 2, 0);
        lv_obj_set_style_transform_zoom(row, 256, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        static const lv_style_prop_t kAnimProps[] = {
            LV_STYLE_BG_COLOR,
            LV_STYLE_BG_GRAD_COLOR,
            LV_STYLE_BORDER_COLOR,
            LV_STYLE_SHADOW_WIDTH,
            LV_STYLE_SHADOW_OPA,
            0
        };
        static lv_style_transition_dsc_t kTransition;
        static bool transition_inited = false;
        if (!transition_inited) {
            lv_style_transition_dsc_init(&kTransition, kAnimProps, lv_anim_path_ease_out, 180, 0, nullptr);
            transition_inited = true;
        }
        lv_obj_set_style_transition(row, &kTransition, 0);

        lv_obj_t* icon = lv_label_create(row);
        menu_icon_labels_[i] = icon;
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_text_font(icon, &font_awesome_30_4, 0);
        lv_obj_set_style_text_opa(icon, LV_OPA_COVER, 0);
        const char* menu_icon = kMenuIcons[i];
        const char* menu_label = kMenuLabels[i];
        if (disable_sd_menu && i == static_cast<int>(MainMenuItem::SdMusic)) {
            menu_icon = FONT_AWESOME_MUSIC;
            menu_label = "Online";
        }
        if (disable_alarm_menu && i == static_cast<int>(MainMenuItem::AlarmClock)) {
            menu_icon = FONT_AWESOME_MICROCHIP_AI;
            menu_label = "Info";
        }
        lv_label_set_text(icon, menu_icon);

        lv_obj_t* text = lv_label_create(row);
        menu_text_labels_[i] = text;
        lv_obj_set_width(text, 62);
        lv_obj_align(text, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_text_font(text, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(text, menu_label);
    }

    menu_hint_label_ = lv_label_create(menu_stage_);
    lv_obj_set_width(menu_hint_label_, 188);
    lv_obj_align(menu_hint_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_font(menu_hint_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(menu_hint_label_, lv_color_hex(0x3A6289), 0);
    lv_obj_set_style_text_align(menu_hint_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(menu_hint_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(menu_hint_label_, true);
    lv_obj_add_flag(menu_hint_label_, LV_OBJ_FLAG_HIDDEN);

    menu_wifi_guide_label_ = lv_label_create(menu_panel_);
    lv_obj_set_width(menu_wifi_guide_label_, 228);
    lv_obj_align(menu_wifi_guide_label_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_font(menu_wifi_guide_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(menu_wifi_guide_label_, lv_color_hex(0x2E6FB0), 0);
    lv_obj_set_style_text_align(menu_wifi_guide_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(menu_wifi_guide_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(menu_wifi_guide_label_, 15000, 0);
    lv_obj_add_flag(menu_wifi_guide_label_, LV_OBJ_FLAG_HIDDEN);

    menu_visible_ = false;
    menu_index_ = 0;
    picker_visible_ = false;
    UpdateMainMenuStyles();

    ApplyThemeStyles();

    ESP_LOGI(TAG, "Idle screen UI created (static flip-look balanced style)");
}

void IdleScreen::UpdateMainMenuStyles() {
    static const uint32_t kIconColors[6] = {
        0xF59E0B, // AI Talk (orange)
        0x22A447, // Music (green)
        0x1E7BFF, // Online (blue)
        0x8A46E8, // Radio (purple)
        0xE53935, // Alarm (red)
        0x6B7280  // Setting (gray)
    };
    static const uint32_t kSelectedBg[6] = {
        0xFFE8C2, // AI highlight
        0xDDF6E6, // Music highlight
        0xDFEDFF, // Online highlight
        0xEADFFF, // Radio highlight
        0xFFE1E1, // Alarm highlight
        0xECEFF4  // Setting highlight
    };
    static const uint32_t kSelectedBorder[6] = {
        0xF59E0B, // AI
        0x22A447, // Music
        0x1E7BFF, // Online
        0x8A46E8, // Radio
        0xE53935, // Alarm
        0x6B7280  // Setting
    };
    static const uint32_t kSelectedShadow[6] = {
        0xF9CB7A, // AI
        0xA9E0B9, // Music
        0xAFCFFF, // Online
        0xD4BEFF, // Radio
        0xFFC0C0, // Alarm
        0xC7CFD9  // Setting
    };
    static const uint32_t kSelectedTextColors[6] = {
        0x9A5800, // AI
        0x1A7A35, // Music
        0x165DBF, // Online
        0x6532B3, // Radio
        0xB02D2A, // Alarm
        0x49515E  // Setting
    };
    for (int i = 0; i < 6; ++i) {
        if (menu_rows_[i] == nullptr) {
            continue;
        }
        const bool selected = (i == menu_index_);
        lv_obj_set_style_bg_color(menu_rows_[i], selected ? lv_color_hex(kSelectedBg[i]) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_grad_color(menu_rows_[i], selected ? lv_color_hex(kSelectedBg[i]) : lv_color_hex(0xF7FBFF), 0);
        lv_obj_set_style_bg_opa(menu_rows_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(menu_rows_[i], selected ? 2 : 1, 0);
        lv_obj_set_style_border_color(menu_rows_[i], selected ? lv_color_hex(kSelectedBorder[i]) : lv_color_hex(0xD6E4F2), 0);
        lv_obj_set_style_shadow_color(menu_rows_[i], selected ? lv_color_hex(kSelectedShadow[i]) : lv_color_hex(0xA9C3DC), 0);
        lv_obj_set_style_shadow_width(menu_rows_[i], selected ? 12 : 8, 0);
        lv_obj_set_style_shadow_opa(menu_rows_[i], selected ? LV_OPA_60 : LV_OPA_40, 0);
        lv_obj_set_style_shadow_ofs_y(menu_rows_[i], selected ? 3 : 2, 0);
        lv_obj_set_style_transform_zoom(menu_rows_[i], selected ? 266 : 256, 0);
        lv_obj_set_style_outline_width(menu_rows_[i], 0, 0);
        lv_obj_set_style_outline_opa(menu_rows_[i], LV_OPA_TRANSP, 0);

        bool disabled = false;
        if (Application::GetInstance().IsOfflineMode()) {
            if (i == static_cast<int>(MainMenuItem::AiTalk) ||
                i == static_cast<int>(MainMenuItem::OnlineMusic) ||
                i == static_cast<int>(MainMenuItem::Radio) ||
                i == static_cast<int>(MainMenuItem::AlarmClock)) {
                disabled = true;
            }
        }

        lv_obj_set_style_text_color(menu_icon_labels_[i], disabled ? lv_color_hex(0xA0A0A0) : lv_color_hex(kIconColors[i]), 0);
        lv_obj_set_style_text_color(menu_text_labels_[i], lv_color_hex(selected ? (disabled ? 0xA0A0A0 : kSelectedTextColors[i]) : (disabled ? 0xA0A0A0 : 0x2C5377)), 0);
    }
}

void IdleScreen::ApplyThemeStyles() {
    auto palette = GetThemePalette(current_theme_);
    if (idle_container_ != nullptr) {
        lv_obj_set_style_bg_color(idle_container_, lv_color_hex(palette.bg_top), 0);
        lv_obj_set_style_bg_grad_color(idle_container_, lv_color_hex(palette.bg_bottom), 0);
    }
    if (logo_img_ != nullptr) {
        lv_obj_set_style_text_color(logo_img_, lv_color_hex(palette.logo), 0);
    }
    if (slogan_label_ != nullptr) {
        lv_obj_set_style_text_color(slogan_label_, lv_color_hex(palette.slogan), 0);
    }
    if (hour_label_ != nullptr) {
        auto card = lv_obj_get_parent(hour_label_);
        lv_obj_set_style_bg_color(card, lv_color_hex(palette.card_bg), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(palette.card_border), 0);
        lv_obj_set_style_text_color(hour_label_, lv_color_hex(palette.time_text), 0);
    }
    if (minute_label_ != nullptr) {
        auto card = lv_obj_get_parent(minute_label_);
        lv_obj_set_style_bg_color(card, lv_color_hex(palette.card_bg), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(palette.card_border), 0);
        lv_obj_set_style_text_color(minute_label_, lv_color_hex(palette.time_text), 0);
    }
    if (second_label_ != nullptr) {
        auto card = lv_obj_get_parent(second_label_);
        lv_obj_set_style_bg_color(card, lv_color_hex(palette.card_bg), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(palette.card_border), 0);
        lv_obj_set_style_text_color(second_label_, lv_color_hex(palette.time_text), 0);
    }
    if (weekday_chip_ != nullptr) {
        lv_obj_set_style_bg_color(weekday_chip_, lv_color_hex(palette.weekday_chip_bg), 0);
        lv_obj_set_style_bg_grad_color(weekday_chip_, lv_color_hex(palette.weekday_chip_bg), 0);
        lv_obj_set_style_bg_grad_dir(weekday_chip_, LV_GRAD_DIR_NONE, 0);
    }
    if (date_chip_ != nullptr) {
        lv_obj_set_style_bg_color(date_chip_, lv_color_hex(palette.date_chip_bg), 0);
        lv_obj_set_style_bg_grad_color(date_chip_, lv_color_hex(palette.date_chip_bg), 0);
        lv_obj_set_style_bg_grad_dir(date_chip_, LV_GRAD_DIR_NONE, 0);
    }
    if (greeting_label_ != nullptr) {
        lv_obj_set_style_text_color(greeting_label_, lv_color_hex(palette.greeting_text), 0);
    }
}

void IdleScreen::DestroyIdleScreenUI() {
    if (idle_container_ != nullptr) {
        DisplayLockGuard lock(display_);
        // 删除容器会自动删除所有子对象
        lv_obj_del(idle_container_);
        idle_container_ = nullptr;
        background_img_ = nullptr;
        logo_img_ = nullptr;
        slogan_label_ = nullptr;
        time_label_ = nullptr;
        weekday_label_ = nullptr;
        date_label_ = nullptr;
        greeting_label_ = nullptr;
        wifi_icon_label_ = nullptr;
        battery_icon_label_ = nullptr;
        hour_label_ = nullptr;
        minute_label_ = nullptr;
        second_label_ = nullptr;
        clock_row_ = nullptr;
        info_row_ = nullptr;
        weekday_chip_ = nullptr;
        date_chip_ = nullptr;
        menu_panel_ = nullptr;
        menu_stage_ = nullptr;
        menu_title_ = nullptr;
        menu_hint_label_ = nullptr;
        menu_wifi_guide_label_ = nullptr;
        picker_row_labels_ = {};
        menu_rows_ = {};
        menu_icon_labels_ = {};
        menu_text_labels_ = {};
        last_hms_ = {-1, -1, -1};
        greeting_name_.clear();
        greeting_mode_cache_ = true;
        bottom_line_phase_cache_ = -1;
        bottom_line_text_cache_.clear();
        menu_visible_ = false;
        menu_index_ = 0;
        picker_visible_ = false;
        picker_title_.clear();
        picker_value_.clear();
        ESP_LOGI(TAG, "Idle screen UI destroyed");
    }
}

void IdleScreen::UpdateDisplay() {
    if (!is_active_ || idle_container_ == nullptr) {
        return;
    }

    if (menu_visible_ && !picker_visible_) {
        auto now_tp = std::chrono::system_clock::now();
        auto menu_idle_secs = std::chrono::duration_cast<std::chrono::seconds>(now_tp - last_activity_time_).count();
        if (menu_idle_secs >= kMainMenuTimeoutSeconds) {
            menu_visible_ = false;
            DisplayLockGuard lock(display_);
            if (menu_panel_ != nullptr) {
                lv_obj_add_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);
            }
            if (clock_row_ != nullptr) {
                lv_obj_clear_flag(clock_row_, LV_OBJ_FLAG_HIDDEN);
            }
            if (info_row_ != nullptr) {
                lv_obj_clear_flag(info_row_, LV_OBJ_FLAG_HIDDEN);
            }
            if (greeting_label_ != nullptr) {
                lv_obj_clear_flag(greeting_label_, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }
    }

    DisplayLockGuard lock(display_);

    // 获取当前时间
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Use static buffers to avoid temporary lifetime issues.
    static char hh_buf[8];
    static char mm_buf[8];
    static char ss_buf[8];
    static char date_buf[32];

    snprintf(hh_buf, sizeof(hh_buf), "%02d", timeinfo.tm_hour);
    snprintf(mm_buf, sizeof(mm_buf), "%02d", timeinfo.tm_min);
    snprintf(ss_buf, sizeof(ss_buf), "%02d", timeinfo.tm_sec);

    if (hour_label_ != nullptr && last_hms_[0] != timeinfo.tm_hour) {
        lv_label_set_text(hour_label_, hh_buf);
        last_hms_[0] = timeinfo.tm_hour;
    }
    if (minute_label_ != nullptr && last_hms_[1] != timeinfo.tm_min) {
        lv_label_set_text(minute_label_, mm_buf);
        last_hms_[1] = timeinfo.tm_min;
    }
    if (second_label_ != nullptr && last_hms_[2] != timeinfo.tm_sec) {
        lv_label_set_text(second_label_, ss_buf);
        last_hms_[2] = timeinfo.tm_sec;
    }

    if (greeting_label_ != nullptr && !greeting_name_.empty()) {
#if CONFIG_USE_ALARM
        auto& app = Application::GetInstance();
        auto* timer = app.GetGeneralTimer();
        std::vector<std::pair<time_t, std::string>> upcoming_alarms;
        const bool alarm_line_enabled = !IsLuxiaoban154Board();
        bool has_alarm = alarm_line_enabled &&
                         (timer != nullptr) &&
                         timer->GetUpcomingAlarmsToday(upcoming_alarms, 5);
        int phase = 0;
        std::string line_text;
        if (has_alarm) {
            // Continuous loop:
            // Greeting scroll 60s -> blank 5s -> Alarm scroll 60s -> blank 5s
            constexpr int kGreetingSec = 60;
            constexpr int kAlarmSec = 60;
            constexpr int kGapSec = 5;
            constexpr int kLoopSec = kGreetingSec + kGapSec + kAlarmSec + kGapSec;

            int cycle_slot = static_cast<int>(now % kLoopSec);
            if (cycle_slot < kGreetingSec) {
                phase = 0;
                line_text = BuildGreetingText(timeinfo.tm_sec);
            } else if (cycle_slot < (kGreetingSec + kGapSec)) {
                phase = 1;
                line_text = " ";
            } else if (cycle_slot < (kGreetingSec + kGapSec + kAlarmSec)) {
                phase = 2;
                const int alarm_slot = cycle_slot - (kGreetingSec + kGapSec);
                const int rotate_every_sec = 12; // 60s window / 5 alarms
                size_t alarm_index = static_cast<size_t>((alarm_slot / rotate_every_sec) % upcoming_alarms.size());
                line_text = BuildAlarmText(upcoming_alarms[alarm_index].first, upcoming_alarms[alarm_index].second);
            } else {
                phase = 3;
                line_text = " ";
            }
        } else {
            phase = 0;
            line_text = BuildGreetingText(timeinfo.tm_sec);
        }

        if (phase != bottom_line_phase_cache_ || line_text != bottom_line_text_cache_) {
            bottom_line_phase_cache_ = phase;
            bottom_line_text_cache_ = line_text;
            bool should_scroll = (phase == 0 || phase == 2);
            lv_label_set_long_mode(greeting_label_, should_scroll ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP);
            lv_label_set_text(greeting_label_, line_text.c_str());
        }
#else
        std::string line_text = BuildGreetingText(timeinfo.tm_sec);
        if (line_text != bottom_line_text_cache_) {
            bottom_line_phase_cache_ = 0;
            bottom_line_text_cache_ = line_text;
            lv_label_set_long_mode(greeting_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_label_set_text(greeting_label_, line_text.c_str());
        }
#endif
    }

    // Update date in DD-MM-YY format.
    strftime(date_buf, sizeof(date_buf), "%d-%m-%y", &timeinfo);
    lv_label_set_text(date_label_, date_buf);

    // 更新星期
    std::string weekday_str = GetWeekDay();
    lv_label_set_text(weekday_label_, weekday_str.c_str());

    auto& board = Board::GetInstance();
    if (wifi_icon_label_ != nullptr) {
        lv_label_set_text(wifi_icon_label_, board.GetNetworkStateIcon());
    }
    if (battery_icon_label_ != nullptr) {
        int battery_level = 0;
        bool charging = false;
        bool discharging = false;
        if (board.GetBatteryLevel(battery_level, charging, discharging)) {
            lv_label_set_text(battery_icon_label_, GetBatteryIconByLevel(battery_level, charging));
        }
    }

}


void IdleScreen::SelectRandomGreetingTemplate() {
    if (kGreetingTemplates.empty()) {
        greeting_template_ = "Hi, {name}! Let's Chat";
        return;
    }

    int next_index = static_cast<int>(esp_random() % kGreetingTemplates.size());
    if (kGreetingTemplates.size() > 1 &&
        greeting_template_index_ >= 0 &&
        next_index == greeting_template_index_) {
        next_index = (next_index + 1) % kGreetingTemplates.size();
    }
    greeting_template_index_ = next_index;
    greeting_template_ = kGreetingTemplates[greeting_template_index_];
}

std::string IdleScreen::BuildGreetingText(int second) const {
    (void)second;
    std::string line = greeting_template_;
    const std::string placeholder = "{name}";
    size_t pos = line.find(placeholder);
    if (pos != std::string::npos) {
        line.replace(pos, placeholder.size(), "#" + std::string("FFF176 ") + greeting_name_ + "#");
    }
    return line;
}

std::string IdleScreen::BuildAlarmText(time_t trigger_time, const std::string& message) const {
    struct tm tm_buf;
    localtime_r(&trigger_time, &tm_buf);
    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "%H:%M", &tm_buf);

    std::string alarm_text = FONT_AWESOME_CLOCK;
    alarm_text += " ";
    alarm_text += message.empty() ? "Alarm" : message;
    alarm_text += " @ ";
    alarm_text += time_buf;
    return alarm_text;
}

void IdleScreen::AnimateFlip(lv_obj_t* label, const char* new_text) {
    if (label == nullptr || new_text == nullptr) {
        return;
    }

    lv_coord_t base_y = lv_obj_get_y(label);
    lv_label_set_text(label, new_text);
    lv_obj_set_y(label, base_y - 10);
    lv_obj_set_style_text_opa(label, LV_OPA_0, 0);

    lv_anim_t anim_y;
    lv_anim_init(&anim_y);
    lv_anim_set_var(&anim_y, label);
    lv_anim_set_values(&anim_y, base_y - 10, base_y);
    lv_anim_set_time(&anim_y, 160);
    lv_anim_set_exec_cb(&anim_y, [](void* obj, int32_t v) {
        lv_obj_set_y(static_cast<lv_obj_t*>(obj), v);
    });
    lv_anim_start(&anim_y);

    lv_anim_t anim_opa;
    lv_anim_init(&anim_opa);
    lv_anim_set_var(&anim_opa, label);
    lv_anim_set_values(&anim_opa, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&anim_opa, 160);
    lv_anim_set_exec_cb(&anim_opa, [](void* obj, int32_t v) {
        lv_obj_set_style_text_opa(static_cast<lv_obj_t*>(obj), v, 0);
    });
    lv_anim_start(&anim_opa);
}

const char* IdleScreen::GetBatteryIconByLevel(int level, bool charging) const {
    if (charging) {
        return FONT_AWESOME_BATTERY_BOLT;
    }
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    static const char* levels[] = {
        FONT_AWESOME_BATTERY_EMPTY,
        FONT_AWESOME_BATTERY_QUARTER,
        FONT_AWESOME_BATTERY_HALF,
        FONT_AWESOME_BATTERY_THREE_QUARTERS,
        FONT_AWESOME_BATTERY_FULL,
        FONT_AWESOME_BATTERY_FULL,
    };
    return levels[level / 20];
}

std::string IdleScreen::GetCurrentTime() {
    time_t now;
    struct tm timeinfo;
    char strftime_buf[64];
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // 格式化时间：13:45
    strftime(strftime_buf, sizeof(strftime_buf), "%H:%M", &timeinfo);
    
    return std::string(strftime_buf);
}

std::string IdleScreen::GetCurrentDate() {
    time_t now;
    struct tm timeinfo;
    char strftime_buf[64];
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // 格式化日期：2025-01-14
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d", &timeinfo);
    
    return std::string(strftime_buf);
}

std::string IdleScreen::GetWeekDay() {
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    const char* weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    
    std::string weekday = weekdays[timeinfo.tm_wday];
    // ESP_LOGI(TAG, "Current weekday: %s (wday=%d)", weekday.c_str(), timeinfo.tm_wday);
    
    return weekday;
}

#else // CONFIG_ENABLE_IDLE_SCREEN 未定义 - 提供空实现

// ============= 禁用待机界面功能时的空实现 =============

IdleScreen::IdleScreen(Display* display)
    : display_(display),
      is_active_(false),
      is_enabled_(false),
      current_theme_(Theme::Pink),
      last_activity_time_(std::chrono::system_clock::now()),
      idle_container_(nullptr),
      background_img_(nullptr),
      logo_img_(nullptr),
      slogan_label_(nullptr),
      time_label_(nullptr),
      weekday_label_(nullptr),
      date_label_(nullptr),
      greeting_label_(nullptr),
      wifi_icon_label_(nullptr),
      battery_icon_label_(nullptr),
      hour_label_(nullptr),
      minute_label_(nullptr),
      second_label_(nullptr),
      clock_row_(nullptr),
      info_row_(nullptr),
      weekday_chip_(nullptr),
      date_chip_(nullptr),
      menu_panel_(nullptr),
      menu_stage_(nullptr),
      menu_title_(nullptr),
      menu_hint_label_(nullptr),
      picker_row_labels_{},
      menu_rows_{},
      menu_icon_labels_{},
      menu_text_labels_{},
      last_hms_{-1, -1, -1},
      greeting_name_(),
      greeting_template_("Hi, {name}! Let's Chat"),
      greeting_template_index_(-1),
      greeting_mode_cache_(true),
      bottom_line_phase_cache_(-1),
      bottom_line_text_cache_() {
    ESP_LOGI(TAG, "IdleScreen feature is disabled (CONFIG_ENABLE_IDLE_SCREEN not set)");
}

IdleScreen::~IdleScreen() {}

void IdleScreen::Start() {}

void IdleScreen::Stop() {}

void IdleScreen::ResetTimer() {}

void IdleScreen::ToggleTheme() {}

void IdleScreen::SetTheme(Theme) {}

void IdleScreen::PreviewTheme(Theme) {}

void IdleScreen::RestoreSavedTheme() {}

bool IdleScreen::ToggleMainMenu() { return false; }

bool IdleScreen::MoveMainMenu(int) { return false; }

IdleScreen::MainMenuItem IdleScreen::GetMainMenuItem() const { return MainMenuItem::AiTalk; }

bool IdleScreen::SelectMainMenuItem() { return false; }

void IdleScreen::SetMainMenuSelectCallback(std::function<void(MainMenuItem)>) {}

bool IdleScreen::SetMainMenuPicker(const std::string&, const std::string&) { return false; }

void IdleScreen::ClearMainMenuPicker() {}

void IdleScreen::CheckIdleTimeout() {}

void IdleScreen::ShowIdleScreen() {}

void IdleScreen::HideIdleScreen() {}

void IdleScreen::UpdateDisplay() {}

void IdleScreen::CreateIdleScreenUI() {}

void IdleScreen::DestroyIdleScreenUI() {}

std::string IdleScreen::GetCurrentTime() { return "00:00"; }

std::string IdleScreen::GetCurrentDate() { return "2025-01-01"; }

std::string IdleScreen::GetWeekDay() { return ""; }

void IdleScreen::ApplyThemeStyles() {}

void IdleScreen::UpdateMainMenuStyles() {}

std::string IdleScreen::BuildGreetingText(int) const { return ""; }

std::string IdleScreen::BuildAlarmText(time_t, const std::string&) const { return ""; }

void IdleScreen::SelectRandomGreetingTemplate() {}

#endif // CONFIG_ENABLE_IDLE_SCREEN
