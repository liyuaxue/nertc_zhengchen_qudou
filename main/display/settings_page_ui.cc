#include "settings_page_ui.h"
#include "display.h"
#include "board.h"
#include "application.h"
#include "settings.h"
#include "boards/common/dual_network_board.h"
#include "boards/common/wifi_board.h"
#include "boards/common/esp32_camera.h"
#include "display/lcd_display.h"
#include "assets/lang_config.h"
#include "libs/qrcode/lv_qrcode.h"
#include "sdkconfig.h"

extern "C" {
lv_obj_t * lv_qrcode_create(lv_obj_t * parent);
void lv_qrcode_set_size(lv_obj_t * obj, int32_t size);
void lv_qrcode_set_dark_color(lv_obj_t * obj, lv_color_t color);
void lv_qrcode_set_light_color(lv_obj_t * obj, lv_color_t color);
lv_result_t lv_qrcode_update(lv_obj_t * obj, const void * data, uint32_t data_len);
}

#include <esp_log.h>
#include <esp_timer.h>
#include <font_awesome.h>
#include <lvgl.h>
#include <functional>

extern "C" {
    extern const lv_img_dsc_t icon_cam;
    extern const lv_img_dsc_t icon_WiFi;
    extern const lv_img_dsc_t icon_net;
    extern const lv_img_dsc_t icon_4G;
    extern const lv_img_dsc_t icon_4G_num;
    extern const lv_img_dsc_t icon_video;
    extern const lv_img_dsc_t icon_on;
    extern const lv_img_dsc_t icon_off;
    extern const lv_img_dsc_t icon_break;
}

#define TAG "SettingsPageUI"

SettingsPageUI::SettingsPageUI(LvglDisplay* display) 
    : display_(display), theme_(nullptr) {
    menu_items_.clear();
    
    esp_timer_create_args_t timer_args = {
        .callback = StatusBarUpdateTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "settings_status_bar_update",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &status_bar_update_timer_));
}

SettingsPageUI::~SettingsPageUI() {
    Hide();
    
    if (status_bar_update_timer_ != nullptr) {
        esp_timer_stop(status_bar_update_timer_);
        esp_timer_delete(status_bar_update_timer_);
        status_bar_update_timer_ = nullptr;
    }
}

void SettingsPageUI::UpdateNetworkMenuItem() {
    auto& board = Board::GetInstance();
#if CONFIG_NETWORK_MODE_WIFI_ONLY
    (void)this;
#else
    auto* dual_network_board = dynamic_cast<DualNetworkBoard*>(&board);
    if (dual_network_board) {
        NetworkType current_net = dual_network_board->GetNetworkType();
        const char* network_text = (current_net == NetworkType::WIFI) ? "切换为4G网络" : "切换为WiFi网络";
        const lv_img_dsc_t* network_icon = (current_net == NetworkType::WIFI) ? &icon_4G : &icon_WiFi;

        int network_item_index = 3; 
        if (network_item_index < static_cast<int>(menu_items_.size())) {
            UpdateMenuItemText(network_item_index, network_text);
            UpdateMenuItemIcon(network_item_index, network_icon);
        }
    }
#endif
}

void SettingsPageUI::Show() {
    if (is_visible_) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    
    if (theme_ == nullptr) {
        theme_ = static_cast<LvglTheme*>(display_->GetTheme());
    }
    
    InitializeMenuItems();
    
    if (menu_items_.empty()) {
        ESP_LOGW(TAG, "No menu items, cannot show settings page");
        return;
    }
    
    CreateUI();
    is_visible_ = true;
    
    UpdateStatusBar();
    
    if (status_bar_update_timer_ != nullptr) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(status_bar_update_timer_, 10000000));
    }
    
    if (container_ != nullptr) {
        lv_obj_move_foreground(container_);
    }
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}

void SettingsPageUI::Hide() {
    if (!is_visible_) {
        return;
    }
    
    if (status_bar_update_timer_ != nullptr) {
        esp_timer_stop(status_bar_update_timer_);
    }
    
    DisplayLockGuard lock(display_);
    DestroyUI();
    is_visible_ = false;
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
}

