/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

 #include "mcp_server.h"
 #include <esp_log.h>
 #include <esp_app_desc.h>
 #include <algorithm>
 #include <cstring>
 #include <esp_pthread.h>
 
#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "lcd_display.h"
#include "boards/zhengchen-qudou/alarm.h"
 
 #define TAG "MCP"
 
 McpServer::McpServer() {
 }
 
 McpServer::~McpServer() {
     for (auto tool : tools_) {
         delete tool;
     }
     tools_.clear();
 }
 
 void McpServer::AddCommonTools() {
     // *Important* To speed up the response time, we add the common tools to the beginning of
     // the tools list to utilize the prompt cache.
     // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。
 
     // Backup the original tools list and restore it after adding the common tools.
     auto original_tools = std::move(tools_);
     auto& board = Board::GetInstance();
 
     // Do not add custom tools here.
     // Custom tools must be added in the board's InitializeTools function.
 
     AddTool("self.get_device_status",
        "获取设备实时状态信息，包括：扬声器音量、屏幕状态、电池、网络等。\n"
        "适用场景：\n"
        "1. 回答用户关于设备当前状态的问题（例如“现在音量是多少？”）\n"
        "2. 进行设备控制前的状态确认（例如先读取音量，再决定调大/调小）",
         PropertyList(),
         [&board](const PropertyList& properties) -> ReturnValue {
             return board.GetDeviceStatusJson();
         });
 
    AddTool("self.good_bye",
        "用户有明确离开意图的时候，比如说“再见”、“我要休息啦”、“拜拜啦”、“goodbye”、“byebye”等等，调用它。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.SetAISleep();
            return true;
        });

     AddTool("self.audio_speaker.set_volume", 
         "设置扬声器音量。\n"
         "如果当前音量未知，建议先调用 `self.get_device_status` 获取状态后再设置。",
         PropertyList({
             Property("volume", kPropertyTypeInteger, 0, 100)
         }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            int volume = properties["volume"].value<int>();
            codec->SetOutputVolume(volume);
            board.GetDisplay()->UpdateVolume(volume);
            return true;
        });
     
     auto backlight = board.GetBacklight();
     if (backlight) {
         AddTool("self.screen.set_brightness",
             "设置屏幕亮度（0-100）。",
             PropertyList({
                 Property("brightness", kPropertyTypeInteger, 0, 100)
             }),
             [backlight](const PropertyList& properties) -> ReturnValue {
                 uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                 backlight->SetBrightness(brightness, true);
                 return true;
             });
     }
 
