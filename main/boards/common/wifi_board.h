#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include <cstdint>

#include "board.h"

class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;
    void* roaming_task_handle_ = nullptr;
    int64_t roaming_cooldown_until_us_ = 0;
    bool initial_roaming_probe_pending_ = true;
    void EnterWifiConfigMode();
    void StartRoamingMonitor();
    void RoamingLoop();
    static void RoamingTask(void* arg);
    virtual std::string GetBoardJson() override;

public:
    WifiBoard();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration();
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
};

#endif // WIFI_BOARD_H
