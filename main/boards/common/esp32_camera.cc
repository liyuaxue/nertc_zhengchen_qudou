#include "esp32_camera.h"
#include "board.h"
#include "system_info.h"
#include "lvgl_display.h"
#include "display/clock_desktop_ui.h"
#include "display/lcd_display.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <img_converters.h>
#include <cstring>

#define TAG "Esp32Camera"

Esp32Camera::Esp32Camera(const camera_config_t& config) {
    // camera init
    esp_err_t err = esp_camera_init(&config); 
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == GC2145_PID) {
        s->set_hmirror(s, 1); 
    }

     // ===== 暗光环境下减少“绿点”噪声的一些调参 =====
    // 限制增益上限，避免特别高的模拟增益带来大量彩色噪点
    s->set_gainceiling(s, GAINCEILING_4X);      // 默认一般是 8X，可根据效果改成 2X/4X/8X 试

    // 自动曝光、自动增益打开
    s->set_exposure_ctrl(s, 1);                 // 开 AEC
    s->set_aec2(s, 1);                          // 使用改进版 AEC 算法（有的 sensor 支持）
    s->set_gain_ctrl(s, 1);                     // 开 AGC

    // 稍微压一点整体亮度，换取更少的噪点
    s->set_ae_level(s, -1);                     // -2 ~ +2，越高越亮噪点越多
    s->set_brightness(s, 0);                    // 不要拉太高

    // 对比度略提高，饱和度略降低，可以减弱“绿点”观感
    s->set_contrast(s, 1);                      // -2 ~ +2
    s->set_saturation(s, -1);                   // -2 ~ +2，负值略微去色

    // 如果驱动支持降噪接口就打开
    if (s->set_denoise) {
        s->set_denoise(s, 1);                   // 1 打开，具体强度看驱动实现
    }

    // 自动白平衡打开，避免偏绿
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);

}

Esp32Camera::~Esp32Camera() {
    StopPreview();
    if (fb_) {
        esp_camera_fb_return(fb_);
        fb_ = nullptr;
    }
    esp_camera_deinit();
}

void Esp32Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    auto start_time = esp_timer_get_time();
    int frames_to_get = 2;
    // Try to get a stable frame
    for (int i = 0; i < frames_to_get; i++) {
        if (fb_ != nullptr) {
            esp_camera_fb_return(fb_);
        }
        fb_ = esp_camera_fb_get();
        if (fb_ == nullptr) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }
    }
    auto end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "Camera captured %d frames in %d ms", frames_to_get, int((end_time - start_time) / 1000));

    // 显示预览图片
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display != nullptr) {
        auto data = (uint8_t*)heap_caps_malloc(fb_->len, MALLOC_CAP_SPIRAM);
        if (data == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for preview image");
            return false;
        }

        auto src = (uint16_t*)fb_->buf;
        auto dst = (uint16_t*)data;
        size_t pixel_count = fb_->len / 2;
        for (size_t i = 0; i < pixel_count; i++) {
            // 交换每个16位字内的字节
            dst[i] = __builtin_bswap16(src[i]);
        }

        auto image = std::make_unique<LvglAllocatedImage>(data, fb_->len, fb_->width, fb_->height, fb_->width * 2, LV_COLOR_FORMAT_RGB565);
        display->SetPreviewImage(std::move(image));
    }
    return true;
}

bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return false;
    }
    
    esp_err_t err = s->set_hmirror(s, enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set horizontal mirror: %d", err);
        return false;
    }
    
    // 如果预览正在运行，清空当前帧缓冲区，让下一帧使用新设置
    if (preview_running_) {
        // 丢弃当前帧，强制获取新帧
        camera_fb_t* frame = esp_camera_fb_get();
        if (frame != nullptr) {
            esp_camera_fb_return(frame);
        }
    }
    
    ESP_LOGI(TAG, "Camera horizontal mirror set to: %s", enabled ? "enabled" : "disabled");
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return false;
    }
    
    esp_err_t err = s->set_vflip(s, enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set vertical flip: %d", err);
        return false;
    }
    
    ESP_LOGI(TAG, "Camera vertical flip set to: %s", enabled ? "enabled" : "disabled");
    return true;
}

