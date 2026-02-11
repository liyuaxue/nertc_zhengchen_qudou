#include "clock_desktop_ui.h"
#include "display.h"
#include "display/lcd_display.h"
#include "display/settings_page_ui.h"
#include "board.h"
#include "application.h"
#include "boards/zhengchen-qudou/alarm.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <ctime>
#include <cstring>
#include <font_awesome.h>
#include <lvgl.h>

// 声明时间字体
LV_FONT_DECLARE(time_font);

#define TAG "ClockDesktopUI"

// 静态成员变量定义
bool ClockDesktopUI::camera_preview_active_ = false;

// 星期名称（中文）
static const char* weekdays[] = {
    "周日", "周一", "周二", "周三", "周四", "周五", "周六"
};

ClockDesktopUI::ClockDesktopUI(LvglDisplay* display) 
    : display_(display), theme_(nullptr) {
    
    // 创建更新定时器（每秒更新一次）
    esp_timer_create_args_t timer_args = {
        .callback = TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_desktop_update",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &update_timer_));
    
    // 创建延迟显示定时器（10秒后显示）
    esp_timer_create_args_t delay_timer_args = {
        .callback = DelayShowTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_desktop_delay_show",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&delay_timer_args, &delay_show_timer_));
    
    // 使用 LVGL 定时器进行状态检查（每秒检查一次充电和待命状态）
    state_check_lv_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* ui = static_cast<ClockDesktopUI*>(lv_timer_get_user_data(timer));
            if (ui != nullptr) {
                ui->CheckChargingAndStandbyState();
            }
        },
        1000,  // 1 秒
        this
    );
}

ClockDesktopUI::~ClockDesktopUI() {
    Hide();
    
    if (update_timer_ != nullptr) {
        esp_timer_stop(update_timer_);
        esp_timer_delete(update_timer_);
    }
    
    if (delay_show_timer_ != nullptr) {
        esp_timer_stop(delay_show_timer_);
        esp_timer_delete(delay_show_timer_);
    }
    
    if (state_check_lv_timer_ != nullptr) {
        lv_timer_del(state_check_lv_timer_);
        state_check_lv_timer_ = nullptr;
    }
}

void ClockDesktopUI::Show() {
    if (is_visible_) {
        return;
    }
    
    // 检查摄像头预览是否正在运行，如果是则禁止显示时钟UI
    if (camera_preview_active_) {
        ESP_LOGI(TAG, "Camera preview is active, cannot show clock UI");
        return;
    }
    
    // 检查设置UI是否正在显示，如果是则禁止显示时钟UI
    auto* lcd_display = dynamic_cast<LcdDisplay*>(display_);
    if (lcd_display != nullptr) {
        auto* settings_ui = lcd_display->GetSettingsPageUI();
        if (settings_ui != nullptr && settings_ui->IsVisible()) {
            ESP_LOGI(TAG, "Settings UI is visible, cannot show clock UI");
            return;  
        }
    }
    
    DisplayLockGuard lock(display_);
    
    // 获取当前主题
    if (theme_ == nullptr) {
        theme_ = static_cast<LvglTheme*>(display_->GetTheme());
    }
    
    CreateUI();
    is_visible_ = true;
    
    // 启动定时器
    ESP_ERROR_CHECK(esp_timer_start_periodic(update_timer_, 1000000)); // 1秒
    
    // 立即更新一次
    Update();
}

void ClockDesktopUI::ShowByCharging() {
    // 标记为通过充电状态自动显示
    auto_shown_by_charging_ = true;
    Show();
}

void ClockDesktopUI::Hide() {
    if (!is_visible_) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    
    // 停止定时器
    if (update_timer_ != nullptr) {
        esp_timer_stop(update_timer_);
    }
    
    DestroyUI();
    is_visible_ = false;
    // 清除自动显示标记（因为UI已被隐藏）
    auto_shown_by_charging_ = false;
}