void SettingsPageUI::SetSelectedIndex(int index) {
    if (index < 0 || index >= static_cast<int>(menu_items_.size())) {
        return;
    }
    
    selected_index_ = index;
    
    if (!is_visible_) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    UpdateMenuItems();
}

void SettingsPageUI::SetTheme(LvglTheme* theme) {
    theme_ = theme;
    
    if (!is_visible_ || theme_ == nullptr) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    
    lv_color_t bg_color = theme_->background_color();
    lv_color_t text_color = theme_->text_color();
    
    if (container_ != nullptr) {
        if (theme_->background_image() != nullptr) {
            lv_obj_set_style_bg_image_src(container_, theme_->background_image()->image_dsc(), 0);
        } else {
            lv_obj_set_style_bg_color(container_, bg_color, 0);
        }
    }
    
    if (status_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(status_bar_, bg_color, 0);
        lv_obj_set_style_text_color(status_bar_, text_color, 0);
    }
    
    if (title_label_ != nullptr) {
        lv_obj_set_style_text_color(title_label_, text_color, 0);
    }
    
    auto text_font = theme_->text_font()->font();
    auto icon_font = theme_->icon_font()->font();
    auto large_icon_font = theme_->large_icon_font()->font();
    
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
    
    UpdateMenuItems();
    UpdateStatusBar();
}

void SettingsPageUI::SetMenuItemCallback(int index, std::function<void()> callback) {
    if (index >= 0 && index < static_cast<int>(menu_items_.size())) {
        menu_items_[index].callback = callback;
    }
}

void SettingsPageUI::UpdateMenuItemText(int index, const char* text) {
    if (index < 0 || index >= static_cast<int>(menu_items_.size()) || !is_visible_) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    
    if (index < static_cast<int>(menu_item_objects_.size())) {
        lv_obj_t* item = menu_item_objects_[index];
        lv_obj_t* text_label = static_cast<lv_obj_t*>(lv_obj_get_child(item, 1));
        if (text_label != nullptr && lv_obj_check_type(text_label, &lv_label_class)) {
            lv_label_set_text(text_label, text);
        }
    }
    
    menu_items_[index].text = text;
}

void SettingsPageUI::UpdateMenuItemIcon(int index, const lv_img_dsc_t* icon_image) {
    if (index < 0 || index >= static_cast<int>(menu_items_.size()) || !is_visible_) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    
    if (index < static_cast<int>(menu_item_objects_.size())) {
        lv_obj_t* item = menu_item_objects_[index];
        lv_obj_t* icon_obj = static_cast<lv_obj_t*>(lv_obj_get_child(item, 0));
        if (icon_obj != nullptr && lv_obj_check_type(icon_obj, &lv_image_class)) {
            lv_image_set_src(icon_obj, icon_image);
        }
    }
    
    menu_items_[index].icon_image = icon_image;
}

