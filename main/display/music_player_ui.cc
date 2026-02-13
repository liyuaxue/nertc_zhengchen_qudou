#include "music_player_ui.h"

#include "display.h"
#include "display/lcd_display.h"
#include "board.h"

#include <esp_log.h>
#include <font_awesome.h>
#include <lvgl.h>

extern "C" {
    extern const lv_img_dsc_t idle0;
}

static const char* TAG = "MusicPlayerUI";

MusicPlayerUI::MusicPlayerUI(LvglDisplay* display)
    : display_(display) {
}

MusicPlayerUI::~MusicPlayerUI() {
    Hide();
}

void MusicPlayerUI::Show() {
    if (is_visible_) {
        return;
    }

    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Display is null, cannot show music UI");
        return;
    }

    DisplayLockGuard lock(display_);

    if (theme_ == nullptr) {
        theme_ = static_cast<LvglTheme*>(display_->GetTheme());
    }

    CreateUI();
    is_visible_ = true;

    // 重置时间与进度
    elapsed_sec_ = 0;
    current_time_text_.assign("00:00");
    total_time_text_.assign("--:--");

    // 启动进度定时器（每秒刷新一次）
    if (progress_timer_ == nullptr) {
        progress_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* ui = static_cast<MusicPlayerUI*>(lv_timer_get_user_data(timer));
                if (ui != nullptr) {
                    ui->SetTimes("", ""); // 内部使用 elapsed_sec_ 刷新
                }
            },
            1000,
            this);
    }

    // 初始刷新一次状态栏和布局
    UpdateStatusBar();
    UpdateLayout();
}

void MusicPlayerUI::Hide() {
    if (!is_visible_) {
        return;
    }

    if (display_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(display_);

    if (progress_timer_ != nullptr) {
        lv_timer_del(progress_timer_);
        progress_timer_ = nullptr;
    }

    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    DestroyUI();
    is_visible_ = false;
}

void MusicPlayerUI::SetTheme(LvglTheme* theme) {
    theme_ = theme;
    if (!is_visible_ || theme_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(display_);

    auto text_font = theme_->text_font()->font();
    auto icon_font = theme_->icon_font()->font();
    auto large_icon_font = theme_->large_icon_font()->font();

    lv_color_t bg_color = theme_->background_color();
    lv_color_t text_color = theme_->text_color();

    // 根容器背景
    if (container_ != nullptr) {
        if (theme_->background_image() != nullptr) {
            lv_obj_set_style_bg_image_src(container_, theme_->background_image()->image_dsc(), 0);
        } else {
            lv_obj_set_style_bg_image_src(container_, nullptr, 0);
            lv_obj_set_style_bg_color(container_, bg_color, 0);
        }
    }

    // 顶部状态栏
    if (status_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(status_bar_, bg_color, 0);
        lv_obj_set_style_text_color(status_bar_, text_color, 0);
    }
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_color(network_label_, text_color, 0);
        lv_obj_set_style_text_font(network_label_,
                                   (text_font->line_height >= 40) ? large_icon_font : icon_font,
                                   0);
    }
    if (status_label_ != nullptr) {
        lv_obj_set_style_text_color(status_label_, text_color, 0);
        lv_obj_set_style_text_font(status_label_, text_font, 0);
    }
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_color(battery_label_, text_color, 0);
        lv_obj_set_style_text_font(battery_label_,
                                   (text_font->line_height >= 40) ? large_icon_font : icon_font,
                                   0);
    }

    // 歌曲标题 + 歌手
    if (title_label_ != nullptr) {
        lv_obj_set_style_text_color(title_label_, text_color, 0);
        lv_obj_set_style_text_font(title_label_, text_font, 0);
    }
    if (artist_label_ != nullptr) {
        lv_obj_set_style_text_color(artist_label_, text_color, 0);
        lv_obj_set_style_text_font(artist_label_, text_font, 0);
    }

    // 时间标签
    if (current_time_label_ != nullptr) {
        lv_obj_set_style_text_color(current_time_label_, text_color, 0);
        lv_obj_set_style_text_font(current_time_label_, text_font, 0);
    }
    if (total_time_label_ != nullptr) {
        lv_obj_set_style_text_color(total_time_label_, text_color, 0);
        lv_obj_set_style_text_font(total_time_label_, text_font, 0);
    }

    // 进度条颜色
    if (progress_bar_ != nullptr) {
        lv_obj_set_style_bg_color(progress_bar_, lv_color_hex(0x404040), LV_PART_MAIN);
        lv_obj_set_style_bg_color(progress_bar_, lv_color_hex(0x00FF7F), LV_PART_INDICATOR);
    }
}

