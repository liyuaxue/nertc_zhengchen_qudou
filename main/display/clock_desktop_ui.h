#ifndef CLOCK_DESKTOP_UI_H
#define CLOCK_DESKTOP_UI_H

#include "lvgl_display.h"
#include "lvgl_theme.h"

#include <lvgl.h>
#include <esp_timer.h>
#include <string>

/**
 * 时钟桌面UI类
 * 独立代码，不混合现有显示代码
 * 功能：
 * 1. 正常显示状态栏
 * 2. 显示日期和时间（翻页式数字时钟）
 * 3. 显示天气信息
 * 4. 显示空气质量
 * 5. 背景支持兼容显示assets资源背景
 */
class ClockDesktopUI {
public:
    ClockDesktopUI(LvglDisplay* display);
    ~ClockDesktopUI();

    // 显示时钟桌面UI
    void Show();
    
    // 通过充电状态自动显示时钟UI（内部使用）
    void ShowByCharging();
    
    // 隐藏时钟桌面UI
    void Hide();
    
    // 更新UI（时间、日期等）
    void Update();
    
    // 设置主题（用于背景图片）
    void SetTheme(LvglTheme* theme);
    
    // 设置天气信息
    void SetWeather(const std::string& condition, int temp_high, int temp_low);
    
    // 设置天气图标（可选，使用Font Awesome图标字符串）
    void SetWeatherIcon(const char* icon);
    
    // 设置空气质量
    void SetAirQuality(const std::string& quality);
    
    // 设置摄像头预览状态（用于控制时钟UI是否允许显示）
    static void SetCameraPreviewActive(bool active);
    static bool IsCameraPreviewActive();

private:
    void CreateUI();
    void DestroyUI();
    void UpdateTime();
    void UpdateDate();
    void UpdateAlarm();
    static void TimerCallback(void* arg);
    static void DelayShowTimerCallback(void* arg);
    void CheckChargingAndStandbyState();
    
    LvglDisplay* display_;
    LvglTheme* theme_;
    
    // UI对象
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* hour_tens_label_ = nullptr;
    lv_obj_t* hour_ones_label_ = nullptr;
    lv_obj_t* minute_tens_label_ = nullptr;
    lv_obj_t* minute_ones_label_ = nullptr;
    lv_obj_t* colon_label_ = nullptr;
    lv_obj_t* weather_container_ = nullptr;
    lv_obj_t* weather_icon_label_ = nullptr;
    lv_obj_t* weather_text_label_ = nullptr;
    lv_obj_t* air_quality_label_ = nullptr;
    lv_obj_t* alarm_label_ = nullptr;  // 闹钟信息标签
    
    // 状态栏对象（复用现有状态栏）
    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    
    // 定时器
    esp_timer_handle_t update_timer_ = nullptr;
    esp_timer_handle_t delay_show_timer_ = nullptr;  // 延迟显示定时器（10秒）
    lv_timer_t* state_check_lv_timer_ = nullptr;      // 使用 LVGL 定时器进行状态检查
    
    // 数据
    std::string weather_condition_;
    int weather_temp_high_ = 0;
    int weather_temp_low_ = 0;
    std::string air_quality_;
    
    bool is_visible_ = false;
    bool is_charging_ = false;  // 当前充电状态
    bool delay_show_timer_started_ = false;  // 延迟显示定时器是否已启动
    bool auto_shown_by_charging_ = false;  // UI是否由充电状态自动显示
    
    // 静态标记位：摄像头预览是否正在运行
    static bool camera_preview_active_;
};

#endif // CLOCK_DESKTOP_UI_H