/**
 * @brief 将摄像头捕获的图像发送到远程服务器进行AI分析和解释
 * 
 * 该函数将当前摄像头缓冲区中的图像编码为JPEG格式，并通过HTTP POST请求
 * 以multipart/form-data的形式发送到指定的解释服务器。服务器将根据提供的
 * 问题对图像进行AI分析并返回结果。
 * 
 * 实现特点：
 * - 使用独立线程编码JPEG，与主线程分离
 * - 采用分块传输编码(chunked transfer encoding)优化内存使用
 * - 通过队列机制实现编码线程和发送线程的数据同步
 * - 支持设备ID、客户端ID和认证令牌的HTTP头部配置
 * 
 * @param question 要向AI提出的关于图像的问题，将作为表单字段发送
 * @return std::string 服务器返回的JSON格式响应字符串
 *         成功时包含AI分析结果，失败时包含错误信息
 *         格式示例：{"success": true, "result": "分析结果"}
 *                  {"success": false, "message": "错误信息"}
 * 
 * @note 调用此函数前必须先调用SetExplainUrl()设置服务器URL
 * @note 函数会等待之前的编码线程完成后再开始新的处理
 * @warning 如果摄像头缓冲区为空或网络连接失败，将返回错误信息
 */
std::string Esp32Camera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }

    // 创建局部的 JPEG 队列, 40 entries is about to store 512 * 40 = 20480 bytes of JPEG data
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        throw std::runtime_error("Failed to create JPEG queue");
    }

    // We spawn a thread to encode the image to JPEG
    encoder_thread_ = std::thread([this, jpeg_queue]() {
        frame2jpg_cb(fb_, 80, [](void* arg, size_t index, const void* data, size_t len) -> unsigned int {
            auto jpeg_queue = (QueueHandle_t)arg;
            JpegChunk chunk = {
                .data = (uint8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM),
                .len = len
            };
            memcpy(chunk.data, data, len);
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
            return len;
        }, jpeg_queue);
    });

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    // 构造multipart/form-data请求体
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    // 配置HTTP客户端，使用分块传输编码
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        // Clear the queue
        encoder_thread_.join();
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        vQueueDelete(jpeg_queue);
        throw std::runtime_error("Failed to connect to explain URL");
    }
    
    {
        // 第一块：question字段
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        // 第二块：文件字段头部
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    // 第三块：JPEG数据
    size_t total_sent = 0;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            break; // The last chunk
        }
        http->Write((const char*)chunk.data, chunk.len);
        total_sent += chunk.len;
        heap_caps_free(chunk.data);
    }
    // Wait for the encoder thread to finish
    encoder_thread_.join();
    // 清理队列
    vQueueDelete(jpeg_queue);

    {
        // 第四块：multipart尾部
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    // 结束块
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());
        throw std::runtime_error("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    // Get remain task stack size
    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
        fb_->width, fb_->height, total_sent, remain_stack_size, question.c_str(), result.c_str());
    return result;
}

void Esp32Camera::PreviewTask(void* arg) {
    Esp32Camera* camera = static_cast<Esp32Camera*>(arg);
    camera->PreviewLoop();
    vTaskDelete(NULL);
}

