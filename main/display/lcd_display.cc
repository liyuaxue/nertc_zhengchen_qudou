#include "lcd_display.h"
#include "device_state.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"
#include "clock_desktop_ui.h"
#include "settings_page_ui.h"
#include "music_player_ui.h"

#include <vector>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <random>

#include "board.h"
#include "application.h"

extern "C" {
    extern const lv_img_dsc_t listening;
    extern const lv_img_dsc_t listening0;
    extern const lv_img_dsc_t speaking0;
    extern const lv_img_dsc_t idle0;
    extern const lv_img_dsc_t qrcode;
    extern const lv_img_dsc_t icon_video;
}

#define TAG "LcdDisplay"

bool LcdDisplay::camera_preview_hide_bottom_bar_ = false;
bool LcdDisplay::camera_preview_hint_enabled_ = false;

// Use builtin fonts passed from CMake via BUILTIN_TEXT_FONT / BUILTIN_ICON_FONT
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x000000));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel), random_generator_(std::random_device{}()) {
    width_ = width;
    height_ = height;

    InitializeLcdThemes();

    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "dark");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    text_mode_ = settings.GetBool("text_mode", true);
    saved_text_mode_ = text_mode_;  
    
    DeviceState current_state = Application::GetInstance().GetDeviceState();
    if (current_state == kDeviceStateWifiConfiguring || current_state == kDeviceStateActivating) {
        saved_text_mode_ = text_mode_;  
        text_mode_ = true;  
    }

    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            auto* display = static_cast<LcdDisplay*>(arg);
            if (display == nullptr) {
                return;
            }
            
            lv_async_call([](void* data) {
                auto* display = static_cast<LcdDisplay*>(data);
                if (display != nullptr) {
                    display->SetPreviewImage(nullptr);
                }
            }, display);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);

    esp_timer_create_args_t activating_timer_args = {
        .callback = [](void* arg) {
            auto* display = static_cast<LcdDisplay*>(arg);
            if (display == nullptr) {
                return;
            }
            lv_async_call([](void* data) {
                auto* display = static_cast<LcdDisplay*>(data);
                if (display != nullptr) {
                    display->OnActivatingMinDurationElapsed();
                }
            }, display);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "activating_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&activating_timer_args, &activating_timer_);

    DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
        [this](DeviceState previous_state, DeviceState current_state) {
            this->OnDeviceStateChanged(previous_state, current_state);
        }
    );
    

    if (text_mode_) {
        DeviceState current_state = Application::GetInstance().GetDeviceState();
        UpdateEmotionByState(current_state);
    }
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
    
    // 初始化时钟桌面UI
    clock_ui_ = new ClockDesktopUI(this);
    
    // 初始化设置页面UI
    settings_page_ui_ = new SettingsPageUI(this);
    
    // 初始化音乐播放器UI
    music_player_ui_ = new MusicPlayerUI(this);
}

RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    SetupUI();
    
    // 初始化时钟桌面UI
    clock_ui_ = new ClockDesktopUI(this);
    
    // 初始化设置页面UI
    settings_page_ui_ = new SettingsPageUI(this);
    
    // 初始化音乐播放器UI
    music_player_ui_ = new MusicPlayerUI(this);
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    SetupUI();
    
    clock_ui_ = new ClockDesktopUI(this);
    music_player_ui_ = new MusicPlayerUI(this);
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);
    
    if (settings_page_ui_ != nullptr) {
        delete settings_page_ui_;
        settings_page_ui_ = nullptr;
    }
    
    if (music_player_ui_ != nullptr) {
        delete music_player_ui_;
        music_player_ui_ = nullptr;
    }
    
    if (clock_ui_ != nullptr) {
        delete clock_ui_;
        clock_ui_ = nullptr;
    }
    
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (chat_message_gif_controller_) {
        chat_message_gif_controller_->Stop();
        chat_message_gif_controller_.reset();
    }
    
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (activating_timer_ != nullptr) {
        esp_timer_stop(activating_timer_);
        esp_timer_delete(activating_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (chat_message_image_ != nullptr) {
        lv_obj_del(chat_message_image_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
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
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(emoji_box_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, ""); 
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN); 

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_, height_);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Camera preview hint bar (shown only during camera preview) */
    camera_hint_bar_ = lv_obj_create(screen);
    // Make it a centered "pill" instead of a full-width bottom bar
    lv_obj_set_size(camera_hint_bar_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(camera_hint_bar_, 40, 0);
    lv_obj_set_style_radius(camera_hint_bar_, 20, 0);
    lv_obj_set_style_border_width(camera_hint_bar_, 0, 0);
    lv_obj_set_style_bg_color(camera_hint_bar_, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_bg_opa(camera_hint_bar_, LV_OPA_70, 0);
    lv_obj_set_style_pad_top(camera_hint_bar_, 8, 0);
    lv_obj_set_style_pad_bottom(camera_hint_bar_, 8, 0);
    lv_obj_set_style_pad_left(camera_hint_bar_, 14, 0);
    lv_obj_set_style_pad_right(camera_hint_bar_, 14, 0);
    lv_obj_set_style_pad_column(camera_hint_bar_, 10, 0);
    lv_obj_set_style_pad_row(camera_hint_bar_, 4, 0);
    lv_obj_set_scrollbar_mode(camera_hint_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(camera_hint_bar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(camera_hint_bar_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(camera_hint_bar_, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(camera_hint_bar_, LV_OBJ_FLAG_HIDDEN);

    // Icon (shown for both preview stream and static preview image)
    camera_hint_icon_ = lv_img_create(camera_hint_bar_);
    lv_img_set_src(camera_hint_icon_, &icon_video);
    lv_obj_set_size(camera_hint_icon_, 16, 16);

    // First row: "双击退出，单击拍照" (only shown during preview stream)
    camera_hint_first_row_ = lv_label_create(camera_hint_bar_);
    lv_label_set_text(camera_hint_first_row_, "双击退出，单击拍照");
    lv_obj_set_style_text_align(camera_hint_first_row_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(camera_hint_first_row_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_font(camera_hint_first_row_, text_font, 0);
    // Keep reference for backward compatibility
    camera_hint_label_ = camera_hint_first_row_;

    // Second row: "长按颠倒画面" (always shown)
    camera_hint_second_row_ = lv_label_create(camera_hint_bar_);
    lv_label_set_text(camera_hint_second_row_, "长按颠倒画面");
    lv_obj_set_style_text_align(camera_hint_second_row_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(camera_hint_second_row_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_font(camera_hint_second_row_, text_font, 0);


    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);


    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);    
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);



    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);
    

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(bottom_bar_, 48, 0); // Set minimum height 48
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_top(bottom_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(bottom_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);

    /* chat_message_label_ placed in bottom_bar_ and vertically centered */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8)); // Subtract left and right padding
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP); // Auto wrap mode
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0); // Center text alignment
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0); // Vertically and horizontally centered in bottom_bar_

    chat_message_image_ = lv_image_create(bottom_bar_);
    lv_obj_set_size(chat_message_image_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_align(chat_message_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(chat_message_image_, LV_OBJ_FLAG_HIDDEN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    

    volume_bar_container_ = lv_obj_create(screen);
    lv_obj_set_size(volume_bar_container_, 40, 140);
    lv_obj_align(volume_bar_container_, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(volume_bar_container_, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(volume_bar_container_, 20, 0);
    lv_obj_set_style_border_width(volume_bar_container_, 0, 0);
    lv_obj_set_style_pad_all(volume_bar_container_, 0, 0);
    lv_obj_set_flex_flow(volume_bar_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(volume_bar_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_style_pad_row(volume_bar_container_, 0, 0);
    lv_obj_set_style_pad_column(volume_bar_container_, 0, 0);
    lv_obj_clear_flag(volume_bar_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(volume_bar_container_, LV_OBJ_FLAG_HIDDEN);

    volume_bar_ = lv_bar_create(volume_bar_container_);
    lv_obj_set_size(volume_bar_, 16, 80);
    lv_bar_set_range(volume_bar_, 0, 100);
    lv_obj_set_style_bg_color(volume_bar_, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_bar_, lv_color_hex(0x64e5ff), LV_PART_INDICATOR);
    lv_obj_set_style_radius(volume_bar_, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(volume_bar_, 8, LV_PART_INDICATOR);
    lv_obj_clear_flag(volume_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_margin_left(volume_bar_, 12, 0);
    lv_obj_set_style_margin_right(volume_bar_, 12, 0);

    volume_icon_label_ = lv_label_create(volume_bar_container_);
    lv_obj_set_style_text_font(volume_icon_label_, icon_font, 0);
    lv_obj_set_style_text_color(volume_icon_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(volume_icon_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(volume_icon_label_, 0, 0);
    lv_obj_set_style_margin_top(volume_icon_label_, 10, 0);
    lv_obj_set_width(volume_icon_label_, LV_PCT(100));
    lv_label_set_text(volume_icon_label_, FONT_AWESOME_VOLUME_HIGH);
    
    UpdateUILayout();
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        if (camera_hint_bar_ != nullptr) {
            lv_obj_add_flag(camera_hint_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        // Restore normal layout (e.g. bottom bar) when leaving preview
        UpdateUILayout();
        preview_image_cached_.reset();
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();

    lv_image_set_src(preview_image_, img_dsc);
    lv_obj_set_size(preview_image_, width_, height_);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        lv_coord_t scale_w = (width_ * 256) / img_dsc->header.w;
        lv_coord_t scale_h = (height_ * 256) / img_dsc->header.h;
        lv_coord_t scale = (scale_w > scale_h) ? scale_w : scale_h;
        lv_image_set_scale(preview_image_, scale);
    }

    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    // During preview, optionally show camera hint bar
    // Show hint bar if preview stream is enabled OR if displaying static preview image
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (camera_hint_bar_ != nullptr) {
        // Always show hint bar when displaying preview image (both stream and static image)
        lv_obj_remove_flag(camera_hint_bar_, LV_OBJ_FLAG_HIDDEN);
        
        // Icon is always shown
        if (camera_hint_icon_ != nullptr) {
            lv_obj_remove_flag(camera_hint_icon_, LV_OBJ_FLAG_HIDDEN);
        }
        
        // If preview stream is enabled, show full hint (icon + "双击退出，单击拍照" + "长按颠倒画面")
        // If static preview image, show simplified hint (icon + "长按颠倒画面" only)
        if (camera_hint_first_row_ != nullptr) {
            if (camera_preview_hint_enabled_) {
                // Preview stream: show "双击退出，单击拍照"
                lv_obj_remove_flag(camera_hint_first_row_, LV_OBJ_FLAG_HIDDEN);
            } else {
                // Static preview image: hide "双击退出，单击拍照", only show icon and "长按颠倒画面"
                lv_obj_add_flag(camera_hint_first_row_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // Second row (长按颠倒画面) is always shown
        if (camera_hint_second_row_ != nullptr) {
            lv_obj_remove_flag(camera_hint_second_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    if (!text_mode_) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        if (chat_message_image_ != nullptr) {
            lv_obj_add_flag(chat_message_image_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    std::string content_str(content);

    // 通用：隐藏 GIF 图片并显示文字标签
    auto hide_image_and_show_label = [this]() {
        if (chat_message_image_ != nullptr && !lv_obj_has_flag(chat_message_image_, LV_OBJ_FLAG_HIDDEN)) {
            if (chat_message_gif_controller_) {
                chat_message_gif_controller_->Stop();
                chat_message_gif_controller_.reset();
            }
            lv_obj_add_flag(chat_message_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (chat_message_label_ != nullptr) {
            lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        }
    };

    if (content_str.rfind("%", 0) == 0) {
        if (content_str.find("camera.take_photo") != std::string::npos) {
            hide_image_and_show_label();
            lv_label_set_text(chat_message_label_, "正在拍照并分析图片中");
            return;
        }
        return;
    }

    if(strcmp(content, "llm image sent") == 0) {
        hide_image_and_show_label();
        lv_label_set_text(chat_message_label_, "分析图片中...");
        return;
    }

    // 在聆听状态时，如果有文字更新，优先显示文字标签
    DeviceState current_state = Application::GetInstance().GetDeviceState();
    bool is_listening = (current_state == kDeviceStateListening);

    bool has_text_content = (content != nullptr && strlen(content) > 0 && 
                             strcmp(content, "llm image sent") != 0);

    if (has_text_content && (is_listening || (role != nullptr && strcmp(role, "user") == 0))) {
        hide_image_and_show_label();
    }

    lv_label_set_text(chat_message_label_, content);
}

void LcdDisplay::SetEmotion(const char* emotion) {
    if (text_mode_) {
        return;
    }
    
    const char* mapped_emotion = emotion;
    DeviceState current_state = Application::GetInstance().GetDeviceState();
    
    if (current_state == kDeviceStateSpeaking) {
        if (strcmp(mapped_emotion, "happy") == 0) {
            mapped_emotion = "laughing";
        } else if (strcmp(mapped_emotion, "surprised") == 0) {
            mapped_emotion = "shocked";
        }
    }
    
    if (strcmp(emotion, "neutral") == 0) {
        std::uniform_int_distribution<int> dist(0, 1);
        int choice = dist(random_generator_);
        mapped_emotion = (choice == 0) ? "happy" : "neutral";
    } else if (strcmp(emotion, "error") == 0) {
        mapped_emotion = "surprised";
    }
    
    DisplayLockGuard lock(this);
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (chat_message_gif_controller_) {
        chat_message_gif_controller_->Stop();
        chat_message_gif_controller_.reset();
    }
    
    if (emoji_image_ == nullptr || emoji_box_ == nullptr) {
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(mapped_emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(mapped_emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            int status_bar_height = top_bar_ != nullptr ? lv_obj_get_height(top_bar_) : 0;
            lv_obj_set_size(emoji_box_, LV_HOR_RES, LV_VER_RES - status_bar_height);
            lv_obj_align(emoji_box_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (emoji_label_ != nullptr) {
                lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (emoji_image_ != nullptr) {
                lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }
    
    int status_bar_height = top_bar_ != nullptr ? lv_obj_get_height(top_bar_) : 0;
    lv_obj_set_size(emoji_box_, LV_HOR_RES, LV_VER_RES - status_bar_height);
    lv_obj_align(emoji_box_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    
    lv_obj_set_size(emoji_image_, LV_HOR_RES, LV_VER_RES - status_bar_height);
    lv_obj_align(emoji_image_, LV_ALIGN_CENTER, 0, 0);
    
    if (image->IsGif()) {
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        
        if (gif_controller_->IsLoaded()) {
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", mapped_emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    if (lvgl_theme == nullptr) {
        ESP_LOGE(TAG, "SetTheme: lvgl_theme is null");
        return;
    }
    
    if (lvgl_theme->text_font() == nullptr || 
        lvgl_theme->icon_font() == nullptr || 
        lvgl_theme->large_icon_font() == nullptr) {
        ESP_LOGE(TAG, "SetTheme: font is null");
        return;
    }
    
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        ESP_LOGE(TAG, "SetTheme: screen is null");
        return;
    }

    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();
    
    if (text_font == nullptr || icon_font == nullptr || large_icon_font == nullptr) {
        ESP_LOGE(TAG, "SetTheme: font pointer is null");
        return;
    }

    if (text_font->line_height >= 40) {
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        }
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
        }
    } else {
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        }
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, icon_font, 0);
        }
    }

    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }
    
    current_theme_ = lvgl_theme;
    UpdateStatusBarStyle();
    
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }
    
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    

    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }

    if (camera_hint_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(camera_hint_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(camera_hint_bar_, lvgl_theme->background_color(), 0);
    }
    if (camera_hint_label_ != nullptr) {
        lv_obj_set_style_text_color(camera_hint_label_, lvgl_theme->text_color(), 0);
    }

    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    if (clock_ui_ != nullptr) {
        clock_ui_->SetTheme(lvgl_theme);
    }
    
    if (settings_page_ui_ != nullptr) {
        settings_page_ui_->SetTheme(lvgl_theme);
    }

    Display::SetTheme(lvgl_theme);
    
    if (text_mode_) {
        DeviceState current_state = Application::GetInstance().GetDeviceState();
        UpdateEmotionByState(current_state);
    }
}

void LcdDisplay::OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state) {
    if (current_state == kDeviceStateWifiConfiguring || current_state == kDeviceStateActivating || current_state == kDeviceStateUpgrading) {
        if (!text_mode_) {
            saved_text_mode_ = text_mode_;
            SetTextModeInternal(true, false);
        }
    }
    else if (previous_state == kDeviceStateWifiConfiguring || previous_state == kDeviceStateActivating || previous_state == kDeviceStateUpgrading) {
        if (text_mode_ && !saved_text_mode_) {
            SetTextMode(saved_text_mode_);
        }
    }
    
    if (!text_mode_) {
        return;
    }

    // 进入激活态时记录时间，并取消任何等待中的延迟刷新
    if (current_state == kDeviceStateActivating) {
        activating_enter_time_us_ = esp_timer_get_time();
        if (activating_timer_ != nullptr) {
            esp_timer_stop(activating_timer_);
        }
        UpdateEmotionByState(current_state);
        return;
    }

    // 从激活态离开时，确保二维码至少显示 ACTIVATING_QRCODE_MIN_DURATION_MS
    if (previous_state == kDeviceStateActivating && activating_enter_time_us_ > 0) {
        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_ms = (now_us - activating_enter_time_us_) / 1000;
        if (elapsed_ms < ACTIVATING_QRCODE_MIN_DURATION_MS) {
            int64_t remaining_ms = ACTIVATING_QRCODE_MIN_DURATION_MS - elapsed_ms;
            if (activating_timer_ != nullptr) {
                esp_timer_stop(activating_timer_);
                ESP_ERROR_CHECK(esp_timer_start_once(activating_timer_, remaining_ms * 1000));
            }
            return; // 延迟刷新，避免二维码一闪而过
        }
    }
    
    UpdateEmotionByState(current_state);
}

void LcdDisplay::OnActivatingMinDurationElapsed() {
    if (!text_mode_ || emoji_image_ == nullptr) {
        return;
    }

    // 到达最短展示时间后，刷新为“当前最新状态”的表情/界面
    DeviceState current_state = Application::GetInstance().GetDeviceState();
    UpdateEmotionByState(current_state);
}

void LcdDisplay::UpdateEmotionByState(DeviceState state) {
    if (!text_mode_ || emoji_image_ == nullptr) {
        return;
    }
    
    DisplayLockGuard lock(this);
    
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (chat_message_gif_controller_) {
        chat_message_gif_controller_->Stop();
        chat_message_gif_controller_.reset();
    }
    
    const lv_img_dsc_t* gif_dsc = nullptr;
    
    if (state == kDeviceStateSpeaking) {
        gif_dsc =&speaking0;  
    } else if (state == kDeviceStateListening) {
        gif_dsc =&listening0;
    } else if (state == kDeviceStateActivating) {
        // qrcode is a static image (not GIF). Render directly to avoid GIF decode errors.
        lv_image_set_src(emoji_image_, &qrcode);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        return;
    } else {
       gif_dsc = &idle0;
    }
    
    if (gif_dsc != nullptr) {
        gif_controller_ = std::make_unique<LvglGif>(gif_dsc);
        
        if (gif_controller_->IsLoaded()) {
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for state: %d", state);
            gif_controller_.reset();
        }
    }
    
    if (state == kDeviceStateListening) {
        if (chat_message_image_ != nullptr) {
            chat_message_gif_controller_ = std::make_unique<LvglGif>(&listening);
            
            if (chat_message_gif_controller_->IsLoaded()) {
                chat_message_gif_controller_->SetFrameCallback([this]() {
                    lv_image_set_src(chat_message_image_, chat_message_gif_controller_->image_dsc());
                });
                
                lv_image_set_src(chat_message_image_, chat_message_gif_controller_->image_dsc());
                chat_message_gif_controller_->Start();
                
                lv_obj_remove_flag(chat_message_image_, LV_OBJ_FLAG_HIDDEN);
            } else {
                ESP_LOGE(TAG, "Failed to load listening GIF");
                chat_message_gif_controller_.reset();
            }
        }
        if (chat_message_label_ != nullptr) {
            lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (chat_message_image_ != nullptr) {
            lv_obj_add_flag(chat_message_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (chat_message_label_ != nullptr) {
            lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::UpdateStatusBarStyle() {
    if (top_bar_ == nullptr) {
        return;
    }
    
    DisplayLockGuard lock(this);
    
    if (!text_mode_) {
        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(top_bar_, lv_color_hex(0x000000), 0);
        }
        
        lv_color_t cyan_color = lv_color_hex(0x00FFFF);
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_color(network_label_, cyan_color, 0);
        }
        if (status_label_ != nullptr) {
            lv_obj_set_style_text_color(status_label_, cyan_color, 0);
        }
        if (notification_label_ != nullptr) {
            lv_obj_set_style_text_color(notification_label_, cyan_color, 0);
        }
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_color(mute_label_, cyan_color, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_color(battery_label_, cyan_color, 0);
        }
    } else {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
        }
       
        lv_color_t text_color = lvgl_theme->text_color();
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_color(network_label_, text_color, 0);
        }
        if (status_label_ != nullptr) {
            lv_obj_set_style_text_color(status_label_, text_color, 0);
        }
        if (notification_label_ != nullptr) {
            lv_obj_set_style_text_color(notification_label_, text_color, 0);
        }
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_color(mute_label_, text_color, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_color(battery_label_, text_color, 0);
        }
    }
}

void LcdDisplay::UpdateUILayout() {
    DisplayLockGuard lock(this);
    
    if (emoji_box_ == nullptr || chat_message_label_ == nullptr) {
        return;
    }
    
    if (text_mode_) {
        lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_center(emoji_box_);
        if (emoji_image_ != nullptr) {
            lv_obj_set_size(emoji_image_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_center(emoji_image_);
        }
        if (bottom_bar_ != nullptr) {
            if (hide_subtitle_ || camera_preview_hide_bottom_bar_) {
                lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        int status_bar_height = top_bar_ != nullptr ? lv_obj_get_height(top_bar_) : 0;
        lv_obj_set_size(emoji_box_, LV_HOR_RES, LV_VER_RES - status_bar_height);
        lv_obj_align(emoji_box_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(emoji_box_, lv_color_hex(0x000000), 0);
        if (emoji_image_ != nullptr) {
            lv_obj_set_size(emoji_image_, LV_HOR_RES, LV_VER_RES - status_bar_height);
            lv_obj_align(emoji_image_, LV_ALIGN_CENTER, 0, 0);
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        if (chat_message_image_ != nullptr) {
            lv_obj_add_flag(chat_message_image_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    UpdateStatusBarStyle();
}

void LcdDisplay::SetTextModeInternal(bool text_mode, bool save_to_settings) {
    DeviceState current_state = Application::GetInstance().GetDeviceState();
    if (current_state == kDeviceStateWifiConfiguring || current_state == kDeviceStateActivating || current_state == kDeviceStateUpgrading) {
        if (!text_mode) {
            return;
        }
    }
    
    if (text_mode_ == text_mode) {
        return;
    }
    
    text_mode_ = text_mode;
    
    if (save_to_settings) {
        Settings settings("display", true);
        settings.SetBool("text_mode", text_mode_);
    }

    {
        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        if (!text_mode_) {
            SetTheme(LvglThemeManager::GetInstance().GetTheme("dark"));
        }
    }
    
    UpdateUILayout();
    
    UpdateStatusBarStyle();

    if (text_mode_) {
        DeviceState current_state = Application::GetInstance().GetDeviceState();
        UpdateEmotionByState(current_state);
    } else {
        DisplayLockGuard lock(this);
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        if (chat_message_gif_controller_) {
            chat_message_gif_controller_->Stop();
            chat_message_gif_controller_.reset();
        }
        SetEmotion("neutral");
    }
}

void LcdDisplay::SetTextMode(bool text_mode) {
    SetTextModeInternal(text_mode, true);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    
    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::SetCameraPreviewHideBottomBar(bool hide) {
    camera_preview_hide_bottom_bar_ = hide;
    auto* lcd_display = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
    if (lcd_display != nullptr) {
        lcd_display->UpdateUILayout();
    }
}

void LcdDisplay::SetCameraPreviewHintEnabled(bool enabled) {
    camera_preview_hint_enabled_ = enabled;
}

