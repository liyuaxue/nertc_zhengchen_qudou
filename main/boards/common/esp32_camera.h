#ifndef ESP32_CAMERA_H
#define ESP32_CAMERA_H

#include <esp_camera.h>
#include <lvgl.h>
#include <thread>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>

#include "camera.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class Esp32Camera : public Camera {
private:
    camera_fb_t* fb_ = nullptr;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    
    // 预览相关成员变量
    std::atomic<bool> preview_running_{false};
    TaskHandle_t preview_task_handle_ = nullptr;
    static void PreviewTask(void* arg);
    void PreviewLoop();

public:
    Esp32Camera(const camera_config_t& config);
    ~Esp32Camera();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);
    virtual std::string ExtractExplanationText(const std::string& json_response);
    
    // 预览控制函数
    bool StartPreview();
    void StopPreview();
    bool IsPreviewRunning() const { return preview_running_; }
    bool IsPreviewRunningFlag() const { return preview_running_.load(); }
    virtual bool GetCapturedJpeg(uint8_t*& data, size_t& len) override;
};

#endif // ESP32_CAMERA_H