void Esp32Camera::PreviewLoop() {
    ESP_LOGI(TAG, "Camera preview task started");
    
    while (preview_running_) {
        camera_fb_t* frame = esp_camera_fb_get();
        if (frame == nullptr) {
            ESP_LOGW(TAG, "Camera frame buffer is null");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        

        auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
        if (display != nullptr) {
            auto data = (uint8_t*)heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM);
            if (data != nullptr) {
                auto src = (uint16_t*)frame->buf;
                auto dst = (uint16_t*)data;
                size_t pixel_count = frame->len / 2;
                for (size_t i = 0; i < pixel_count; i++) {
                    dst[i] = __builtin_bswap16(src[i]);
                }
                
                auto image = std::make_unique<LvglAllocatedImage>(data, frame->len, frame->width, frame->height, frame->width * 2, LV_COLOR_FORMAT_RGB565);
                display->SetPreviewImage(std::move(image));
            } else {
                ESP_LOGW(TAG, "Failed to allocate memory for preview frame");
            }
        }
        
        // 释放帧缓冲区
        esp_camera_fb_return(frame);
        
        // 控制帧率，约15-20 FPS
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
    ESP_LOGI(TAG, "Camera preview task stopped");
}

bool Esp32Camera::StartPreview() {
    if (preview_running_) {
        ESP_LOGW(TAG, "Preview is already running");
        return true;
    }
    
    // 设置摄像头预览标记位，禁止时钟UI显示，并开启预览提示条，同时关闭唤醒词检测
    ClockDesktopUI::SetCameraPreviewActive(true);
    LcdDisplay::SetCameraPreviewHideBottomBar(true);
    LcdDisplay::SetCameraPreviewHintEnabled(true);
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);

    auto* lcd_display = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
    if (lcd_display != nullptr) {
        auto* clock_ui = lcd_display->GetClockDesktopUI();
        if (clock_ui != nullptr) {
            clock_ui->Hide();
        }
    }
    
    
    preview_running_ = true;
    
    // 创建预览任务
    BaseType_t ret = xTaskCreate(
        PreviewTask,
        "camera_preview",
        4096,  // 堆栈大小
        this,
        5,     // 优先级
        &preview_task_handle_
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create preview task");
        preview_running_ = false;
        return false;
    }
    
    ESP_LOGI(TAG, "Camera preview started");
    return true;
}

void Esp32Camera::StopPreview() {
    if (!preview_running_) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping camera preview");
    preview_running_ = false;
    
    if (preview_task_handle_ != nullptr) {
        int timeout_ms = 2000;
        while (timeout_ms > 0) {
            eTaskState state = eTaskGetState(preview_task_handle_);
            if (state == eDeleted || state == eInvalid) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout_ms -= 100;
        }
        preview_task_handle_ = nullptr;
    }
    

    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display != nullptr) {
        display->SetPreviewImage(nullptr);
    }
    
    // 清除摄像头预览标记位，允许时钟UI显示
    ClockDesktopUI::SetCameraPreviewActive(false);
    LcdDisplay::SetCameraPreviewHideBottomBar(false);
    LcdDisplay::SetCameraPreviewHintEnabled(false);
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    
    ESP_LOGI(TAG, "Camera preview stopped");
}


std::string Esp32Camera::ExtractExplanationText(const std::string& json_response) {

    cJSON* root = cJSON_Parse(json_response.c_str());
    if (root == nullptr) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != nullptr) {
            ESP_LOGE(TAG, "JSON parse error before: %s", error_ptr);
        }
        throw std::runtime_error("Failed to parse JSON response");
    }
    
    std::string explanation_text;
    
    // 检查success字段
    cJSON* success_item = cJSON_GetObjectItem(root, "success");
    if (success_item == nullptr || !cJSON_IsBool(success_item)) {
        cJSON_Delete(root);
        throw std::runtime_error("Missing or invalid 'success' field in response");
    }
    
    if (!cJSON_IsTrue(success_item)) {
        cJSON* message_item = cJSON_GetObjectItem(root, "message");
        if (message_item != nullptr && cJSON_IsString(message_item)) {
            std::string error_msg = "AI analysis failed: ";
            error_msg += message_item->valuestring;
            cJSON_Delete(root);
            throw std::runtime_error(error_msg);
        } else {
            cJSON_Delete(root);
            throw std::runtime_error("AI analysis failed without specific error message");
        }
    }
    
    // 提取text字段
    cJSON* text_item = cJSON_GetObjectItem(root, "text");
    if (text_item != nullptr && cJSON_IsString(text_item)) {
        explanation_text = text_item->valuestring;
        ESP_LOGI(TAG, "Successfully extracted explanation text: %s", explanation_text.c_str());
    } else {
        cJSON* result_item = cJSON_GetObjectItem(root, "result");
        if (result_item != nullptr && cJSON_IsString(result_item)) {
            explanation_text = result_item->valuestring;
            ESP_LOGI(TAG, "Extracted explanation text from 'result' field: %s", explanation_text.c_str());
        } else {
            cJSON_Delete(root);
            throw std::runtime_error("Missing 'text' or 'result' field in successful response");
        }
    }
    
    cJSON_Delete(root);
    return explanation_text;
}


