# Jiuchuan Optimization Notes

Last updated: 2026-05-02
Target: `sdkconfig.jiuchuan` (board `jiuchuan-s3`)

## Release Notes (2026-05-01)
- Packaging target:
  - `2.1.3-Final-May01`
  - Assets: `main/Pro-Snoopy-Hitelly.bin`
- Output package names:
  - `Globy-RabbitPro-2.1.3-Final-May01.bin` (merged flash image)
  - `Globy-RabbitPro-2.1.3-Final-May01-OTA.bin` (application OTA image)
- LCD default/rescue policy for Jiuchuan:
  - Default driver is **new screen** (`JD9853`, `lcd_driver=2`).
  - If old-screen device shows wrong display, user can **double-click power button**
    to force old driver (`GC9301`, `lcd_driver=1`) and reboot for recovery.

## 1) Radio

### Summary
- Added support for both direct stream URLs (AAC/MP3) and HLS `.m3u8`.
- Improved station switching behavior (next/prev) and runtime stability.
- Added fallback/skip behavior when a station fails.

### Key updates
- HLS support in `main/features/music/esp32_radio.cc`:
  - Playlist fetch and parsing (`.m3u8`).
  - Relative URL resolution for variants/segments.
  - Segment streaming to audio buffer.
- MPEG-TS segment handling for HLS:
  - TS re-sync logic for non-188-aligned buffers.
  - PAT/PMT parsing to locate AAC PID.
  - PES payload extraction + ADTS frame push to decoder.
- HLS polling/backoff tuning:
  - Parse `#EXT-X-TARGETDURATION`.
  - Adaptive wait intervals.
  - Reduced aggressive retry loops and transient error storms.
- Station control:
  - `NextStation()` / `PrevStation()` using station order list.
  - Skip-to-next with notification when current station fails.
- Switch safety:
  - Added station-switch lock + short debounce to avoid overlapping next/prev transitions.

### Jiuchuan button mapping
- Updated in `main/boards/jiuchuan-s3/jiuchuan_dev_board.cc`:
  - Short press `Vol-` => Radio `Next`.
  - Short press `Vol+` => Radio `Prev`.
  - Long press `Vol-` / `Vol+` => Volume adjust.
- Added app bridge methods:
  - `IsRadioPlaybackMode()`
  - `RadioNextStationFromMenu()`
  - `RadioPrevStationFromMenu()`

---

## 2) SD Music

### Summary
- Hardened track switching flow to reduce crash risk and make next/prev smoother.
- Added synchronization around switch operations and stricter stop/start gating.
- Added natural sorting for playlist order (numeric prefixes play ascending).

### Key updates
- `main/features/music/audio_stream_player.cc`:
  - Safer teardown model for stream tasks.
  - Avoid force cleanup while tasks are still alive.
  - `StartStream()` now aborts if previous stream shutdown is incomplete.
  - Static task buffers/stacks cleanup moved to destructor-safe path.
- `main/features/music/esp32_sd_music.cc` and `.h`:
  - Added track-switch mutex for serialized `Play/Next/Prev`.
  - Removed switch debounce that blocked valid next/prev events.
  - `Play()` now respects `StopStream()` result before opening next track.
  - `Next()` / `Prev()` use guarded switch path to avoid overlap.
  - Added natural sort for scanned/loaded playlists:
    - Numeric-aware compare (`001`, `002`, `010`).
    - Works with mixed names like `lv01-066`, `lv01-067`.
    - Sort key based on relative path, so files inside folder play in ascending order.

### Expected behavior impact
- Better stability when pressing next after long playback sessions.
- Reduced chance of race conditions during stop/start transitions.
- Smoother user-perceived next/prev behavior under repeated input.

---

## 3) Next sections (to be filled)

### 3.1 Alarm Clock

#### Summary
- Alarm now preempts active media playback cleanly.
- When alarm is dismissed, previous media is restored automatically.
- Alarm auto-stops after 5 minutes if unattended.

#### Key updates
- `main/application.h` + `main/application.cc`:
  - Added alarm media snapshot/restore flow:
    - Snapshot before entering `kDeviceStateAlarm`.
    - Stop current media so alarm ring can play clearly.
    - Restore media when alarm ends.
  - Restored media types:
    - Radio: restore by current station name.
    - SD Music: restore by saved track index.
  - Added alarm timeout watchdog:
    - If alarm rings continuously for more than 300s, alarm is auto-cleared.
- `main/features/alarm_clock/general_timer.cc`:
  - `ClearRinging()` path aligned so power-button dismiss and timeout dismiss both resume media consistently.
- `main/features/music/esp32_radio.h` + `main/features/music/esp32_radio.cc`:
  - Fixed crash when dismissing alarm and restoring radio.
  - Root cause: `OnPlaybackFinishedAndContinue()` called `StartStream()` directly in playback task context, causing task self-wait/deadlock and WDT panic.
  - Fix: moved radio auto-reconnect to async task (`ReconnectTaskEntry`) and added `reconnect_pending_` guard to prevent reconnect overlap.