void SettingsPageUI::ShowProcessingPopup(const char* text) {
    DisplayLockGuard lock(display_);
    if (reboot_popup_ == nullptr || reboot_label_ == nullptr) {
        return;
    }
    lv_label_set_text(reboot_label_, text);
    lv_obj_clear_flag(reboot_popup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(reboot_popup_);
    lv_refr_now(nullptr);
}


void SettingsPageUI::AddMenuItem(const lv_img_dsc_t* icon_image, const char* icon_text, 
                                  const char* text, lv_color_t icon_color, 
                                  std::function<void()> callback) {
    MenuItem item;
    item.icon_image = icon_image;
    item.icon_text = icon_text;
    item.text = text;
    item.icon_color = icon_color;
    item.callback = callback;
    menu_items_.push_back(item);
}

void SettingsPageUI::ClearMenuItems() {
    menu_items_.clear();
}

void SettingsPageUI::InitializeMenuItems() {
    ClearMenuItems();
    
    auto& board = Board::GetInstance();
    auto& app = Application::GetInstance();
    
    auto* dual_network_board = dynamic_cast<DualNetworkBoard*>(&board);
    if (dual_network_board == nullptr) {
        ESP_LOGW(TAG, "Board is not DualNetworkBoard, cannot initialize menu items");
        return;
    }
    
    Settings settings("camera", true);
    bool camera_is_front = settings.GetBool("is_front", false);
    
    int menu_index = 0;
    const char* camera_text = camera_is_front ? "换成后置镜头" : "换成前置镜头";
    int camera_index = menu_index;
    AddMenuItem(&icon_cam, nullptr, camera_text, lv_color_hex(0xFFFFFF),
        [this, camera_index, &app]() {
            Settings settings("camera", true);
            bool camera_is_front = settings.GetBool("is_front", false);
            camera_is_front = !camera_is_front;
            settings.SetBool("is_front", camera_is_front);
            
            auto& board = Board::GetInstance();
            Camera* camera = board.GetCamera();
            if (camera != nullptr) {
                auto* esp32_camera = dynamic_cast<Esp32Camera*>(camera);
                if (esp32_camera != nullptr) {
                    esp32_camera->SetHMirror(!camera_is_front);
                    esp32_camera->SetVFlip(!camera_is_front);
                }
            }
            
            if (camera_is_front) {
                app.PlaySound(Lang::Sounds::OGG_CAM_FRONT);
            } else {
                app.PlaySound(Lang::Sounds::OGG_CAM_BACK);
            }
            
            const char* new_text = camera_is_front ? "换成后置镜头" : "换成前置镜头";
            UpdateMenuItemText(camera_index, new_text);
        });
    menu_index++;
    
    if (dual_network_board->GetNetworkType() == NetworkType::WIFI) {
        AddMenuItem(&icon_net, nullptr, "重新配置网络", lv_color_hex(0x00FF00),
            [this]() {
                ShowProcessingPopup("正在为您设置");
                Application::GetInstance().Schedule([]() {
                    auto& board = Board::GetInstance();
                    auto* dual_network_board = dynamic_cast<DualNetworkBoard*>(&board);
                    if (dual_network_board != nullptr) {
                        auto& current_board = dual_network_board->GetCurrentBoard();
                        auto* wifi_board = dynamic_cast<WifiBoard*>(&current_board);
                        if (wifi_board != nullptr) {
                            wifi_board->ResetWifiConfiguration();
                        }
                    }
                });
            });
        menu_index++;
    }
    
    int agent_interrupt_mode = app.GetAgentInterruptMode();
    const char* aec_text = (agent_interrupt_mode == 0) ? "打开语音打断" : "关闭语音打断";
    const lv_img_dsc_t* aec_icon = (agent_interrupt_mode == 0) ? &icon_on : &icon_off;
    AddMenuItem(aec_icon, nullptr, aec_text, lv_color_hex(0xFFA500),
        [this]() {
            ShowHintPopup("请在小程序端控制该功能\n\n并重启设备");
        });
    menu_index++;

    NetworkType current_net = dual_network_board->GetNetworkType();
#if CONFIG_NETWORK_MODE_WIFI_ONLY
    (void)this;
#else
    const char* network_text = (current_net == NetworkType::WIFI) ? "切换为4G网络" : "切换为WiFi网络";
    const lv_img_dsc_t* network_icon = (current_net == NetworkType::WIFI) ? &icon_4G : &icon_WiFi;
    AddMenuItem(network_icon, nullptr, network_text, lv_color_hex(0xFF69B4),
        [this]() {
            ShowProcessingPopup("正在为您设置");
            Application::GetInstance().Schedule([]() {
                auto& board = Board::GetInstance();
                auto* dual_network_board = dynamic_cast<DualNetworkBoard*>(&board);
                if (dual_network_board != nullptr) {
                    dual_network_board->SwitchNetworkType();
                }
            });
        });
#endif
    // 仅在 4G 模式下显示：查询 4G 卡号（ICCID）/ 显示二维码
    if (current_net == NetworkType::ML307) {
        AddMenuItem(&icon_4G_num, nullptr, "查询4G卡号", lv_color_hex(0xFF69B4),
            [this]() {
                DisplayLockGuard lock(display_);
                if (iccid_popup_ != nullptr && iccid_label_ != nullptr) {
                    lv_label_set_text(iccid_label_, "正在获取4G卡号...\n\n");
                    lv_obj_clear_flag(iccid_popup_, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(iccid_popup_);
                    if (iccid_hint_row_ != nullptr) {
                        lv_obj_add_flag(iccid_hint_row_, LV_OBJ_FLAG_HIDDEN);
                    }
                    if (iccid_qrcode_ != nullptr) {
                        lv_obj_add_flag(iccid_qrcode_, LV_OBJ_FLAG_HIDDEN);
                    }
                    lv_refr_now(nullptr);
                }

                auto* ui = this;
                Application::GetInstance().Schedule([ui]() {
                    std::string iccid;
                    auto& board = Board::GetInstance();
                    auto* dual_network_board = dynamic_cast<DualNetworkBoard*>(&board);
                    if (dual_network_board != nullptr) {
                        auto& current_board = dual_network_board->GetCurrentBoard();
                        auto* ml307_board = dynamic_cast<Ml307Board*>(&current_board);
                        if (ml307_board != nullptr) {
                            iccid = ml307_board->GetIccid();
                        }
                    }
                    if (iccid.empty()) {
                        iccid = "未知";
                    }

                    struct Payload {
                        SettingsPageUI* ui;
                        std::string iccid;
                    };
                    auto* payload = new Payload{ui, iccid};
                    lv_async_call([](void* data) {
                        auto* p = static_cast<Payload*>(data);
                        if (p && p->ui && p->ui->is_visible_ &&
                            p->ui->iccid_popup_ != nullptr && p->ui->iccid_label_ != nullptr) {

                            if (p->iccid == "未知") {
                                // 未能获取卡号，提示错误信息
                                lv_label_set_text(p->ui->iccid_label_, "未能获取4G卡号\n请检查网络后重试");
                                if (p->ui->iccid_qrcode_ != nullptr) {
                                    lv_obj_add_flag(p->ui->iccid_qrcode_, LV_OBJ_FLAG_HIDDEN);
                                }
                                if (p->ui->iccid_value_label_ != nullptr) {
                                    lv_label_set_text(p->ui->iccid_value_label_, "");
                                }
                            } else {
                                // 生成包含 ICCID 的登录链接二维码
                                std::string url = "http://wx.wwlelianiot.com/pages/login/index?iccid=" + p->iccid;

                                if (p->ui->iccid_qrcode_ != nullptr) {
                                    // 更新二维码内容
                                    lv_qrcode_update(p->ui->iccid_qrcode_, url.c_str(), url.size());
                                    lv_obj_clear_flag(p->ui->iccid_qrcode_, LV_OBJ_FLAG_HIDDEN);
                                }

                                lv_label_set_text(p->ui->iccid_label_, "请使用手机扫码\n查看/管理4G卡号");
                                if (p->ui->iccid_value_label_ != nullptr) {
                                    lv_label_set_text(p->ui->iccid_value_label_, p->iccid.c_str());
                                }
                            }

                            lv_obj_clear_flag(p->ui->iccid_popup_, LV_OBJ_FLAG_HIDDEN);
                            lv_obj_move_foreground(p->ui->iccid_popup_);
                            if (p->ui->iccid_hint_row_ != nullptr) {
                                lv_obj_clear_flag(p->ui->iccid_hint_row_, LV_OBJ_FLAG_HIDDEN);
                            }
                        }
                        delete p;
                    }, payload);
                });
            });
    }

    AddMenuItem(&icon_break, nullptr, "退出设置", lv_color_hex(0xFF3333),
        [this]() {
            Hide();
        });
}

bool SettingsPageUI::OnVolumeUp() {
    if (!is_visible_) {
        return false;
    }
    
    if (selected_index_ > 0) {
        SetSelectedIndex(selected_index_ - 1);
    } else {
        if (!menu_items_.empty()) {
            SetSelectedIndex(static_cast<int>(menu_items_.size()) - 1);
        }
    }
    return true;
}

bool SettingsPageUI::OnVolumeDown() {
    if (!is_visible_) {
        return false;
    }
    
    if (selected_index_ < static_cast<int>(menu_items_.size()) - 1) {
        SetSelectedIndex(selected_index_ + 1);
    } else {
        SetSelectedIndex(0);
    }
    return true;
}

bool SettingsPageUI::OnCameraClick() {
    if (!is_visible_) {
        return false;
    }
    
    // 若正在显示 4G 卡号弹窗，则优先关闭弹窗
    if (IsIccidPopupVisible()) {
        HideIccidPopup();
        return true;
    }

    // 若正在显示提示弹窗，则优先关闭弹窗
    if (IsHintPopupVisible()) {
        HideHintPopup();
        return true;
    }

    ConfirmSelectedItem();
    return true;
}

bool SettingsPageUI::IsIccidPopupVisible() const {
    if (!is_visible_ || iccid_popup_ == nullptr) {
        return false;
    }
    return !lv_obj_has_flag(iccid_popup_, LV_OBJ_FLAG_HIDDEN);
}

void SettingsPageUI::HideIccidPopup() {
    if (!is_visible_ || iccid_popup_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (iccid_qrcode_ != nullptr) {
        lv_obj_add_flag(iccid_qrcode_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(iccid_popup_, LV_OBJ_FLAG_HIDDEN);
}

bool SettingsPageUI::IsHintPopupVisible() const {
    if (!is_visible_ || hint_popup_ == nullptr) {
        return false;
    }
    return !lv_obj_has_flag(hint_popup_, LV_OBJ_FLAG_HIDDEN);
}

void SettingsPageUI::HideHintPopup() {
    if (!is_visible_ || hint_popup_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (hint_hint_row_ != nullptr) {
        lv_obj_add_flag(hint_hint_row_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(hint_popup_, LV_OBJ_FLAG_HIDDEN);
}

void SettingsPageUI::ShowHintPopup(const char* text) {
    if (!is_visible_ || hint_popup_ == nullptr || hint_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_label_set_text(hint_label_, text);
    if (hint_hint_row_ != nullptr) {
        lv_obj_clear_flag(hint_hint_row_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(hint_popup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(hint_popup_);
    lv_refr_now(nullptr);
}

void SettingsPageUI::ConfirmSelectedItem() {
    if (selected_index_ < 0 || selected_index_ >= static_cast<int>(menu_items_.size())) {
        return;
    }
    
    if (menu_items_[selected_index_].callback) {
        menu_items_[selected_index_].callback();
    }
}

void SettingsPageUI::MenuItemClickCallback(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    SettingsPageUI* ui = static_cast<SettingsPageUI*>(lv_event_get_user_data(e));
    
    if (ui == nullptr) {
        return;
    }
    
    for (size_t i = 0; i < ui->menu_item_objects_.size(); i++) {
        if (ui->menu_item_objects_[i] == obj) {
            if (i < ui->menu_items_.size() && ui->menu_items_[i].callback) {
                ui->menu_items_[i].callback();
            }
            break;
        }
        int child_count = lv_obj_get_child_cnt(ui->menu_item_objects_[i]);
        for (int j = 0; j < child_count; j++) {
            lv_obj_t* child = static_cast<lv_obj_t*>(lv_obj_get_child(ui->menu_item_objects_[i], j));
            if (child == obj) {
                if (i < ui->menu_items_.size() && ui->menu_items_[i].callback) {
                    ui->menu_items_[i].callback();
                }
                return;
            }
        }
    }
}

void SettingsPageUI::CreateUI() {
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
    
    screen_ = lv_screen_active();
    
    container_ = lv_obj_create(screen_);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(container_, 0, 0);
    lv_obj_move_background(container_);
    
    if (theme_->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, theme_->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_color(container_, theme_->background_color(), 0);
    }
    
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
    
    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, 
        (text_font->line_height >= 40) ? large_icon_font : icon_font, 0);
    lv_obj_set_style_text_color(network_label_, theme_->text_color(), 0);
    
    title_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(title_label_, 1);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title_label_, theme_->text_color(), 0);
    lv_obj_set_style_text_font(title_label_, text_font, 0);
    lv_label_set_text(title_label_, "系统设置");
    
    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, 
        (text_font->line_height >= 40) ? large_icon_font : icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, theme_->text_color(), 0);
    
    lv_obj_t* content = lv_obj_create(container_);
    int status_bar_height = 24;
    lv_obj_set_size(content, LV_HOR_RES, LV_VER_RES - status_bar_height);
    lv_obj_set_y(content, status_bar_height);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    
    menu_container_ = lv_obj_create(content);
    lv_obj_set_width(menu_container_, LV_PCT(90));
    lv_obj_set_height(menu_container_, LV_VER_RES - status_bar_height - 8);
    lv_obj_set_style_radius(menu_container_, 0, 0);
    lv_obj_set_style_pad_all(menu_container_, 0, 0);
    lv_obj_set_style_border_width(menu_container_, 0, 0);
    lv_obj_set_style_bg_opa(menu_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(menu_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(menu_container_, 1, 0);
    lv_obj_set_style_pad_top(menu_container_, 4, 0); 
    lv_obj_set_style_pad_bottom(menu_container_, 4, 0);  
    lv_obj_center(menu_container_);
    lv_obj_add_flag(menu_container_, LV_OBJ_FLAG_SCROLLABLE);  
    lv_obj_set_scrollbar_mode(menu_container_, LV_SCROLLBAR_MODE_OFF);

    reboot_popup_ = lv_obj_create(container_);
    lv_obj_set_size(reboot_popup_, LV_PCT(80), 70);
    lv_obj_align(reboot_popup_, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(reboot_popup_, 12, 0);
    lv_obj_set_style_bg_opa(reboot_popup_, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(reboot_popup_, lv_color_black(), 0);
    lv_obj_set_style_border_width(reboot_popup_, 0, 0);
    lv_obj_set_style_pad_all(reboot_popup_, 12, 0);
    reboot_label_ = lv_label_create(reboot_popup_);
    lv_obj_set_style_text_color(reboot_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(reboot_label_, text_font, 0);
    lv_label_set_text(reboot_label_, "正在为您设置");
    lv_obj_center(reboot_label_);
    lv_obj_add_flag(reboot_popup_, LV_OBJ_FLAG_HIDDEN);


    iccid_popup_ = lv_obj_create(container_);
    lv_obj_set_width(iccid_popup_, LV_PCT(84));
    lv_obj_set_height(iccid_popup_, LV_SIZE_CONTENT);
    lv_obj_align(iccid_popup_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(iccid_popup_, 20, 0);
    lv_obj_set_style_bg_opa(iccid_popup_, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(iccid_popup_, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_border_width(iccid_popup_, 0, 0);
    lv_obj_set_style_pad_all(iccid_popup_, 14, 0);
    lv_obj_clear_flag(iccid_popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(iccid_popup_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(iccid_popup_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 上方提示文案
    iccid_label_ = lv_label_create(iccid_popup_);
    lv_obj_set_style_text_color(iccid_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(iccid_label_, text_font, 0);
    lv_obj_set_style_text_align(iccid_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(iccid_label_, "正在搜索当前卡号...\n\n");
    lv_obj_set_width(iccid_label_, LV_PCT(100));

    // 中间二维码
    const int qr_size = 120;  
    iccid_qrcode_ = lv_qrcode_create(iccid_popup_);
    lv_qrcode_set_size(iccid_qrcode_, qr_size);
    lv_qrcode_set_dark_color(iccid_qrcode_, lv_color_black());
    lv_qrcode_set_light_color(iccid_qrcode_, lv_color_white());
    lv_obj_add_flag(iccid_qrcode_, LV_OBJ_FLAG_HIDDEN);

    // 下方显示 ICCID 本身
    iccid_value_label_ = lv_label_create(iccid_popup_);
    lv_obj_set_style_text_color(iccid_value_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(iccid_value_label_, text_font, 0);
    lv_obj_set_style_text_align(iccid_value_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(iccid_value_label_, "");
    lv_obj_set_width(iccid_value_label_, LV_PCT(100));

    iccid_hint_row_ = lv_obj_create(iccid_popup_);
    lv_obj_set_size(iccid_hint_row_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(iccid_hint_row_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(iccid_hint_row_, 0, 0);
    lv_obj_set_style_pad_all(iccid_hint_row_, 0, 0);
    lv_obj_set_style_pad_column(iccid_hint_row_, 8, 0);
    lv_obj_clear_flag(iccid_hint_row_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(iccid_hint_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(iccid_hint_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    iccid_hint_icon_ = lv_img_create(iccid_hint_row_);
    lv_img_set_src(iccid_hint_icon_, &icon_video);
    lv_obj_set_size(iccid_hint_icon_, 16, 16);

    iccid_hint_label_ = lv_label_create(iccid_hint_row_);
    lv_obj_set_style_text_color(iccid_hint_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(iccid_hint_label_, text_font, 0);
    lv_label_set_text(iccid_hint_label_, "返回设置");

    lv_obj_add_flag(iccid_hint_row_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(iccid_popup_, LV_OBJ_FLAG_HIDDEN);

    // 提示弹窗
    hint_popup_ = lv_obj_create(container_);
    lv_obj_set_width(hint_popup_, LV_PCT(84));
    lv_obj_set_height(hint_popup_, LV_SIZE_CONTENT);
    lv_obj_align(hint_popup_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(hint_popup_, 20, 0);
    lv_obj_set_style_bg_opa(hint_popup_, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(hint_popup_, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_border_width(hint_popup_, 0, 0);
    lv_obj_set_style_pad_all(hint_popup_, 14, 0);
    lv_obj_clear_flag(hint_popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hint_popup_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hint_popup_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    hint_label_ = lv_label_create(hint_popup_);
    lv_obj_set_style_text_color(hint_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(hint_label_, text_font, 0);
    lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint_label_, "");
    lv_obj_set_width(hint_label_, LV_PCT(100));

    hint_hint_row_ = lv_obj_create(hint_popup_);
    lv_obj_set_size(hint_hint_row_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hint_hint_row_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hint_hint_row_, 0, 0);
    lv_obj_set_style_pad_all(hint_hint_row_, 0, 0);
    lv_obj_set_style_pad_column(hint_hint_row_, 8, 0);
    lv_obj_clear_flag(hint_hint_row_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hint_hint_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint_hint_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    hint_hint_icon_ = lv_img_create(hint_hint_row_);
    lv_img_set_src(hint_hint_icon_, &icon_video);
    lv_obj_set_size(hint_hint_icon_, 16, 16);

    hint_hint_label_ = lv_label_create(hint_hint_row_);
    lv_obj_set_style_text_color(hint_hint_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(hint_hint_label_, text_font, 0);
    lv_label_set_text(hint_hint_label_, "返回设置");

    lv_obj_add_flag(hint_popup_, LV_OBJ_FLAG_HIDDEN);
    
    menu_item_objects_.clear();
    for (size_t i = 0; i < menu_items_.size(); i++) {
        lv_obj_t* item = lv_obj_create(menu_container_);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 40); 
        lv_obj_set_style_radius(item, 10, 0); 
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(item, 12, 0); 
        lv_obj_set_style_pad_right(item, 12, 0); 
        lv_obj_set_style_pad_column(item, 10, 0); 
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        
        lv_obj_add_event_cb(item, MenuItemClickCallback, LV_EVENT_CLICKED, this);
        
        lv_obj_t* icon_obj = nullptr;
        if (menu_items_[i].icon_image != nullptr) {
            icon_obj = lv_image_create(item);
            lv_image_set_src(icon_obj, menu_items_[i].icon_image);
            lv_obj_set_size(icon_obj, 20, 20);
        } else if (menu_items_[i].icon_text != nullptr) {
            icon_obj = lv_label_create(item);
            lv_obj_set_style_text_font(icon_obj, 
                (text_font->line_height >= 40) ? large_icon_font : icon_font, 0);
            lv_obj_set_style_text_color(icon_obj, menu_items_[i].icon_color, 0);
            lv_label_set_text(icon_obj, menu_items_[i].icon_text);
        }
        
        lv_obj_t* text_label = lv_label_create(item);
        lv_obj_set_style_text_font(text_label, text_font, 0);
        lv_obj_set_style_text_color(text_label, theme_->text_color(), 0);
        lv_label_set_text(text_label, menu_items_[i].text);
        lv_obj_set_flex_grow(text_label, 1);
        
        menu_item_objects_.push_back(item);
    }
    
    UpdateMenuItems();
}

void SettingsPageUI::UpdateStatusBar() {
    if (!is_visible_ || status_bar_ == nullptr) {
        return;
    }
    
    DisplayLockGuard lock(display_);
    
    auto& board = Board::GetInstance();
    
    const char* network_icon = board.GetNetworkStateIcon();
    if (network_label_ != nullptr && network_icon != nullptr) {
        lv_label_set_text(network_label_, network_icon);
    }
    
    int battery_level;
    bool charging, discharging;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        const char* battery_icon = nullptr;
        if (charging) {
            battery_icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY,
                FONT_AWESOME_BATTERY_QUARTER,
                FONT_AWESOME_BATTERY_HALF,
                FONT_AWESOME_BATTERY_THREE_QUARTERS,
                FONT_AWESOME_BATTERY_FULL,
                FONT_AWESOME_BATTERY_FULL,
            };
            battery_icon = levels[battery_level / 20];
        }
        if (battery_label_ != nullptr && battery_icon != nullptr) {
            lv_label_set_text(battery_label_, battery_icon);
        }
    }
}

void SettingsPageUI::StatusBarUpdateTimerCallback(void* arg) {
    SettingsPageUI* ui = static_cast<SettingsPageUI*>(arg);
    if (ui != nullptr) {
        lv_async_call([](void* data) {
            auto* ui = static_cast<SettingsPageUI*>(data);
            if (ui) {
                ui->UpdateStatusBar();
            }
        }, ui);
    }
}

void SettingsPageUI::DestroyUI() {
    if (menu_container_ != nullptr) {
        lv_obj_del(menu_container_);
        menu_container_ = nullptr;
    }
    
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
        status_bar_ = nullptr;
    }

    if (reboot_popup_ != nullptr) {
        lv_obj_del(reboot_popup_);
        reboot_popup_ = nullptr;
        reboot_label_ = nullptr;
    }

    if (iccid_popup_ != nullptr) {
        lv_obj_del(iccid_popup_);
        iccid_popup_ = nullptr;
        iccid_label_ = nullptr;
        iccid_value_label_ = nullptr;
        iccid_qrcode_ = nullptr;
        iccid_hint_row_ = nullptr;
        iccid_hint_icon_ = nullptr;
        iccid_hint_label_ = nullptr;
    }

    if (hint_popup_ != nullptr) {
        lv_obj_del(hint_popup_);
        hint_popup_ = nullptr;
        hint_label_ = nullptr;
        hint_hint_row_ = nullptr;
        hint_hint_icon_ = nullptr;
        hint_hint_label_ = nullptr;
    }
    
    network_label_ = nullptr;
    title_label_ = nullptr;
    battery_label_ = nullptr;
    
    if (container_ != nullptr) {
        lv_obj_del(container_);
        container_ = nullptr;
    }
    
    menu_item_objects_.clear();
    screen_ = nullptr;
}

void SettingsPageUI::UpdateMenuItems() {
    if (menu_item_objects_.empty() || theme_ == nullptr) {
        return;
    }
    
    for (size_t i = 0; i < menu_item_objects_.size() && i < menu_items_.size(); i++) {
        lv_obj_t* item = menu_item_objects_[i];
        
        if (static_cast<int>(i) == selected_index_) {
            lv_obj_set_style_bg_color(item, lv_color_hex(0x4B0082), 0);
            lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        }
        
        lv_obj_t* text_label = static_cast<lv_obj_t*>(lv_obj_get_child(item, 1));
        if (text_label != nullptr && lv_obj_check_type(text_label, &lv_label_class)) {
            if (static_cast<int>(i) == selected_index_) {
                lv_obj_set_style_text_color(text_label, lv_color_white(), 0);
            } else {
                lv_obj_set_style_text_color(text_label, theme_->text_color(), 0);
            }
        }
    }
}