bool Esp32Camera::GetCapturedJpeg(uint8_t*& data, size_t& len) {
    // 检查 frame buffer 是否有效
    if (!fb_) {
        ESP_LOGE(TAG, "No frame buffer available");
        return false;
    }

    // 创建局部的 JPEG 队列
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        return false;
    }

    // 编码任务参数结构体
    struct EncodeParam {
        SemaphoreHandle_t done_sem;
        camera_fb_t* src;
        QueueHandle_t dest;
    };

    EncodeParam* param = new EncodeParam;
    param->done_sem = xSemaphoreCreateBinary();
    param->src = fb_;
    param->dest = jpeg_queue;

    // 创建编码 Task 替代 std::thread
    xTaskCreate(
        [](void* arg) {
            auto param = static_cast<EncodeParam*>(arg);
            frame2jpg_cb(param->src, 63,
                [](void* arg, size_t index, const void* data, size_t len) -> unsigned int {
                    auto jpeg_queue = (QueueHandle_t)arg;
                    JpegChunk chunk = {
                        .data = (uint8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM),
                        .len = len
                    };
                    if (chunk.data) {
                        memcpy(chunk.data, data, len);
                        xQueueSend(jpeg_queue, &chunk, pdMS_TO_TICKS(200));
                    }
                    return len;
                },
                param->dest);

            // 发送结束标记
            JpegChunk end_chunk = {.data = nullptr, .len = 0};
            xQueueSend(param->dest, &end_chunk, pdMS_TO_TICKS(200));

            xSemaphoreGive(param->done_sem);
            vTaskDelete(NULL);
        },
        "jpeg_encode", 4 * 1024, param, 2, NULL);

    // 收集所有的 JPEG 数据块
    std::vector<JpegChunk> chunks;
    size_t total_size = 0;
    JpegChunk chunk;

    while (true) {
        if (xQueueReceive(jpeg_queue, &chunk, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk, timeout");
            break;
        }
        if (chunk.data == nullptr) {
            break;  // 结束标记
        }
        chunks.push_back(chunk);
        total_size += chunk.len;
    }

    // 等待 Task 完成
    int count = 0;
    const int max_wait_count = 20; 
    while (xSemaphoreTake(param->done_sem, pdMS_TO_TICKS(500)) == pdFALSE) {
        count++;
        ESP_LOGW(TAG, "Waiting for encode task, count: %d", count);
        if (count >= max_wait_count) {
            ESP_LOGE(TAG, "Encode task timeout after 10 seconds, giving up");
            // 清理资源并返回失败
            vSemaphoreDelete(param->done_sem);
            delete param;
            vQueueDelete(jpeg_queue);
            // 清理已收集的chunks
            for (auto& c : chunks) {
                if (c.data) {
                    heap_caps_free(c.data);
                }
            }
            return false;
        }
    }

    // 清理资源
    vSemaphoreDelete(param->done_sem);
    delete param;
    vQueueDelete(jpeg_queue);

    if (chunks.empty() || total_size == 0) {
        ESP_LOGE(TAG, "No JPEG data received or encoding failed");
        return false;
    }

    // 释放旧数据
    if (data != nullptr) {
        heap_caps_free(data);
        data = nullptr;
    }
    len = 0;

    // 分配连续的内存空间存储完整的 JPEG 数据
    data = (uint8_t*)heap_caps_aligned_alloc(16, total_size, MALLOC_CAP_SPIRAM);
    if (!data) {
        ESP_LOGE(TAG, "Failed to allocate memory for complete JPEG: %u bytes", total_size);
        for (auto& c : chunks) {
            if (c.data) {
                heap_caps_free(c.data);
            }
        }
        return false;
    }

    // 将所有 chunk 拷贝到连续内存中
    size_t offset = 0;
    for (auto& c : chunks) {
        memcpy(data + offset, c.data, c.len);
        offset += c.len;
        heap_caps_free(c.data);
    }

    len = total_size;
    ESP_LOGI(TAG, "JPEG encoding completed, total size: %u bytes, data: %p", len, data);
    return true;
}