#### Behavior after update
- If user is listening to radio/SD music and alarm triggers:
  - Media stops temporarily.
  - Alarm ring plays.
- If user presses power button to stop alarm:
  - Alarm stops.
  - Previous media resumes automatically.
- If user is away:
  - Alarm stops automatically after 5 minutes.
- If alarm stops while radio is active:
  - Radio restore/reconnect runs asynchronously and avoids playback-task deadlock/WDT crash.

#### Validation status (2026-05-01)
- Verified in monitor logs:
  - Alarm preempts radio correctly.
  - Alarm dismiss returns to `idle`, then restores previous radio station.
  - No immediate `Interrupt wdt timeout on CPU0` in patched flow.

### 3.2 UI / Menu Flow
- Main-menu activation notification on `jiuchuan-s3` now shows short function
  labels instead of long text that could break layout:
  - `AI Talk`, `Music`, `Online`, `Radio`, `Alarm`, `Setting`
- Implementation:
  - Added `Application::GetSelectedMainMenuLabel()`
  - `main/boards/jiuchuan-s3/jiuchuan_dev_board.cc` uses this label when user
    presses power to activate a selected menu item.

### 3.3 Power / Audio Pipeline
- Pending updates...

### 3.4 Performance / Memory
- Online music stop-path hardened to match radio stop responsiveness.
- `StopStream()` no longer blocks for long waits when user presses power during
  online playback:
  - wait window reduced from long polling to short bounded wait
    (`10 x 20ms` per task).
  - if source/play task still stuck, force-delete task handle to avoid UI freeze.
- TCP teardown hardening (`managed_components/78__esp-ml307/src/esp/esp_tcp.cc`):
  - disable callbacks early during `Disconnect()` to avoid late callback races.
  - if receive-task wait timeout occurs, force-delete receive task.
  - prevent stale task/event-group interactions after media stop.

### 3.5 Online Music Stability (Power Key)

#### Symptom
- While online music is playing, pressing power had higher lag than radio and
  could lead to instability after repeated stop/start cycles.

#### Root cause
- Online path relied on slower blocking shutdown and TCP receive-task exit
  timing, unlike radio’s simpler quick stop path.
- Timeout log observed repeatedly:
  - `EspTcp: Failed to wait for receive task exit`

#### Fix applied
- Fast-stop behavior aligned with radio UX:
  - immediate transition to stopping state
  - bounded short wait
  - controlled force cleanup on timeout
- Additional TCP disconnect safeguards to prevent task/callback race after stop.

#### Result
- Power-key stop during online playback returns to home much faster and with
  lower crash risk in repeated usage.

## 5) Wi-Fi Setup QR UI (Jiuchuan)

### Summary
- Wi-Fi setup text for `jiuchuan-s3` now follows Luxiaoban content style.
- Bottom instruction line in QR screen changed to one-line marquee and moved
  upward to avoid clipping on rounded display corners.

### Key updates
- `main/boards/common/wifi_board.cc`
  - `jiuchuan-s3` reuses Luxiaoban Wi-Fi setup wording for title/hint/QR text.
- `main/features/QRCode/qrcode_display.cc`
  - Bottom text uses one-line scrolling mode (`LV_LABEL_LONG_SCROLL_CIRCULAR`).
  - Vertical offset raised for Jiuchuan LCD to avoid bottom cut-off.
  - Marquee speed tuned slower for readability.

---

## 4) OTA & Time Sync

### Summary
- Fixed system time drift/wrong-hour issue from OTA `server_time`.
- Split OTA into 2 independent flows:
  - Default flow: always used for time sync and base system/protocol config.
  - Custom flow: used only for firmware update checks (e.g. Globy OTA).

### Key updates
- `main/ota.cc`:
  - `server_time.timestamp` is now treated as UTC epoch milliseconds.
  - Removed extra `timezone_offset` addition before `settimeofday()`
    (this was causing local time to shift incorrectly).
  - `GetCheckVersionUrl()` now reads `wifi.ota_url_default` first,
    fallback to `CONFIG_OTA_URL`.
- `main/application.cc`:
  - In `CheckNewVersion()`:
    - First call always checks default endpoint (`CONFIG_OTA_URL` or board-specific default path).
    - Second call checks custom endpoint from `wifi.ota_url_custom` (fallback legacy `wifi.ota_url`) only for firmware updates.
    - Custom OTA failures no longer break default time/system flow.
- `components/TienHuyIoT_esp-wifi-connect/wifi_configuration_ap.cc`:
  - Added NVS key separation:
    - `ota_url_default` for default flow.
    - `ota_url_custom` for custom update flow.
  - Kept backward compatibility:
    - Legacy `ota_url` is migrated to `ota_url_custom`.
    - Legacy key still updated when saving from AP config UI.

### Behavior after update
- Device time should align correctly with timezone handling.
- OTA custom URL no longer overrides the default server used for core time/system bootstrap.
