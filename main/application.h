#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"
#include "esp32_sd_music.h"
#include "esp32_music.h"
#include "esp32_radio.h"
#if CONFIG_USE_ALARM
#include "features/alarm_clock/general_timer.h"
#endif
class AudioStreamPlayer;
class VideoPlayer;

// Forward declaration for MusicVisualizer (owned by Application)
namespace music { class MusicVisualizer; struct MusicInfo; }
namespace spectrum { class SpectrumManager; }

// --- Display Weather ---
#include "display.h"
#include "features/weather/weather_service.h"
#include "features/weather/weather_model.h"
#if CONFIG_ENABLE_IDLE_SCREEN
#include "features/Idle_Screen/idle_screen.h"
#endif
// ---------------------

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(Ota& ota, const std::string& url = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    // 新增：接收外部音频数据（如音乐播放）
    void AddAudioData(AudioStreamPacket&& packet);
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
	Esp32Music* GetMusic() { return music_; }
	Esp32Radio* GetRadio() { return radio_; }
	Esp32SdMusic* GetSdMusic() { return sd_music_; }
	VideoPlayer* GetVideo() { return sd_video_; }
#if CONFIG_USE_ALARM
    GeneralTimer* GetGeneralTimer() { return general_timer_.get(); }
    void SetAlarmEvent() { alarm_event_active_ = true; }
    void ClearAlarmEvent() { alarm_event_active_ = false; }
    bool HasAlarmEvent() const { return alarm_event_active_; }
    void OnAlarmDismissed();
#endif

    /** Get the music visualizer (owned by Application). */
    music::MusicVisualizer* GetMusicVisualizer() { return music_visualizer_.get(); }

    /* ================================================================== */
    /*  Media Player APIs                                                 */
    /* ================================================================== */

    /**
     * @brief Play online music by song name.
     * @param song_name   Song name to search for
     * @param artist_name Optional artist name filter
     * @return true if playback started
     */
    bool PlayMusic(const std::string& song_name, const std::string& artist_name = "");

    /**
     * @brief Play a radio station by name.
     * @param station_name Station name or key (e.g. "VOV1")
     * @return true if playback started
     */
    bool PlayRadio(const std::string& station_name);

    /**
     * @brief Play radio from custom URL.
     * @param url          Stream URL
     * @param station_name Optional display name
     * @return true if playback started
     */
    bool PlayRadioUrl(const std::string& url, const std::string& station_name = "");

    /**
     * @brief Play media from SD card (music or video).
     * @param keyword  File name or search keyword
     * @param is_video true to play as AVI video, false for audio
     * @return true if playback started
     */
    bool PlaySdMedia(const std::string& keyword, bool is_video = false);

    /**
     * @brief Play AVI video from SD card by full path.
     * @param file_path Absolute path to AVI file
     * @return true if playback started
     */
    bool PlayVideo(const std::string& file_path);

    /**
     * @brief Stop all media playback (music, radio, SD music, video).
     */
    void StopAllMedia();

    /**
     * @brief Cycle classic idle-screen color theme.
     * @return true if idle screen exists and theme changed
     */
    bool ToggleIdleTheme();
    bool ToggleMainMenu();
    bool IsMainMenuVisible() const;
    bool MoveMainMenu(int delta);
    std::string GetSelectedMainMenuLabel() const;
    bool ActivateMainMenuSelection();
    bool IsSdMusicPlaybackMode() const;
    bool IsRadioPlaybackMode() const;
    bool SdMusicNextTrackFromMenu();
    bool SdMusicPrevTrackFromMenu();
    bool RadioNextStationFromMenu();
    bool RadioPrevStationFromMenu();
    bool ExitSdMusicPlaybackToMainMenu();
    bool IsOfflineMode() const { return offline_mode_; }

    /**
     * @brief Check if any media is currently playing.
     */
    bool IsMediaPlaying() const;

    /**
     * @brief Ensure device is in idle state before media playback.
     *
     * If the device is in Listening or Speaking state, toggles the chat
     * to transition back to Idle.  Blocks until the transition completes
     * (up to a configurable timeout).
     *
     * Must be called from outside the audio player — keeps media
     * components decoupled from Application state management.
     *
     * @return true if device is now in idle (or was already idle)
     */
    bool EnsureIdleForMedia();

    /**
     * @brief Setup FFT display callback for a given audio player.
     * Installs FFT data callback and state callback for FFT lifecycle.
     */
    void SetupAudioPlayerCallback(AudioStreamPlayer* player);

    /**
     * @brief Build a MusicInfo snapshot by auto-detecting the active player.
     * Called by MusicVisualizer's periodic callback to update the UI.
     */
    music::MusicInfo BuildMusicInfo();

    /* ================================================================== */
    /*  Component Initializers                                            */
    /* ================================================================== */

    /** Initialize online music player and register MCP tools. */
    bool InitMusic();

    /** Initialize internet radio player and register MCP tools. */
    bool InitRadio();

    /** Initialize SD card music player and register MCP tools. */
    bool InitSdMusic();

    /** Initialize SD card video player and register MCP tools. */
    bool InitVideo();
    bool InitAlarmClock();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    Esp32Music* music_ = nullptr;
    Esp32Radio* radio_ = nullptr;
    Esp32SdMusic* sd_music_ = nullptr;
    VideoPlayer* sd_video_ = nullptr;

    // Music spectrum visualizer (owned by Application, not Display)
    std::unique_ptr<music::MusicVisualizer> music_visualizer_;

    // OLED spectrum (simple bars, no music UI overlay)
    std::unique_ptr<spectrum::SpectrumManager> oled_spectrum_mgr_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;
    TaskHandle_t wifi_scan_refresh_task_handle_ = nullptr;
    TaskHandle_t sd_reload_task_handle_ = nullptr;
    bool sd_reload_in_progress_ = false;
    bool offline_mode_ = false;
#ifdef CONFIG_WEATHER_IDLE_DISPLAY_ENABLE
    TaskHandle_t weather_idle_task_handle_ = nullptr;
#endif
#if CONFIG_ENABLE_IDLE_SCREEN
    std::unique_ptr<IdleScreen> idle_screen_;
#endif
    enum class MenuPickerContext : uint8_t {
        None = 0,
        SdDirectory,
        SdTrack,
        RadioStation,
        SettingAction,
        LcdSettingAction,
        ThemeSettingAction,
        LcdSwitchConfirm,
        SdReloadConfirm,
        RestartConfirm,
        ShutdownConfirm,
        WifiAccessPoint,
        WifiAddConfirm,
        AlarmList,
        AlarmAction,
        AlarmEditTime,
        AlarmDeleteConfirm,
        PowerAction,
        PowerTimeoutAction,
        DeviceInfo,
        OtaSettingAction,
        WifiResetConfirm,
        SoundScreenAction,
        SystemAction,
        VolumeAdjust,
        BrightnessAdjust,
    };
    MenuPickerContext menu_picker_ctx_ = MenuPickerContext::None;
    std::vector<std::string> menu_picker_items_;
    std::vector<int> menu_picker_map_;
    std::vector<int> wifi_saved_rssi_cache_;
    bool wifi_saved_rssi_valid_ = false;
    int menu_picker_index_ = 0;
    bool keep_main_menu_open_after_picker_ = false;
    int original_theme_ = -1;

    /**
     * @brief Identifies which media component to exclude from stopping.
     * Used by StopOtherMedia() to skip the component about to play.
     */
    enum class MediaComponent : uint8_t {
        kNone     = 0,   ///< Stop all media (no exclusion)
        kMusic    = 1,   ///< Keep music, stop everything else
        kRadio    = 2,   ///< Keep radio, stop everything else
        kSdMusic  = 3,   ///< Keep SD music, stop everything else
        kVideo    = 4,   ///< Keep video, stop everything else
    };

    /**
     * @brief Stop all active media playback except the specified component.
     *
     * Centralized media teardown: conditionally stops music, radio,
     * SD music and video. Each component is stopped only when currently
     * active. Call with kNone (default) to stop everything.
     *
     * @param except  Component to skip (default: kNone = stop all)
     */
    void StopOtherMedia(MediaComponent except = MediaComponent::kNone);
    bool OpenMenuPicker(MenuPickerContext ctx, const std::string& title,
        const std::vector<std::string>& items, int initial_index = 0,
        const std::vector<int>& index_map = {});
    void CloseMenuPicker();
    bool RefreshMenuPickerDisplay();
    bool MoveMenuPicker(int delta);
    bool ActivateMenuPicker();
    bool StartAiTalkFromMenu();
    bool StartRandomSdMusicFromMenu();
    bool StartOnlineMusicPromptFromMenu();
    bool StartRandomRadioFromMenu();
    bool StartAlarmPromptFromMenu();
    bool StartDeviceInfoPromptFromMenu();
    bool OpenSdDirectoryPicker();
    bool OpenSdTrackPicker(const std::string& relative_dir);
    bool OpenRadioPicker();
    bool OpenSettingPicker();
    bool OpenLcdSettingPicker();
    bool OpenLcdSwitchConfirmPicker();
    bool OpenThemeSettingPicker();
    bool OpenOtaSettingPicker();
    bool OpenSoundScreenPicker();
    bool OpenSystemPicker();
    bool OpenVolumeAdjustPicker();
    bool OpenBrightnessAdjustPicker();
    std::string GetSdCardStorageString();
    bool OpenWifiResetConfirmPicker();
    void ApplyLiveThemeSelection();
    bool OpenSdReloadConfirmPicker();
    bool StartSdLibraryReloadTask();
    bool OpenAlarmListPicker();
    bool OpenAlarmActionPicker();
    bool OpenAlarmDeleteConfirmPicker();
    bool RefreshAlarmListCache();
    bool ApplySelectedAlarmAction();
    bool OpenRestartConfirmPicker();
    bool OpenShutdownConfirmPicker();
    bool OpenPowerPicker();
    bool OpenPowerTimeoutPicker();
    bool OpenDeviceInfoPicker();
    bool OpenWifiPickerFromSavedList();
    bool OpenWifiAddConfirmPicker();
    void RefreshSavedWifiRssiCache();
    bool ApplyWifiPickerSelection();
    void StartWifiScanRefreshTask();
    bool TriggerDeviceRestart();
    bool TriggerDeviceShutdown();
    bool TriggerWifiReconfigureMode();
    bool RestartWifiAndReconnectAsync();
    void SetSdMusicPlaybackMode(bool enabled);

    bool sd_music_playback_mode_ = false;
    std::string sd_selected_directory_;
    int pending_lcd_driver_ = 0;
    bool story_mode_active_ = false;
    bool waiting_alarm_from_ai_ = false;
    bool waiting_device_id_from_ai_ = false;

    bool AddAlarmEventAt(time_t trigger_time, const std::string& message, int repeat_seconds);
    void LoadDefaultAlarmsIfEmpty();
    bool TryCreateAlarmFromText(const std::string& text);

#if CONFIG_USE_ALARM
    std::unique_ptr<GeneralTimer> general_timer_;
    bool alarm_event_active_ = false;
    uint8_t alarm_repeat_interval_ticks_ = 0;
    time_t alarm_started_at_ = 0;
    struct AlarmMediaResumeState {
        bool active = false;
        bool had_radio = false;
        std::string radio_station;
        bool had_sd_music = false;
        int sd_track_index = -1;
    } alarm_media_resume_;

    struct AlarmMenuEntry {
        bool active = true;
        int timer_index = -1;
        int repeat_seconds = 0;
        int disabled_index = -1;
        time_t trigger_time = 0;
        std::string message;
    };
    struct DisabledAlarmEntry {
        time_t trigger_time = 0;
        int repeat_seconds = 0;
        std::string message;
    };
    std::vector<AlarmMenuEntry> alarm_menu_entries_;
    std::vector<DisabledAlarmEntry> disabled_alarm_entries_;
    AlarmMenuEntry alarm_action_entry_;
    bool alarm_action_entry_valid_ = false;
    int alarm_edit_hour_ = 0;
    int alarm_edit_minute_ = 0;

    void SnapshotAndPauseMediaForAlarm();
    void RestoreMediaAfterAlarm();
#endif

    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    void CheckAssetsVersion();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);

#ifdef CONFIG_WEATHER_IDLE_DISPLAY_ENABLE
    // --- Weather Info ---
    void StartWeatherIdleTask();
    void UpdateIdleDisplay();
    // -------------------
#endif
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
