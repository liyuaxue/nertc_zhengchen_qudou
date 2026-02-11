#ifndef SETTINGS_PAGE_UI_H
#define SETTINGS_PAGE_UI_H

#include "lvgl_display.h"
#include "lvgl_theme.h"

#include <lvgl.h>
#include <esp_timer.h>
#include <vector>
#include <functional>

/**
 * 设置页面UI类
 * 功能：
 * 1. 显示设置菜单（灵动岛）
 * 2. 包含多个设置选项
 * 3. 不影响状态栏
 */
class SettingsPageUI {
public:
    SettingsPageUI(LvglDisplay* display);
    ~SettingsPageUI();

    // 显示设置页面
    void Show();
    
    // 隐藏设置页面
    void Hide();
    
    // 设置当前选中的菜单项索引（0-based）
    void SetSelectedIndex(int index);
    
    // 获取当前选中的菜单项索引
    int GetSelectedIndex() const { return selected_index_; }
    
    // 检查是否可见
    bool IsVisible() const { return is_visible_; }
    
    // 设置主题
    void SetTheme(LvglTheme* theme);
    
    // 设置菜单项点击回调
    void SetMenuItemCallback(int index, std::function<void()> callback);
    
    // 更新菜单项文本
    void UpdateMenuItemText(int index, const char* text);
    
    // 更新菜单项图标
    void UpdateMenuItemIcon(int index, const lv_img_dsc_t* icon_image);
    
    
    // 添加菜单项
    void AddMenuItem(const lv_img_dsc_t* icon_image, const char* icon_text, 
                     const char* text, lv_color_t icon_color, 
                     std::function<void()> callback = nullptr);
    
    // 清空菜单项
    void ClearMenuItems();
    
    // 初始化菜单项（从Board获取信息）
    void InitializeMenuItems();
    
    bool OnVolumeUp();
    bool OnVolumeDown();
    bool OnCameraClick();

    // 4G 卡号弹窗
    bool IsIccidPopupVisible() const;
    void HideIccidPopup();
    
    // 提示弹窗
    bool IsHintPopupVisible() const;
    void HideHintPopup();
    void ShowHintPopup(const char* text);
    
    void ConfirmSelectedItem();

private:
    struct MenuItem {
        const lv_img_dsc_t* icon_image; 
        const char* icon_text;           
        const char* text;
        lv_color_t icon_color;
        std::function<void()> callback;   
    };
    
    void CreateUI();
    void DestroyUI();
    void UpdateMenuItems();
    void ShowProcessingPopup(const char* text);
    static void MenuItemClickCallback(lv_event_t* e);
    void UpdateNetworkMenuItem();
    
    std::vector<MenuItem> menu_items_;  
    
    LvglDisplay* display_;
    LvglTheme* theme_;
    
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* menu_container_ = nullptr;
    lv_obj_t* reboot_popup_ = nullptr;
    lv_obj_t* reboot_label_ = nullptr;
    lv_obj_t* iccid_popup_ = nullptr;
    lv_obj_t* iccid_label_ = nullptr;
    lv_obj_t* iccid_value_label_ = nullptr;
    lv_obj_t* iccid_qrcode_ = nullptr;
    lv_obj_t* iccid_hint_row_ = nullptr;
    lv_obj_t* iccid_hint_icon_ = nullptr;
    lv_obj_t* iccid_hint_label_ = nullptr;
    lv_obj_t* hint_popup_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* hint_hint_row_ = nullptr;
    lv_obj_t* hint_hint_icon_ = nullptr;
    lv_obj_t* hint_hint_label_ = nullptr;
    std::vector<lv_obj_t*> menu_item_objects_;
    
    int selected_index_ = 0;
    bool is_visible_ = false;
    
    void UpdateStatusBar();
    
    esp_timer_handle_t status_bar_update_timer_ = nullptr;
    static void StatusBarUpdateTimerCallback(void* arg);
};

#endif // SETTINGS_PAGE_UI_H