void ClockDesktopUI::Update() {
    if (!is_visible_) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    UpdateTime();
    UpdateDate();
    UpdateAlarm();
    
    // 更新状态栏（网络和电池图标）
    auto& board = Board::GetInstance();
    
    // 更新网络图标
    const char* network_icon = board.GetNetworkStateIcon();
    if (network_label_ != nullptr && network_icon != nullptr) {
        lv_label_set_text(network_label_, network_icon);
    }
    
    // 更新电池图标
    int battery_level;
    bool charging, discharging;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        const char* battery_icon = nullptr;
        if (charging) {
            battery_icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY,      // 0-19%
                FONT_AWESOME_BATTERY_QUARTER,    // 20-39%
                FONT_AWESOME_BATTERY_HALF,       // 40-59%
                FONT_AWESOME_BATTERY_THREE_QUARTERS, // 60-79%
                FONT_AWESOME_BATTERY_FULL,       // 80-99%
                FONT_AWESOME_BATTERY_FULL,       // 100%
            };
            battery_icon = levels[battery_level / 20];
        }
        if (battery_label_ != nullptr && battery_icon != nullptr) {
            lv_label_set_text(battery_label_, battery_icon);
        }
    }
}

void ClockDesktopUI::SetTheme(LvglTheme* theme) {
    theme_ = theme;
    
    if (!is_visible_ || theme_ == nullptr) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    
    auto text_font = theme_->text_font()->font();
    auto icon_font = theme_->icon_font()->font();
    auto large_icon_font = theme_->large_icon_font()->font();
    
    // 获取主题颜色
    lv_color_t bg_color = theme_->background_color();
    lv_color_t text_color = theme_->text_color();
    
    // 计算冒号容器背景色（比主背景稍亮）
    lv_color_t colon_bg_color = lv_color_lighten(bg_color, 30);  // 亮度增加30%
    
    // 设置背景图片或颜色
    if (theme_->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, theme_->background_image()->image_dsc(), 0);
        lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, bg_color, 0);
        lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    }
    
    // 更新状态栏样式
    if (status_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(status_bar_, bg_color, 0);
        lv_obj_set_style_text_color(status_bar_, text_color, 0);
    }
    
    // 更新文字颜色
    if (date_label_ != nullptr) {
        lv_obj_set_style_text_color(date_label_, text_color, 0);
    }
    if (alarm_label_ != nullptr) {
        lv_obj_set_style_text_color(alarm_label_, text_color, 0);
    }
    
    // 时间数字颜色（白色确保在灰色背景上清晰）
    if (hour_tens_label_ != nullptr) {
        lv_obj_set_style_text_color(hour_tens_label_, lv_color_hex(0xFFFFFF), 0);
    }
    if (hour_ones_label_ != nullptr) {
        lv_obj_set_style_text_color(hour_ones_label_, lv_color_hex(0xFFFFFF), 0);
    }
    if (minute_tens_label_ != nullptr) {
        lv_obj_set_style_text_color(minute_tens_label_, lv_color_hex(0xFFFFFF), 0);
    }
    if (minute_ones_label_ != nullptr) {
        lv_obj_set_style_text_color(minute_ones_label_, lv_color_hex(0xFFFFFF), 0);
    }
    
    // 冒号颜色 - 根据背景自动调整
    if (colon_label_ != nullptr) {
        // 检查背景色亮度，决定冒号颜色
        uint8_t bg_brightness = lv_color_brightness(bg_color);
        if (bg_brightness > 128) {
            // 背景较亮，使用深色冒号
            lv_obj_set_style_text_color(colon_label_, lv_color_hex(0x000000), 0);
        } else {
            // 背景较暗，使用浅色冒号
            lv_obj_set_style_text_color(colon_label_, lv_color_hex(0xFFFFFF), 0);
        }
    }
    
    // 天气和空气质量文字颜色
    if (weather_text_label_ != nullptr) {
        lv_obj_set_style_text_color(weather_text_label_, text_color, 0);
    }
    if (air_quality_label_ != nullptr) {
        // 空气质量标签保持独立颜色（绿色）
        lv_obj_set_style_text_color(air_quality_label_, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_bg_color(air_quality_label_, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_bg_opa(air_quality_label_, LV_OPA_30, 0);
    }
    
    // 天气图标颜色
    if (weather_icon_label_ != nullptr) {
        lv_obj_set_style_text_color(weather_icon_label_, text_color, 0);
    }
    
    // 更新状态栏图标颜色和字体
    if (text_font->line_height >= 40) {
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
            lv_obj_set_style_text_color(network_label_, text_color, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
            lv_obj_set_style_text_color(battery_label_, text_color, 0);
        }
    } else {
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, icon_font, 0);
            lv_obj_set_style_text_color(network_label_, text_color, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, icon_font, 0);
            lv_obj_set_style_text_color(battery_label_, text_color, 0);
        }
    }
    
    // 更新时间容器背景色
    if (hour_tens_label_ != nullptr) {
        lv_obj_t* hour_container = lv_obj_get_parent(hour_tens_label_);
        if (hour_container != nullptr) {
            lv_obj_set_style_bg_color(hour_container, lv_color_hex(0x333333), 0);
        }
    }
    
    if (minute_tens_label_ != nullptr) {
        lv_obj_t* minute_container = lv_obj_get_parent(minute_tens_label_);
        if (minute_container != nullptr) {
            lv_obj_set_style_bg_color(minute_container, lv_color_hex(0x333333), 0);
        }
    }
    
    // 更新冒号容器背景色
    if (colon_label_ != nullptr) {
        lv_obj_t* colon_container = lv_obj_get_parent(colon_label_);
        if (colon_container != nullptr) {
            lv_obj_set_style_bg_color(colon_container, colon_bg_color, 0);
        }
    }
}

