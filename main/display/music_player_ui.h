#ifndef MUSIC_PLAYER_UI_H
#define MUSIC_PLAYER_UI_H

#include "lvgl_display.h"
#include "lvgl_theme.h"
#include "gif/lvgl_gif.h"

#include <lvgl.h>
#include <string>
#include <memory>

class MusicPlayerUI {
public:
    explicit MusicPlayerUI(LvglDisplay* display);
    ~MusicPlayerUI();

    // 显示 / 隐藏音乐播放 UI
    void Show();
    void Hide();

    // 是否可见
    bool IsVisible() const { return is_visible_; }

    // 更新主题（跟随整体主题变化）
    void SetTheme(LvglTheme* theme);

    // 设置歌曲信息：标题 + 歌手
    void SetSongInfo(const std::string& title, const std::string& artist);

    // 设置进度（0.0f ~ 1.0f）
    void SetProgress(float progress);

    // 设置当前时间与总时长（格式如 "01:23"）
    void SetTimes(const std::string& current_time, const std::string& total_time);

    // 设置总时长（秒），仅更新右侧总时间显示和内部 total_sec_
    void SetTotalDurationSeconds(int total_sec);

private:
    void CreateUI();
    void DestroyUI();
    void UpdateStatusBar();
    void UpdateLayout();

    LvglDisplay* display_;
    LvglTheme* theme_ = nullptr;

    // 根容器 + 顶部状态栏
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;

    // 中部内容
    lv_obj_t* title_label_ = nullptr;         // 歌曲名称
    lv_obj_t* artist_label_ = nullptr;        // 歌手
    lv_obj_t* progress_bar_ = nullptr;        // 播放进度条
    lv_obj_t* time_container_ = nullptr;      // 当前时间 / 总时间 容器
    lv_obj_t* current_time_label_ = nullptr;  // 当前进度时间
    lv_obj_t* total_time_label_ = nullptr;    // 总时长
    lv_obj_t* gif_image_ = nullptr;           // 显示

    // GIF 控制器
    std::unique_ptr<LvglGif> gif_controller_;

    // 进度与时间
    std::string song_title_;
    std::string song_artist_;
    std::string current_time_text_;
    std::string total_time_text_;
    float progress_ = 0.0f;
    int elapsed_sec_ = 0;
    int total_sec_ = 0;          // 目前未知，总时间为 0 时仅显示 "--:--"
    lv_timer_t* progress_timer_ = nullptr;

    bool is_visible_ = false;
};

#endif // MUSIC_PLAYER_UI_H