void MusicPlayerUI::SetSongInfo(const std::string& title, const std::string& artist) {
    song_title_ = title;
    song_artist_ = artist;

    DisplayLockGuard lock(display_);
    ESP_LOGI(TAG, "SetSongInfo title='%s' artist='%s'", song_title_.c_str(), song_artist_.c_str());

    if (title_label_ != nullptr) {
        lv_label_set_text(title_label_, song_title_.c_str());
    }

    if (artist_label_ != nullptr) {
        if (!song_artist_.empty()) {
            lv_label_set_text(artist_label_, song_artist_.c_str());
            lv_obj_clear_flag(artist_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(artist_label_, "");
            lv_obj_add_flag(artist_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void MusicPlayerUI::SetProgress(float progress) {
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    progress_ = progress;

    if (!is_visible_ || progress_bar_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(display_);
    int32_t value = static_cast<int32_t>(progress_ * 100.0f + 0.5f);
    lv_bar_set_value(progress_bar_, value, LV_ANIM_OFF);
}

void MusicPlayerUI::SetTimes(const std::string& current_time, const std::string& total_time) {
   
    if (!current_time.empty()) {
        current_time_text_ = current_time;
    } else {
        
        elapsed_sec_++;
        int m = elapsed_sec_ / 60;
        int s = elapsed_sec_ % 60;
        
        char buf[9];
        snprintf(buf, sizeof(buf), "%02d:%02d", m % 100, s);
        current_time_text_.assign(buf);
    }

    if (!total_time.empty()) {
        total_time_text_ = total_time;
    } else if (total_sec_ <= 0) {
        total_time_text_ = "--:--";
    }

    if (!is_visible_) {
        return;
    }

    DisplayLockGuard lock(display_);
    if (current_time_label_ != nullptr) {
        lv_label_set_text(current_time_label_, current_time_text_.c_str());
    }
    if (total_time_label_ != nullptr) {
        lv_label_set_text(total_time_label_, total_time_text_.c_str());
    }

    // 根据当前与总时长更新进度条
    if (progress_bar_ != nullptr) {
        if (total_sec_ > 0) {
            float p = static_cast<float>(elapsed_sec_) / static_cast<float>(total_sec_);
            if (p < 0.0f) p = 0.0f;
            if (p > 1.0f) p = 1.0f;
            SetProgress(p);
        } else {
            // 未知总时长时，做一个比较缓慢的循环动画（约 200 秒一圈）
            int cycle = elapsed_sec_ % 200;
            float p = static_cast<float>(cycle) / 200.0f;
            SetProgress(p);
        }
    }

    UpdateStatusBar();
}

void MusicPlayerUI::SetTotalDurationSeconds(int total_sec) {
    if (total_sec <= 0) {
        return;
    }
    total_sec_ = total_sec;

    int m = total_sec_ / 60;
    int s = total_sec_ % 60;
    char buf[9];
    snprintf(buf, sizeof(buf), "%02d:%02d", m % 100, s);
    total_time_text_.assign(buf);

    if (!is_visible_) {
        return;
    }

    DisplayLockGuard lock(display_);
    if (total_time_label_ != nullptr) {
        lv_label_set_text(total_time_label_, total_time_text_.c_str());
    }
}

void MusicPlayerUI::CreateUI() {
    if (theme_ == nullptr) {
        theme_ = static_cast<LvglTheme*>(display_->GetTheme());
    }
    if (theme_ == nullptr) {
        ESP_LOGE(TAG, "Theme is null, cannot create music UI");
        return;
    }

    auto text_font = theme_->text_font()->font();
    auto icon_font = theme_->icon_font()->font();
    auto large_icon_font = theme_->large_icon_font()->font();

    // 如果在 UI 创建前已经通过 SetSongInfo 赋值，则优先使用真实的歌名/歌手
    std::string initial_title  = song_title_.empty()  ? "歌曲名称" : song_title_;
    std::string initial_artist = song_artist_.empty() ? "歌手"     : song_artist_;

    screen_ = lv_screen_active();

    // 根容器：全屏、禁止滚动
    container_ = lv_obj_create(screen_);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    if (theme_->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, theme_->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_color(container_, theme_->background_color(), 0);
    }

    // 顶部状态栏（与 lcd_display 类似：左网络，中间状态，右电池）
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(status_bar_, theme_->background_color(), 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, 4, 0);
    lv_obj_set_style_pad_bottom(status_bar_, 4, 0);
    lv_obj_set_style_pad_left(status_bar_, 8, 0);
    lv_obj_set_style_pad_right(status_bar_, 8, 0);
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_bar_, LV_OBJ_FLAG_SCROLLABLE);

    // 左：网络图标
    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_,
                               (text_font->line_height >= 40) ? large_icon_font : icon_font, 0);
    lv_obj_set_style_text_color(network_label_, theme_->text_color(), 0);

    // 中：状态文本
    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, theme_->text_color(), 0);
    lv_obj_set_style_text_font(status_label_, text_font, 0);
    lv_label_set_text(status_label_, "网易AI音乐");

    // 右：电池图标
    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_,
                               (text_font->line_height >= 40) ? large_icon_font : icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, theme_->text_color(), 0);

    // 内容区域（在状态栏下方，垂直布局）
    lv_obj_t* content = lv_obj_create(container_);
    lv_obj_set_size(content, LV_HOR_RES, LV_VER_RES - lv_obj_get_height(status_bar_));
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    int content_height = LV_VER_RES - lv_obj_get_height(status_bar_);

    // 1. 歌曲名称 + 歌手（分两行，超长滚动）
    lv_obj_t* song_container = lv_obj_create(content);
    lv_obj_set_width(song_container, LV_PCT(90));
    lv_obj_set_height(song_container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(song_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(song_container, 0, 0);
    lv_obj_set_style_pad_all(song_container, 0, 0);
    lv_obj_set_flex_flow(song_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(song_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(song_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_margin_top(song_container, content_height * 5 / 100 + 30, 0);

    // 标题
    title_label_ = lv_label_create(song_container);
    lv_obj_set_width(title_label_, LV_PCT(100));
    lv_obj_set_style_text_font(title_label_, text_font, 0);
    lv_obj_set_style_text_color(title_label_, theme_->text_color(), 0);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(title_label_, initial_title.c_str());

    // 歌手
    artist_label_ = lv_label_create(song_container);
    lv_obj_set_width(artist_label_, LV_PCT(100));
    lv_obj_set_style_text_font(artist_label_, text_font, 0);
    lv_obj_set_style_text_color(artist_label_, theme_->text_color(), 0);
    lv_obj_set_style_text_align(artist_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(artist_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(artist_label_, initial_artist.c_str());

    // 2. 进度条（居中）
    progress_bar_ = lv_bar_create(content);
    lv_obj_set_width(progress_bar_, LV_PCT(90));
    lv_obj_set_height(progress_bar_, 10);
    lv_bar_set_range(progress_bar_, 0, 100);
    lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(progress_bar_, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_bar_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_bar_, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar_, lv_color_hex(0x00FF7F), LV_PART_INDICATOR);
    lv_obj_set_style_margin_top(progress_bar_, content_height * 4 / 100, 0);
    lv_obj_align(progress_bar_, LV_ALIGN_CENTER, 0, 0);

    // 3. 时间行：左 当前，右 总时长
    time_container_ = lv_obj_create(content);
    lv_obj_set_width(time_container_, LV_PCT(90));
    lv_obj_set_height(time_container_, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_container_, 0, 0);
    lv_obj_set_style_pad_all(time_container_, 0, 0);
    lv_obj_set_flex_flow(time_container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_container_, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(time_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_margin_top(time_container_, content_height * 2 / 100, 0);

    current_time_label_ = lv_label_create(time_container_);
    lv_obj_set_style_text_font(current_time_label_, text_font, 0);
    lv_obj_set_style_text_color(current_time_label_, theme_->text_color(), 0);
    lv_label_set_text(current_time_label_, "00:00");

    total_time_label_ = lv_label_create(time_container_);
    lv_obj_set_style_text_font(total_time_label_, text_font, 0);
    lv_obj_set_style_text_color(total_time_label_, theme_->text_color(), 0);
    lv_label_set_text(total_time_label_, "00:00");

    // 4. idle0 GIF 居中显示（在底部区域）
    gif_image_ = lv_image_create(content);
    lv_obj_set_style_margin_top(gif_image_, content_height * 6 / 100 - 35, 0);
    lv_obj_align(gif_image_, LV_ALIGN_CENTER, 0, 0);

    // 使用 GIF 控制器播放 idle0 动画
    gif_controller_ = std::make_unique<LvglGif>(&idle0);
    if (gif_controller_ && gif_controller_->IsLoaded()) {
        gif_controller_->SetFrameCallback([this]() {
            if (gif_image_ != nullptr) {
                lv_image_set_src(gif_image_, gif_controller_->image_dsc());
            }
        });
        lv_image_set_src(gif_image_, gif_controller_->image_dsc());
        gif_controller_->Start();
    } else {
        ESP_LOGE(TAG, "Failed to load idle0 GIF for MusicPlayerUI");
        gif_controller_.reset();
        // 退化为静态显示
        lv_image_set_src(gif_image_, &idle0);
    }
}

void MusicPlayerUI::DestroyUI() {
    if (screen_ == nullptr) {
        return;
    }

    if (container_ != nullptr) {
        lv_obj_del(container_);
        container_ = nullptr;
    }

    status_bar_ = nullptr;
    network_label_ = nullptr;
    status_label_ = nullptr;
    battery_label_ = nullptr;
    title_label_ = nullptr;
    artist_label_ = nullptr;
    progress_bar_ = nullptr;
    time_container_ = nullptr;
    current_time_label_ = nullptr;
    total_time_label_ = nullptr;
    gif_image_ = nullptr;
}

void MusicPlayerUI::UpdateStatusBar() {
    if (!is_visible_) {
        return;
    }

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

void MusicPlayerUI::UpdateLayout() {
    // 当前布局主要依赖 flex + margin，创建时已设置，这里暂时不需要动态调整
}


