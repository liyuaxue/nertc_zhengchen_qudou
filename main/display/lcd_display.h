#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"
#include "device_state.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <memory>
#include <random>

#define PREVIEW_IMAGE_DURATION_MS 5000
#define ACTIVATING_QRCODE_MIN_DURATION_MS 3000

// Forward declaration
class ClockDesktopUI;
class SettingsPageUI;

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* camera_hint_bar_ = nullptr;
    lv_obj_t* camera_hint_icon_ = nullptr;
    lv_obj_t* camera_hint_label_ = nullptr;
    lv_obj_t* camera_hint_first_row_ = nullptr;  
    lv_obj_t* camera_hint_second_row_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    std::unique_ptr<LvglGif> chat_message_gif_controller_ = nullptr;  
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* chat_message_image_ = nullptr;   
    esp_timer_handle_t preview_timer_ = nullptr;
    esp_timer_handle_t activating_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    ClockDesktopUI* clock_ui_ = nullptr;
    SettingsPageUI* settings_page_ui_ = nullptr;
    
    // 文字模式相关
    bool text_mode_ = true;  
    bool saved_text_mode_ = true;  
    std::mt19937 random_generator_; 
    bool hide_subtitle_ = false;
    static bool camera_preview_hide_bottom_bar_;
    static bool camera_preview_hint_enabled_;
    int64_t activating_enter_time_us_ = 0;
    
    void InitializeLcdThemes();
    void SetupUI();
    void UpdateUILayout(); 
    void UpdateStatusBarStyle(); 
    void OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state);
    void UpdateEmotionByState(DeviceState state);
    void OnActivatingMinDurationElapsed();
    void SetTextModeInternal(bool text_mode, bool save_to_settings); 
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // 添加protected构造函数
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override; 
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    
    // Get clock desktop UI
    ClockDesktopUI* GetClockDesktopUI() { return clock_ui_; }
    
    // Get settings page UI
    SettingsPageUI* GetSettingsPageUI() { return settings_page_ui_; }
    
    // Set text mode (true for text mode, false for emotion mode)
    void SetTextMode(bool text_mode);
    
    // Get current text mode
    bool GetTextMode() const { return text_mode_; }

    void SetHideSubtitle(bool hide);
    static void SetCameraPreviewHideBottomBar(bool hide);
    static void SetCameraPreviewHintEnabled(bool enabled);
};

// SPI LCD显示器
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD显示器
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD显示器
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