#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "设置屏幕主题。\n"
            "可选值：`light`（浅色）或 `dark`（深色）。",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
        
        // Add text mode switching tool for LcdDisplay
        auto lcd_display = dynamic_cast<LcdDisplay*>(display);
        if (lcd_display) {
            AddTool("self.screen.set_text_mode",
                "设置屏幕显示模式。\n"
                "`true`：文字模式（显示聊天文本 + 静态表情图）。\n"
                "`false`：表情模式（全屏表情，不显示聊天文本）。",
                PropertyList({
                    Property("text_mode", kPropertyTypeBoolean)
                }),
                [lcd_display](const PropertyList& properties) -> ReturnValue {
                    bool text_mode = properties["text_mode"].value<bool>();
                    lcd_display->SetTextMode(text_mode);
                    return true;
                });
            
            AddTool("self.screen.get_text_mode",
                "获取当前屏幕显示模式。\n"
                "返回：`true` 表示文字模式，`false` 表示表情模式。",
                PropertyList(),
                [lcd_display](const PropertyList& properties) -> ReturnValue {
                    cJSON *json = cJSON_CreateObject();
                    cJSON_AddBoolToObject(json, "text_mode", lcd_display->GetTextMode());
                    return json;
                });
        }
    }
 
     auto camera = board.GetCamera();
     if (camera) {
#ifdef CONFIG_CONNECTION_TYPE_NERTC
        AddTool("self.photo_explain",
            "全能视觉与拍照工具。这是你的‘眼睛’。当用户涉及到任何视觉相关的请求时，必须调用此工具。\n"
            "功能范围：\n"
            "1. 拍照/看世界：响应如“拍一张照片”、“看看这是什么”、“我拍到了什么”、“帮我拍个照”等指令。\n"
            "2. 识别与分析：响应如“这是什么东西”、“识别一下”、“看看这个场景”、“描述画面”、“用一首诗描述当前的场景”等指令。\n"
            "3. 功能性视觉：响应如“翻译一下这个”、“这道题怎么解”、“读一下上面的文字”等指令。注意：调用此工具意味着你会获取当前的视觉画面（自动拍照或读取画面）并根据question参数进行分析。不需要区分是单纯拍照还是解释，统一使用此工具。\n"
            "注意：调用此工具意味着你会获取当前的视觉画面（自动拍照或读取画面）并根据question参数进行分析。"
            "不需要区分是单纯拍照还是解释，统一使用此工具。\n"
            "参数：pre_answer，生成3-5字的简短口语回应，必须体现‘正在观看’或‘准备观察’的视觉动作，例如‘让我看看’、‘我瞧瞧看’、‘正在看喔’、‘Let me see’。严格禁止使用‘好的’、‘收到’、‘没问题’等无视觉语义的通用确认词。\n"
            "参数：question，用户的原始问题，不要做任何总结和修改。\n",
            PropertyList({
                Property("pre_answer", kPropertyTypeString),
                Property("question", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                try {
                    auto question = properties["question"].value<std::string>();
                    auto pre_answer = properties["pre_answer"].value<std::string>();

                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() == kDeviceStateIdle) {
                        ESP_LOGE(TAG, "Unsupport explain for ai stop");
                        return "{\"success\":false,\"error\":" "\"当前状态不支持识别操作\"}";
                    }

                    std::string query = "围绕这个主题《" + question + "》，分析并描述你看到了什么。";
                    app.PhotoExplain(query, pre_answer, false);
                    return "{\"success\":true,\"message\":\"识别成功\"}";
                } catch (const std::exception &e) {
                    ESP_LOGE(TAG, "Error interpreting recent photo: %s", e.what());
                    return "{\"success\":false,\"error\":\"" + std::string(e.what()) + "\"}";
                }
            });
#else
        AddTool("self.camera.take_photo",
            "拍照并根据用户问题进行解释/分析。\n"
            "适用场景：用户让你“看看/拍照/识别/描述画面”等。\n"
            "参数：\n"
            "- question：用户希望你结合照片回答的问题。\n"
            "返回：\n"
            "- 一个包含照片信息/解释结果的 JSON 对象。",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
#endif
     }
 #endif

     // Add alarm tool if alarm manager is available
     auto alarm_manager = board.GetAlarmManager();
     if (alarm_manager) {
         AddTool("self.alarm.set_alarm",
             "设置闹钟。只能设置一个闹钟，如果已有闹钟，新设置的闹钟会覆盖旧的。\n"
             "参数说明：\n"
             "- name: 闹钟名称（例如：\"起床\"、\"提醒\"等）\n"
             "- seconds_from_now: 从现在开始多少秒后触发闹钟（必须大于0）\n"
             "示例：设置一个60秒后触发的闹钟，名称为\"起床\"",
             PropertyList({
                 Property("name", kPropertyTypeString),
                 Property("seconds_from_now", kPropertyTypeInteger, 1, 86400)  // 1秒到24小时
             }),
             [alarm_manager](const PropertyList& properties) -> ReturnValue {
                 auto name = properties["name"].value<std::string>();
                 int seconds = properties["seconds_from_now"].value<int>();
                 
                 AlarmError error = alarm_manager->SetAlarm("alarm", name, seconds, true);
                 
                 if (error == ALARM_ERROR_NONE) {
                     std::vector<AlarmInfo> alarms;
                     if (alarm_manager->GetAlarmList(alarms) && !alarms.empty()) {
                         return "{\"success\":true,\"message\":\"闹钟设置成功\",\"alarm\":{\"name\":\"" + 
                                alarms[0].name + "\",\"time\":\"" + alarms[0].format_time + "\"}}";
                     }
                     return "{\"success\":true,\"message\":\"闹钟设置成功\"}";
                 } else if (error == ALARM_ERROR_INVALID_ALARM_TIME) {
                     return "{\"success\":false,\"error\":\"无效的闹钟时间\"}";
                 } else {
                     return "{\"success\":false,\"error\":\"设置闹钟失败\"}";
                 }
             });
     }
 
     // Restore the original tools list to the end of the tools list
     tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
 }
 
 void McpServer::AddUserOnlyTools() {
     // System tools
    AddUserOnlyTool("self.get_system_info",
        "获取系统信息（软硬件版本、构建信息、运行状态等）。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "重启设备。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware",
        "从指定 URL 升级固件：下载并安装固件，完成后设备会自动重启。",
        PropertyList({
            Property("url", kPropertyTypeString, "固件二进制文件的下载地址（URL）")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                 auto ota = std::make_unique<Ota>();
                 
                 bool success = app.UpgradeFirmware(*ota, url);
                 if (!success) {
                     ESP_LOGE(TAG, "Firmware upgrade failed");
                 }
             });
             
             return true;
         });
 
     // Display control
 #ifdef HAVE_LVGL
     auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
     if (display) {
        AddUserOnlyTool("self.screen.get_info",
            "获取屏幕信息（宽度、高度、是否单色等）。",
             PropertyList(),
             [display](const PropertyList& properties) -> ReturnValue {
                 cJSON *json = cJSON_CreateObject();
                 cJSON_AddNumberToObject(json, "width", display->width());
                 cJSON_AddNumberToObject(json, "height", display->height());
                 if (dynamic_cast<OledDisplay*>(display)) {
                     cJSON_AddBoolToObject(json, "monochrome", true);
                 } else {
                     cJSON_AddBoolToObject(json, "monochrome", false);
                 }
                 return json;
             });
 
        AddUserOnlyTool("self.screen.snapshot",
            "截取当前屏幕并上传到指定 URL。",
             PropertyList({
                 Property("url", kPropertyTypeString),
                 Property("quality", kPropertyTypeInteger, 80, 1, 100)
             }),
             [display](const PropertyList& properties) -> ReturnValue {
                 auto url = properties["url"].value<std::string>();
                 auto quality = properties["quality"].value<int>();
 
                 uint8_t* jpeg_output_data = nullptr;
                 size_t jpeg_output_size = 0;
                 if (!display->SnapshotToJpeg(jpeg_output_data, jpeg_output_size, quality)) {
                    throw std::runtime_error("截屏失败");
                 }
 
                 ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_output_size, url.c_str());
                 
                 // 构造multipart/form-data请求体
                 std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                 
                 auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                 http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                 if (!http->Open("POST", url)) {
                     free(jpeg_output_data);
                     throw std::runtime_error("Failed to open URL: " + url);
                 }
                 {
                     // 文件字段头部
                     std::string file_header;
                     file_header += "--" + boundary + "\r\n";
                     file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                     file_header += "Content-Type: image/jpeg\r\n";
                     file_header += "\r\n";
                     http->Write(file_header.c_str(), file_header.size());
                 }
 
                 // JPEG数据
                 http->Write((const char*)jpeg_output_data, jpeg_output_size);
                 free(jpeg_output_data);
 
                 {
                     // multipart尾部
                     std::string multipart_footer;
                     multipart_footer += "\r\n--" + boundary + "--\r\n";
                     http->Write(multipart_footer.c_str(), multipart_footer.size());
                 }
                 http->Write("", 0);
 
                 if (http->GetStatusCode() != 200) {
                     throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                 }
                 std::string result = http->ReadAll();
                 http->Close();
                 ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                 return true;
             });
         
        AddUserOnlyTool("self.screen.preview_image",
            "在屏幕上预览一张图片（通过 URL 下载后显示）。",
             PropertyList({
                 Property("url", kPropertyTypeString)
             }),
             [display](const PropertyList& properties) -> ReturnValue {
                 auto url = properties["url"].value<std::string>();
                 auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
 
                 if (!http->Open("GET", url)) {
                     throw std::runtime_error("Failed to open URL: " + url);
                 }
                 int status_code = http->GetStatusCode();
                 if (status_code != 200) {
                     throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                 }
 
                 size_t content_length = http->GetBodyLength();
                 char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                 if (data == nullptr) {
                     throw std::runtime_error("Failed to allocate memory for image: " + url);
                 }
                 size_t total_read = 0;
                 while (total_read < content_length) {
                     int ret = http->Read(data + total_read, content_length - total_read);
                     if (ret < 0) {
                         heap_caps_free(data);
                         throw std::runtime_error("Failed to download image: " + url);
                     }
                     if (ret == 0) {
                         break;
                     }
                     total_read += ret;
                 }
                 http->Close();
 
                 auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                 display->SetPreviewImage(std::move(image));
                 return true;
             });
     }
 #endif
 
    // 资源包（assets）下载地址
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url",
            "设置资源包（assets）的下载地址（URL）。",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }
 }
 
 void McpServer::AddTool(McpTool* tool) {
     // Prevent adding duplicate tools
     if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
         ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
         return;
     }
 
     ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
     tools_.push_back(tool);
 }
 
 void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
     AddTool(new McpTool(name, description, properties, callback));
 }
 
 void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
     auto tool = new McpTool(name, description, properties, callback);
     tool->set_user_only(true);
     AddTool(tool);
 }
 
 void McpServer::ParseMessage(const std::string& message) {
     cJSON* json = cJSON_Parse(message.c_str());
     if (json == nullptr) {
         ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
         return;
     }
     ParseMessage(json);
     cJSON_Delete(json);
 }
 
 void McpServer::ParseCapabilities(const cJSON* capabilities) {
     auto vision = cJSON_GetObjectItem(capabilities, "vision");
     if (cJSON_IsObject(vision)) {
         auto url = cJSON_GetObjectItem(vision, "url");
         auto token = cJSON_GetObjectItem(vision, "token");
         if (cJSON_IsString(url)) {
             auto camera = Board::GetInstance().GetCamera();
             if (camera) {
                 std::string url_str = std::string(url->valuestring);
                 std::string token_str;
                 if (cJSON_IsString(token)) {
                     token_str = std::string(token->valuestring);
                 }
                 camera->SetExplainUrl(url_str, token_str);
             }
         }
     }
 }
 
 void McpServer::ParseMessage(const cJSON* json) {
     // Check JSONRPC version
     auto version = cJSON_GetObjectItem(json, "jsonrpc");
     if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
         ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
         return;
     }
     
     // Check method
     auto method = cJSON_GetObjectItem(json, "method");
     if (method == nullptr || !cJSON_IsString(method)) {
         ESP_LOGE(TAG, "Missing method");
         return;
     }
     
     auto method_str = std::string(method->valuestring);
     if (method_str.find("notifications") == 0) {
         return;
     }
     
     // Check params
     auto params = cJSON_GetObjectItem(json, "params");
     if (params != nullptr && !cJSON_IsObject(params)) {
         ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
         return;
     }
 
     auto id = cJSON_GetObjectItem(json, "id");
     if (id == nullptr || !cJSON_IsNumber(id)) {
         ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
         return;
     }
     auto id_int = id->valueint;
     
     if (method_str == "initialize") {
         if (cJSON_IsObject(params)) {
             auto capabilities = cJSON_GetObjectItem(params, "capabilities");
             if (cJSON_IsObject(capabilities)) {
                 ParseCapabilities(capabilities);
             }
         }
         auto app_desc = esp_app_get_description();
         std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
         message += app_desc->version;
         message += "\"}}";
         ReplyResult(id_int, message);
     } else if (method_str == "tools/list") {
         std::string cursor_str = "";
         bool list_user_only_tools = false;
         if (params != nullptr) {
             auto cursor = cJSON_GetObjectItem(params, "cursor");
             if (cJSON_IsString(cursor)) {
                 cursor_str = std::string(cursor->valuestring);
             }
             auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
             if (cJSON_IsBool(with_user_tools)) {
                 list_user_only_tools = with_user_tools->valueint == 1;
             }
         }
         GetToolsList(id_int, cursor_str, list_user_only_tools);
     } else if (method_str == "tools/call") {
         if (!cJSON_IsObject(params)) {
             ESP_LOGE(TAG, "tools/call: Missing params");
             ReplyError(id_int, "Missing params");
             return;
         }
         auto tool_name = cJSON_GetObjectItem(params, "name");
         if (!cJSON_IsString(tool_name)) {
             ESP_LOGE(TAG, "tools/call: Missing name");
             ReplyError(id_int, "Missing name");
             return;
         }
         auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
         if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
             ESP_LOGE(TAG, "tools/call: Invalid arguments");
             ReplyError(id_int, "Invalid arguments");
             return;
         }
         DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
     } else {
         ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
         ReplyError(id_int, "Method not implemented: " + method_str);
     }
 }
 
 void McpServer::ReplyResult(int id, const std::string& result) {
     std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
     payload += std::to_string(id) + ",\"result\":";
     payload += result;
     payload += "}";
     Application::GetInstance().SendMcpMessage(payload);
 }
 
 void McpServer::ReplyError(int id, const std::string& message) {
     std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
     payload += std::to_string(id);
     payload += ",\"error\":{\"message\":\"";
     payload += message;
     payload += "\"}}";
     Application::GetInstance().SendMcpMessage(payload);
 }
 
 void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
     const int max_payload_size = 8000;
     std::string json = "{\"tools\":[";
     
     bool found_cursor = cursor.empty();
     auto it = tools_.begin();
     std::string next_cursor = "";
     
     while (it != tools_.end()) {
         // 如果我们还没有找到起始位置，继续搜索
         if (!found_cursor) {
             if ((*it)->name() == cursor) {
                 found_cursor = true;
             } else {
                 ++it;
                 continue;
             }
         }
 
         if (!list_user_only_tools && (*it)->user_only()) {
             ++it;
             continue;
         }
         
         // 添加tool前检查大小
         std::string tool_json = (*it)->to_json() + ",";
         if (json.length() + tool_json.length() + 30 > max_payload_size) {
             // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
             next_cursor = (*it)->name();
             break;
         }
         
         json += tool_json;
         ++it;
     }
     
     if (json.back() == ',') {
         json.pop_back();
     }
     
     if (json.back() == '[' && !tools_.empty()) {
         // 如果没有添加任何tool，返回错误
         ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
         ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
         return;
     }
 
     if (next_cursor.empty()) {
         json += "]}";
     } else {
         json += "],\"nextCursor\":\"" + next_cursor + "\"}";
     }
     
     ReplyResult(id, json);
 }
 
 void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
     auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                  [&tool_name](const McpTool* tool) { 
                                      return tool->name() == tool_name; 
                                  });
     
     if (tool_iter == tools_.end()) {
         ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
         ReplyError(id, "Unknown tool: " + tool_name);
         return;
     }
 
     PropertyList arguments = (*tool_iter)->properties();
     try {
         for (auto& argument : arguments) {
             bool found = false;
             if (cJSON_IsObject(tool_arguments)) {
                 auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                 if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                     argument.set_value<bool>(value->valueint == 1);
                     found = true;
                 } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                     argument.set_value<int>(value->valueint);
                     found = true;
                 } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                     argument.set_value<std::string>(value->valuestring);
                     found = true;
                 }
             }
 
             if (!argument.has_default_value() && !found) {
                 ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                 ReplyError(id, "Missing valid argument: " + argument.name());
                 return;
             }
         }
     } catch (const std::exception& e) {
         ESP_LOGE(TAG, "tools/call: %s", e.what());
         ReplyError(id, e.what());
         return;
     }
 
     // Use main thread to call the tool
     auto& app = Application::GetInstance();
     app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
         try {
             ReplyResult(id, (*tool_iter)->Call(arguments));
         } catch (const std::exception& e) {
             ESP_LOGE(TAG, "tools/call: %s", e.what());
             ReplyError(id, e.what());
         }
     });
 }
 