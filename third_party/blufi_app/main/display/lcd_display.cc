#include "lcd_display.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "type.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));          //rgb(255, 255, 255)
    light_theme->set_text_color(lv_color_hex(0x000000));                //rgb(0, 0, 0)
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));     //rgb(224, 224, 224)
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));         //rgb(0, 128, 0)
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));    //rgb(221, 221, 221)
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));       //rgb(255, 255, 255)
    light_theme->set_system_text_color(lv_color_hex(0x000000));         //rgb(0, 0, 0)
    light_theme->set_border_color(lv_color_hex(0x000000));              //rgb(0, 0, 0)
    light_theme->set_low_battery_color(lv_color_hex(0x000000));         //rgb(0, 0, 0)
    light_theme->set_text_font(text_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));           //rgb(0, 0, 0)
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));                 //rgb(255, 255, 255)
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));      //rgb(31, 31, 31)
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));          //rgb(0, 128, 0)
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));     //rgb(34, 34, 34)
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));        //rgb(0, 0, 0)
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));          //rgb(255, 255, 255)
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));               //rgb(255, 255, 255)
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));          //rgb(255, 0, 0)
    dark_theme->set_text_font(text_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    // Settings settings("display", false);
    // std::string theme_name = settings.GetString("theme", "dark");
    std::string theme_name = "dark";
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);
}

LcdDisplay::~LcdDisplay() {
    SetBgImage(nullptr);

    if (bg_image_ != nullptr) {
        lv_obj_del(bg_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();    

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - 禁用flex布局，改用绝对定位 */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, width_, height_);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);
    lv_obj_set_layout(container_, LV_LAYOUT_NONE); // 关键：禁用flex布局

    /* Background Image - 先创建（z-order最底层） */
    bg_image_ = lv_image_create(container_);
    lv_obj_set_size(bg_image_, width_, height_); // 设置为全屏尺寸
    lv_obj_align(bg_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bg_image_, LV_OBJ_FLAG_HIDDEN);

    /* Content - 后创建（覆盖在bg_image之上） */
    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, width_, height_); // 全屏透明层
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0); // 透明背景，能看到下层图片
    lv_obj_set_layout(content_, LV_LAYOUT_NONE);

    /* Title Label - 标题栏 */
    title_label_ = lv_label_create(content_);
    lv_label_set_text(title_label_, "");
    lv_obj_set_style_text_font(title_label_, text_font, 0); // 使用大号字体
    lv_obj_set_style_text_color(title_label_, lvgl_theme->text_color(), 0); 

    // 设置放大因子
    int zoom_factor = 370;
    float title_height_factor = 0.15; // 根据实际情况调整这个值
    float version_height_factor = 0.14; // 根据实际情况调整这个值
    int x_offset = -25; // 根据实际情况调整这个值
    if (g_app == APP_TYPE_BLUFI) {
        zoom_factor = 262; 
        x_offset = 0;
        title_height_factor = 0.11;
    } 
    lv_obj_set_style_transform_zoom(title_label_, zoom_factor, 0);

    // 确保文字居中对齐
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, x_offset, height_ * title_height_factor);

    /* Status Label - 状态栏 */
    status_label_ = lv_label_create(content_);
    lv_label_set_text(status_label_, "");
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, -height_ * 0.21); 

    /* Version Label - 版本栏 */
    version_label_ = lv_label_create(content_);
    lv_label_set_text(version_label_, "");
    lv_obj_set_style_text_color(version_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(version_label_, LV_ALIGN_BOTTOM_MID, 0, -height_ * version_height_factor); 

    // /* Chat Message Label - 在content上居中显示 */
    // chat_message_label_ = lv_label_create(content_);
    // lv_label_set_text(chat_message_label_, "");
    // lv_obj_set_width(chat_message_label_, width_ * 0.9);
    // lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    // lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    // lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0); 
    // lv_obj_center(chat_message_label_); // 居中对齐

    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0); 
    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, -height_ * 0.04); // 最下面 20% 处
}

void LcdDisplay::SetBgImage(const lv_image_dsc_t* img_dsc, bool scale)
{
    DisplayLockGuard lock(this);
    if (bg_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    lv_image_set_src(bg_image_, img_dsc);

    lv_obj_remove_flag(bg_image_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }
    lv_label_set_text(chat_message_label_, content);
}

void LcdDisplay::SetEmotion(const char* emotion) {
    return;
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    printf("SetTheme name = %s\n", theme->name().c_str());
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }
    
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    lv_obj_set_style_text_color(title_label_, lvgl_theme->text_color(), 0); 
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(version_label_, lvgl_theme->text_color(), 0);

    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetTheme(const std::string& theme_name) {
    if (theme_name != "dark" && theme_name != "light")
        return;

    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);
    SetTheme(current_theme_);
}

void LcdDisplay::SetTitleText(const std::string& text) {
    lv_label_set_text(title_label_, text.c_str());
}

void LcdDisplay::SetStatusText(const std::string& text) {
    lv_label_set_text(status_label_, std::string(status_profile_ + text).c_str());
}

void LcdDisplay::SetVersionText(const std::string& version) {
    lv_label_set_text(version_label_, std::string(version_profile_ + version).c_str());
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    SetupUI();
}