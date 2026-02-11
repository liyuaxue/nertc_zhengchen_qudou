#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"
#include "lvgl_image.h"

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

class LvglDisplay : public Display {
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000) override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void UpdateVolume(int volume) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual bool SnapshotToJpeg(uint8_t*& jpeg_output_data, size_t& jpeg_output_size, int quality = 80);

    // 重置低电量弹窗标志位（例如充电或重启后调用）
    void ResetLowBatteryPopup();

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t *display_ = nullptr;

    lv_obj_t *network_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *notification_label_ = nullptr;
    lv_obj_t *mute_label_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;

    // Volume control UI
    lv_obj_t* volume_bar_container_ = nullptr;
    lv_obj_t* volume_bar_ = nullptr;
    lv_obj_t* volume_icon_label_ = nullptr;
    esp_timer_handle_t volume_timer_ = nullptr;
    
    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;

    // 低电量弹窗相关
    bool low_battery_state_ = false;
    esp_timer_handle_t low_battery_timer_ = nullptr;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) override = 0;
    virtual void Unlock() override = 0;
};


#endif