void ClockDesktopUI::SetWeather(const std::string& condition, int temp_high, int temp_low) {
    weather_condition_ = condition;
    weather_temp_high_ = temp_high;
    weather_temp_low_ = temp_low;
    
    if (!is_visible_) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    
    if (weather_text_label_ != nullptr) {
        char text[64];
        snprintf(text, sizeof(text), "%s %d℃/%d℃", condition.c_str(), temp_high, temp_low);
        lv_label_set_text(weather_text_label_, text);
    }
}

void ClockDesktopUI::SetWeatherIcon(const char* icon) {
    if (!is_visible_ || weather_icon_label_ == nullptr) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    lv_label_set_text(weather_icon_label_, icon != nullptr ? icon : "");
}

void ClockDesktopUI::SetAirQuality(const std::string& quality) {
    air_quality_ = quality;
    
    if (!is_visible_) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    
    if (air_quality_label_ != nullptr) {
        lv_label_set_text(air_quality_label_, quality.c_str());
    }
}

void ClockDesktopUI::CreateUI() {
    if (theme_ == nullptr) {
        theme_ = static_cast<LvglTheme*>(display_->GetTheme());
    }
    
    if (theme_ == nullptr) {
        ESP_LOGE(TAG, "Theme is null, cannot create UI");
        return;
    }
    
    auto text_font = theme_->text_font()->font();
    auto icon_font = theme_->icon_font()->font();
    auto large_icon_font = theme_->large_icon_font()->font();
    
    // 获取当前屏幕
    screen_ = lv_screen_active();
    
    // 创建主容器
    container_ = lv_obj_create(screen_);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    // 禁用滚动
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(container_, LV_DIR_NONE);
    
    // 设置背景
    if (theme_->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, theme_->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_color(container_, theme_->background_color(), 0);
    }
    
    // 创建状态栏
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 24);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(status_bar_, theme_->background_color(), 0);
    lv_obj_set_style_text_color(status_bar_, theme_->text_color(), 0);
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, 4, 0);
    lv_obj_set_style_pad_bottom(status_bar_, 4, 0);
    lv_obj_set_style_pad_left(status_bar_, 8, 0);
    lv_obj_set_style_pad_right(status_bar_, 8, 0);
    lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_bar_, LV_OBJ_FLAG_SCROLLABLE);
    
    // 网络图标
    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, 
        (text_font->line_height >= 40) ? large_icon_font : icon_font, 0);
    lv_obj_set_style_text_color(network_label_, theme_->text_color(), 0);
    
    // 状态文本
    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, theme_->text_color(), 0);
    lv_label_set_text(status_label_, "待命");
    
    // 电池图标
    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, 
        (text_font->line_height >= 40) ? large_icon_font : icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, theme_->text_color(), 0);
    
    // 创建内容区域
    lv_obj_t* content = lv_obj_create(container_);
    lv_obj_set_size(content, LV_HOR_RES, LV_VER_RES - 24);
    lv_obj_set_y(content, 24);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    
    // 计算布局位置
    int content_height = LV_VER_RES - 24;
    int remaining_height = content_height - 100 - 30;
    int date_margin_top = remaining_height / 5;
    
    // 日期标签
    date_label_ = lv_label_create(content);
    lv_obj_set_style_text_font(date_label_, text_font, 0);
    lv_obj_set_style_text_color(date_label_, theme_->text_color(), 0);
    lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(date_label_, LV_PCT(100));
    lv_obj_set_style_margin_top(date_label_, date_margin_top, 0);
    lv_obj_set_style_margin_bottom(date_label_, 2, 0);
    lv_label_set_text(date_label_, "12/10 周三");
    
    // 时钟容器
    lv_obj_t* clock_container = lv_obj_create(content);
    lv_obj_set_size(clock_container, 270, 100);
    lv_obj_set_style_radius(clock_container, 0, 0);
    lv_obj_set_style_pad_all(clock_container, 0, 0);
    lv_obj_set_style_border_width(clock_container, 0, 0);
    lv_obj_set_style_bg_opa(clock_container, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(clock_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock_container, 10, 0);
    lv_obj_set_style_margin_bottom(clock_container, 5, 0);
    
    // 小时容器
    lv_obj_t* hour_container = lv_obj_create(clock_container);
    lv_obj_set_size(hour_container, 105, 86);
    lv_obj_set_style_radius(hour_container, 18, 0); 
    lv_obj_set_style_bg_color(hour_container, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(hour_container, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(hour_container, 0, 0);
    lv_obj_set_style_border_width(hour_container, 0, 0);
    lv_obj_set_style_clip_corner(hour_container, true, 0);
    lv_obj_set_flex_flow(hour_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hour_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // 小时十位
    hour_tens_label_ = lv_label_create(hour_container);
    lv_obj_set_style_text_font(hour_tens_label_, &time_font, 0);
    lv_obj_set_style_text_color(hour_tens_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(hour_tens_label_, LV_OPA_TRANSP, 0);
    lv_label_set_text(hour_tens_label_, "1");
    
    // 小时个位
    hour_ones_label_ = lv_label_create(hour_container);
    lv_obj_set_style_text_font(hour_ones_label_, &time_font, 0);
    lv_obj_set_style_text_color(hour_ones_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(hour_ones_label_, LV_OPA_TRANSP, 0);
    lv_label_set_text(hour_ones_label_, "2");
    
   
    colon_label_ = lv_label_create(clock_container);
    lv_obj_set_style_text_font(colon_label_, &time_font, 0);
    lv_obj_set_style_text_color(colon_label_, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(colon_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_align(colon_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(colon_label_, ":");
    
    // 分钟容器
    lv_obj_t* minute_container = lv_obj_create(clock_container);
    lv_obj_set_size(minute_container, 105, 86);
    lv_obj_set_style_radius(minute_container, 18, 0);
    lv_obj_set_style_bg_color(minute_container, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(minute_container, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(minute_container, 0, 0);
    lv_obj_set_style_border_width(minute_container, 0, 0);
    lv_obj_set_style_clip_corner(minute_container, true, 0);
    lv_obj_set_flex_flow(minute_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(minute_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // 分钟十位
    minute_tens_label_ = lv_label_create(minute_container);
    lv_obj_set_style_text_font(minute_tens_label_, &time_font, 0);
    lv_obj_set_style_text_color(minute_tens_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(minute_tens_label_, LV_OPA_TRANSP, 0);
    lv_label_set_text(minute_tens_label_, "3");
    
    // 分钟个位
    minute_ones_label_ = lv_label_create(minute_container);
    lv_obj_set_style_text_font(minute_ones_label_, &time_font, 0);
    lv_obj_set_style_text_color(minute_ones_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(minute_ones_label_, LV_OPA_TRANSP, 0);
    lv_label_set_text(minute_ones_label_, "4");

    // 闹钟标签（显示在时钟下方）
    alarm_label_ = lv_label_create(content);
    lv_obj_set_style_text_font(alarm_label_, text_font, 0);
    lv_obj_set_style_text_color(alarm_label_, theme_->text_color(), 0);
    lv_obj_set_style_text_align(alarm_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(alarm_label_, LV_PCT(100));
    lv_obj_set_style_margin_top(alarm_label_, 8, 0);
    lv_obj_set_style_margin_bottom(alarm_label_, 2, 0);
    lv_label_set_text(alarm_label_, "");

    /* // 获取时钟容器宽度，用于对齐其他容器
    int clock_container_width = 270; 

     // 天气和空气质量容器 - 与时钟容器宽度对齐
     lv_obj_t* info_container = lv_obj_create(content);
     lv_obj_set_size(info_container, clock_container_width, 30);  
     lv_obj_set_x(info_container, (LV_HOR_RES - clock_container_width) / 2); 
     lv_obj_set_y(info_container, content_height - 40);
     lv_obj_set_style_radius(info_container, 0, 0);
     lv_obj_set_style_pad_all(info_container, 0, 0);
     lv_obj_set_style_border_width(info_container, 0, 0);
     lv_obj_set_style_bg_opa(info_container, LV_OPA_TRANSP, 0);
     lv_obj_set_flex_flow(info_container, LV_FLEX_FLOW_ROW);
     lv_obj_set_flex_align(info_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
     lv_obj_set_style_pad_column(info_container, 10, 0);  
     
     // 天气容器
     weather_container_ = lv_obj_create(info_container);
     int air_quality_width = 80;
     int spacing = 10;
     int weather_container_width = clock_container_width - air_quality_width - spacing;
     
     lv_obj_set_width(weather_container_, weather_container_width);
     lv_obj_set_height(weather_container_, 30);
     lv_obj_set_style_radius(weather_container_, 0, 0);
     lv_obj_set_style_pad_all(weather_container_, 0, 0);
     lv_obj_set_style_border_width(weather_container_, 0, 0);
     lv_obj_set_style_bg_opa(weather_container_, LV_OPA_TRANSP, 0);
     lv_obj_set_flex_flow(weather_container_, LV_FLEX_FLOW_ROW);
     lv_obj_set_flex_align(weather_container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
     lv_obj_set_style_pad_column(weather_container_, 8, 0);
     
     // 天气图标
     weather_icon_label_ = lv_label_create(weather_container_);
     lv_obj_set_style_text_font(weather_icon_label_, icon_font, 0);
     lv_obj_set_style_text_color(weather_icon_label_, theme_->text_color(), 0);
     lv_label_set_text(weather_icon_label_, "☀️");
     
     // 天气文本 - 启用滚动
     weather_text_label_ = lv_label_create(weather_container_);
     lv_obj_set_flex_grow(weather_text_label_, 1);
     lv_obj_set_style_text_font(weather_text_label_, text_font, 0);
     lv_obj_set_style_text_color(weather_text_label_, theme_->text_color(), 0);
     lv_obj_set_style_text_align(weather_text_label_, LV_TEXT_ALIGN_LEFT, 0);
     lv_label_set_text(weather_text_label_, "晴 25/19℃");
     lv_label_set_long_mode(weather_text_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);  
     
     // 空气质量标签
     air_quality_label_ = lv_label_create(info_container);
     lv_obj_set_width(air_quality_label_, air_quality_width); 
     lv_obj_set_height(air_quality_label_, 30);
     lv_obj_set_style_text_font(air_quality_label_, text_font, 0);
     lv_obj_set_style_text_color(air_quality_label_, lv_color_hex(0x00FF00), 0);
     lv_obj_set_style_bg_color(air_quality_label_, lv_color_hex(0x00FF00), 0);
     lv_obj_set_style_bg_opa(air_quality_label_, LV_OPA_30, 0);
     lv_obj_set_style_pad_hor(air_quality_label_, 8, 0);
     lv_obj_set_style_pad_ver(air_quality_label_, 4, 0);
     lv_obj_set_style_radius(air_quality_label_, 15, 0);
     lv_obj_set_style_text_align(air_quality_label_, LV_TEXT_ALIGN_CENTER, 0);
     lv_label_set_text(air_quality_label_, "空气优"); */
}

void ClockDesktopUI::DestroyUI() {
    if (screen_ == nullptr) {
        return;
    }
    
    if (container_ != nullptr) {
        lv_obj_del(container_);
        container_ = nullptr;
    }
    
    status_bar_ = nullptr;
    date_label_ = nullptr;
    hour_tens_label_ = nullptr;
    hour_ones_label_ = nullptr;
    minute_tens_label_ = nullptr;
    minute_ones_label_ = nullptr;
    colon_label_ = nullptr;
    weather_container_ = nullptr;
    weather_icon_label_ = nullptr;
    weather_text_label_ = nullptr;
    air_quality_label_ = nullptr;
    alarm_label_ = nullptr;
    network_label_ = nullptr;
    status_label_ = nullptr;
    battery_label_ = nullptr;
}

void ClockDesktopUI::UpdateTime() {
    if (hour_tens_label_ == nullptr || hour_ones_label_ == nullptr ||
        minute_tens_label_ == nullptr || minute_ones_label_ == nullptr) {
        return;
    }
    
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    
    if (tm->tm_year < 2025 - 1900) {
        return; // 时间未设置
    }
    
    // 更新小时（翻页式，每个数字独立显示）
    int hour = tm->tm_hour;
    char hour_tens = '0' + (hour / 10);
    char hour_ones = '0' + (hour % 10);
    char hour_tens_str[2] = {hour_tens, '\0'};
    char hour_ones_str[2] = {hour_ones, '\0'};
    lv_label_set_text(hour_tens_label_, hour_tens_str);
    lv_label_set_text(hour_ones_label_, hour_ones_str);
    
    // 更新分钟（翻页式，每个数字独立显示）
    int minute = tm->tm_min;
    char minute_tens = '0' + (minute / 10);
    char minute_ones = '0' + (minute % 10);
    char minute_tens_str[2] = {minute_tens, '\0'};
    char minute_ones_str[2] = {minute_ones, '\0'};
    lv_label_set_text(minute_tens_label_, minute_tens_str);
    lv_label_set_text(minute_ones_label_, minute_ones_str);
}

void ClockDesktopUI::UpdateDate() {
    if (date_label_ == nullptr) {
        return;
    }
    
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    
    if (tm->tm_year < 2025 - 1900) {
        return; // 时间未设置
    }
    
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%02d/%02d %s", 
             tm->tm_mon + 1, tm->tm_mday, weekdays[tm->tm_wday]);
    lv_label_set_text(date_label_, date_str);
}

void ClockDesktopUI::UpdateAlarm() {
    if (alarm_label_ == nullptr) {
        return;
    }
    
    auto& board = Board::GetInstance();
    auto* alarm_manager = board.GetAlarmManager();
    
    if (alarm_manager != nullptr && alarm_manager->HasActiveAlarm()) {
        std::vector<AlarmInfo> alarms;
        if (alarm_manager->GetAlarmList(alarms) && !alarms.empty()) {
            // 显示闹钟信息：名称和时间
            char alarm_text[64];
            snprintf(alarm_text, sizeof(alarm_text), "⏰ %s %s", 
                     alarms[0].name.c_str(), alarms[0].format_time.c_str());
            lv_label_set_text(alarm_label_, alarm_text);
        } else {
            lv_label_set_text(alarm_label_, "");
        }
    } else {
        lv_label_set_text(alarm_label_, "");
    }
}

void ClockDesktopUI::TimerCallback(void* arg) {
    auto* ui = static_cast<ClockDesktopUI*>(arg);
    if (ui == nullptr) {
        return;
    }
    // 让 LVGL 相关操作在 LVGL 线程执行，避免占用 esp_timer 任务栈导致溢出
    lv_async_call([](void* data) {
        auto* ui = static_cast<ClockDesktopUI*>(data);
        if (ui != nullptr) {
            ui->Update();
        }
    }, ui);
}

void ClockDesktopUI::DelayShowTimerCallback(void* arg) {
    auto* ui = static_cast<ClockDesktopUI*>(arg);
    if (ui == nullptr) {
        return;
    }
    lv_async_call([](void* data) {
        auto* ui = static_cast<ClockDesktopUI*>(data);
        if (ui != nullptr) {
            ui->delay_show_timer_started_ = false;
            ui->ShowByCharging();
        }
    }, ui);
}

void ClockDesktopUI::CheckChargingAndStandbyState() {
    // 检查设置UI是否正在显示，如果是则禁止显示时钟UI
    auto* lcd_display = dynamic_cast<LcdDisplay*>(display_);
    if (lcd_display != nullptr) {
        auto* settings_ui = lcd_display->GetSettingsPageUI();
        if (settings_ui != nullptr && settings_ui->IsVisible()) {
            // 如果设置UI正在显示，停止延迟显示定时器
            if (delay_show_timer_ != nullptr && delay_show_timer_started_) {
                esp_timer_stop(delay_show_timer_);
                delay_show_timer_started_ = false;
            }
            // 如果时钟UI正在显示且是由充电状态自动显示的，则隐藏
            if (is_visible_ && auto_shown_by_charging_) {
                Hide();
            }
            return;  // 禁止显示时钟UI
        }
    }
    
    auto& board = Board::GetInstance();
    auto& app = Application::GetInstance();
    
    // 获取充电状态
    int battery_level;
    bool charging, discharging;
    bool has_battery = board.GetBatteryLevel(battery_level, charging, discharging);
    
    // 获取设备状态
    DeviceState device_state = app.GetDeviceState();
    bool is_standby = (device_state == kDeviceStateIdle);
    
    // 更新充电状态
    bool was_charging = is_charging_;
    is_charging_ = has_battery && charging;
    
    // 如果非待命状态，只隐藏由充电状态自动显示的UI，不影响其他方式显示的UI（如睡眠模式）
    if (!is_standby) {
        if (is_visible_ && auto_shown_by_charging_) {
            Hide();
        }
        // 停止延迟显示定时器
        if (delay_show_timer_ != nullptr && delay_show_timer_started_) {
            esp_timer_stop(delay_show_timer_);
            delay_show_timer_started_ = false;
        }
        return;
    }
    
    // 如果是待命状态
    if (is_standby) {
        // 如果正在充电
        if (is_charging_) {
            // 如果充电状态刚变化（从非充电变为充电），启动10秒延迟定时器
            if (!was_charging && !delay_show_timer_started_) {
                if (delay_show_timer_ != nullptr) {
                    ESP_ERROR_CHECK(esp_timer_start_once(delay_show_timer_, 10000000)); // 10秒
                    delay_show_timer_started_ = true;
                    ESP_LOGI(TAG, "Charging detected, will show clock UI in 10 seconds");
                }
            }
            // 如果已经在充电且定时器未启动，启动定时器
            else if (!delay_show_timer_started_ && !is_visible_) {
                if (delay_show_timer_ != nullptr) {
                    ESP_ERROR_CHECK(esp_timer_start_once(delay_show_timer_, 10000000)); // 10秒
                    delay_show_timer_started_ = true;
                    ESP_LOGI(TAG, "Charging detected, will show clock UI in 10 seconds");
                }
            }
        } else {
            // 如果停止充电，只隐藏由充电状态自动显示的UI，不影响其他方式显示的UI
            if (is_visible_ && auto_shown_by_charging_) {
                Hide();
                auto_shown_by_charging_ = false;
            }
            // 停止延迟显示定时器
            if (delay_show_timer_ != nullptr && delay_show_timer_started_) {
                esp_timer_stop(delay_show_timer_);
                delay_show_timer_started_ = false;
            }
        }
    }
}

void ClockDesktopUI::SetCameraPreviewActive(bool active) {
    camera_preview_active_ = active;
    ESP_LOGI(TAG, "Camera preview active set to: %s", active ? "true" : "false");
}

bool ClockDesktopUI::IsCameraPreviewActive() {
    return camera_preview_active_;
}

