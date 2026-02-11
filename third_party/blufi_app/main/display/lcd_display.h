#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>
#include "type.h"

#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* bg_image_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* version_label_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    std::string status_profile_;
    std::string version_profile_;

    void InitializeLcdThemes();
    virtual void SetupUI();


protected:
    // 添加protected构造函数
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override; 
    virtual void SetBgImage(const lv_image_dsc_t* img_dsc, bool scale = false) override; 
    virtual bool Lock(int timeout_ms = 10000) override;
    virtual void Unlock() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    void SetTheme(const std::string& theme_name);
    void SetTitleText(const std::string& text);
    void SetStatusProfile(const std::string& profile) {
        status_profile_ = profile;
    }
    void SetStatusText(const std::string& text);
    void SetVersionProfile(const std::string& profile) {
        version_profile_ = profile;
    }
    void SetVersionText(const std::string& version);
};

class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
