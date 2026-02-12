#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#if CONFIG_CONNECTION_TYPE_NERTC
#include "nertc_protocol.h"
#endif
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#ifdef HAVE_LVGL
#include "display/lcd_display.h"
#endif

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#include <esp32_camera.h>
#include <mbedtls/base64.h>

#define TAG "Application"

#define NERTC_AI_START_TOPIC "(wakeup_command#系统指令：用户刚刚喊了你的名字把你唤醒了) 请用一句非常简短、元气满满、开心激动的语气回应用户。 要求：表现出因为被呼唤而感到高兴；可以适当加入可爱的语气词；字数控制在 15 个字以内；不要输出任何解释性文字，直接输出你要说的那句话。"

static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#elif CONFIG_USE_NERTC_SERVER_AEC
    aec_mode_ = kAecOnNertc;
#else
    aec_mode_ = kAecOff;
#endif

    Settings settings("aec", false);
    int32_t saved_mode = settings.GetInt("mode", -1);
    if (saved_mode >= 0 && saved_mode <= kAecOnServerSide) {
        aec_mode_ = static_cast<AecMode>(saved_mode);
    }

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    // Create timer for handling "llm image sent" timeout
    esp_timer_create_args_t llm_image_sent_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->Schedule([app]() {
                if (app->device_state_ == kDeviceStateListening) {
                    ESP_LOGI(TAG, "No response after 'llm image sent', switching to idle state");
                    app->SetDeviceState(kDeviceStateIdle);
                    app->audio_service_.PlaySound(Lang::Sounds::OGG_FAILED);
                }
            });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "llm_image_sent_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&llm_image_sent_timer_args, &llm_image_sent_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (llm_image_sent_timer_handle_ != nullptr) {
        esp_timer_stop(llm_image_sent_timer_handle_);
        esp_timer_delete(llm_image_sent_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

   
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota.CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota.GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion("error");
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
#ifdef CONFIG_USE_MUSIC_PLAYER
    MusicPlayer::GetInstance().InterruptPlay();
#endif
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    // 连接失败，设置表情为error
                    auto display = Board::GetInstance().GetDisplay();
                    if (display != nullptr) {
                        display->SetEmotion("error");
                    }
                    return;
                }
                ai_sleep_ = false;
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    // 连接失败，设置表情为error
                    auto display = Board::GetInstance().GetDisplay();
                    if (display != nullptr) {
                        display->SetEmotion("error");
                    }
                    return;
                }
                ai_sleep_ = false;
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    auto& assets = Assets::GetInstance();
    if (assets.partition_valid() && assets.checksum_valid()) {
        assets.Apply();
    }
    
    SetDeviceState(kDeviceStateStarting);

    #ifdef HAVE_LVGL
    auto lcd_display = dynamic_cast<LcdDisplay*>(display);
    if (lcd_display != nullptr && !lcd_display->GetTextMode()) {
        display->SetEmotion("error");
    }
    #endif

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);
    int interrupteMode = ota.GetOtaAgentInterruptMode();
    agent_interrupt_mode_ = interrupteMode;
    aec_mode_ = interrupteMode == 0 ? kAecOff : kAecOnDeviceSide;
    // Save AEC mode to Settings so GetAppliedOutputVolume() can read it correctly
    Settings aec_settings("aec", true);
    aec_settings.SetInt("mode", static_cast<int32_t>(aec_mode_));
#ifdef CONFIG_USE_MUSIC_PLAYER
    if (ota.GetSupportAirMusicPlayer() && (Board::GetInstance().GetBoardType() != "ml307" || ota.GetSupportAirMusicIn4G())){
        MusicPlayer::GetInstance().Initialize(codec, &audio_service_);
    } 
#endif
    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

#if CONFIG_CONNECTION_TYPE_NERTC
    protocol_ = std::make_unique<NeRtcProtocol>();
#else
    ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
    protocol_ = std::make_unique<MqttProtocol>();
#endif

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        // 设置表情为error
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->SetEmotion("error");
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking|| current_pedding_speaking_.load()) { 
            std::unique_ptr<AudioStreamPacket> reference_packet = nullptr;
#if CONFIG_CONNECTION_TYPE_NERTC && CONFIG_USE_NERTC_SERVER_AEC
            reference_packet = std::make_unique<AudioStreamPacket>();
            reference_packet->payload = packet->payload;
            reference_packet->timestamp = packet->timestamp;
            reference_packet->sample_rate = protocol_->server_sample_rate();
#endif
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
            if (reference_packet) {
                protocol_->SendAecReferenceAudio(std::move(reference_packet));
            }
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                // Check if there's an active timer waiting for response after "llm image sent"
                bool has_active_timer = false;
                if (llm_image_sent_timer_handle_ != nullptr) {
                    int64_t timer_time = esp_timer_get_time();
                    if (esp_timer_is_active(llm_image_sent_timer_handle_)) {
                        has_active_timer = true;
                        esp_timer_stop(llm_image_sent_timer_handle_);
                        ESP_LOGI(TAG, "Received TTS start after 'llm image sent', switching to speaking state");
                    }
                }

                current_pedding_speaking_.store(true);
                if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening){
                    audio_service_.ResetDecoder(); //这里涉及到任务投递执行，所有resetdecoder需要提前做。不然前面一两帧音频都丢失
                }
                Schedule([this, has_active_timer]() {
                    aborted_ = false;
                    // If timer was active (waiting for response after "llm image sent") and we're in listening state, switch to speaking
                    if (has_active_timer && device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    } else if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            // SetDeviceState(kDeviceStateListening);
                            bool wait = false;
                            if(!aborted_){
                                wait = true;
                                audio_service_.WaitForPlayCompletion(200);
                            }
                            if(device_state_ == kDeviceStateSpeaking && (!wait || (wait && !aborted_))){
                                SetDeviceState(kDeviceStateListening);
                            }

                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, message = std::string(text->valuestring)]() {
                        auto display = Board::GetInstance().GetDisplay();
                        if (display != nullptr) {
                            display->SetChatMessage("assistant", message.c_str());
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::string message = text->valuestring;
                ESP_LOGI(TAG, ">> %s", message.c_str());
                
                // Check if this is "llm image sent" message and device is in listening state
                if (message == "llm image sent" && device_state_ == kDeviceStateListening) {
                    ESP_LOGI(TAG, "Received 'llm image sent' in listening state, starting 15s timer");
                    if (llm_image_sent_timer_handle_ != nullptr) {
                        // Stop any existing timer first
                        esp_timer_stop(llm_image_sent_timer_handle_);
                        // Start 15 second timer
                        esp_timer_start_once(llm_image_sent_timer_handle_, 15 * 1000 * 1000);
                    }
                }
                
                Schedule([this, message]() {
                    auto display = Board::GetInstance().GetDisplay();
                    if (display != nullptr) {
                        display->SetChatMessage("user", message.c_str());
                    }
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            // Check if there's an active timer waiting for response after "llm image sent"
            bool has_active_timer = false;
            if (llm_image_sent_timer_handle_ != nullptr) {
                if (esp_timer_is_active(llm_image_sent_timer_handle_)) {
                    has_active_timer = true;
                    esp_timer_stop(llm_image_sent_timer_handle_);
                    ESP_LOGI(TAG, "Received LLM message after 'llm image sent', switching to speaking state");
                }
            }
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, emotion_str = std::string(emotion->valuestring), has_active_timer]() {
                    auto display = Board::GetInstance().GetDisplay();
                    if (display != nullptr) {
                        display->SetEmotion(emotion_str.c_str());
                    }
                    // If timer was active and we're still in listening state, switch to speaking
                    if (has_active_timer && device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else if (strcmp(command->valuestring, "sleep") == 0) {
                    Schedule([this]() {
                        ai_sleep_ = true;
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    auto display = Board::GetInstance().GetDisplay();
                    if (display != nullptr) {
                        display->SetChatMessage("system", payload_str.c_str());
                    }
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        ESP_LOGI(TAG, "Protocol started successfully aec_mode = %d", aec_mode_);
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            // 设置表情为error（Alert中也会设置，但这里确保error表情被设置）
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetEmotion("error");
            }
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }

            if (ai_sleep_ && (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening)) {
                Schedule([this]() {
                    ESP_LOGI(TAG, "AI sleep mode, close the audio channel");
                    if (protocol_) {
                        protocol_->CloseAudioChannel();
                    }
                    ai_sleep_ = false;
                });
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
#ifdef CONFIG_USE_MUSIC_PLAYER
        MusicPlayer::GetInstance().InterruptPlay();
#endif
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel(std::string(NERTC_AI_START_TOPIC))) {
                // 连接失败，设置表情为error
                auto display = Board::GetInstance().GetDisplay();
                if (display != nullptr) {
                    display->SetEmotion("error");
                }
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
            ai_sleep_ = false;
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    current_pedding_speaking_.store(false);
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateStarting: {
            display->SetStatus(Lang::Strings::INITIALIZING);
            display->SetEmotion("error");
            break;
        }
        case kDeviceStateWifiConfiguring: {
            display->SetStatus(Lang::Strings::WIFI_CONFIG_MODE);
            display->SetEmotion("error");
            break;
        }
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            mic_disabled_for_next_listening_ = false;
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
            case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
               
                if (!mic_disabled_for_next_listening_) {
                    audio_service_.EnableVoiceProcessing(true);
                } else {
                    audio_service_.EnableVoiceProcessing(false);
                    audio_service_.EnableMicInput(false);
                }
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            mic_disabled_for_next_listening_ = false;

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            // audio_service_.ResetDecoder();
            break;
        case kDeviceStateActivating: {
            display->SetEmotion("error");
            break;
        }
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";
    
    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);
    
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveMode(false);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    board.StartBlufiOtaMode(upgrade_url, ota.GetFirmwareVersion(), ota.GetMd5());
    return true;

    // bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [display](int progress, size_t speed) {
    //     std::thread([display, progress, speed]() {
    //         char buffer[32];
    //         snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
    //         display->SetChatMessage("system", buffer);
    //     }).detach();
    // });

    // if (!upgrade_success) {
    //     // Upgrade failed, restart audio service and continue running
    //     ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
    //     audio_service_.Start(); // Restart audio service
    //     board.SetPowerSaveMode(true); // Restore power save mode
    //     Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
    //     vTaskDelay(pdMS_TO_TICKS(3000));
    //     return false;
    // } else {
    //     // Upgrade success, reboot immediately
    //     ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
    //     display->SetChatMessage("system", "Upgrade successful, rebooting...");
    //     vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
    //     Reboot();
    //     return true;
    // }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }


    audio_service_.EncodeWakeWord();

    if (!protocol_->IsAudioChannelOpened()) {
        SetDeviceState(kDeviceStateConnecting);
        if (!protocol_->OpenAudioChannel(wake_word)) {
            // 连接失败，设置表情为error
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetEmotion("error");
            }
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
    SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    // Play the pop up sound to indicate the wake word is detected
    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Settings settings("aec", true);
    settings.SetInt("mode", static_cast<int32_t>(aec_mode_));
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            audio_service_.PlaySound(Lang::Sounds::OGG_AEC_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            audio_service_.PlaySound(Lang::Sounds::OGG_AEC_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            audio_service_.PlaySound(Lang::Sounds::OGG_AEC_ON);
            break;
#if CONFIG_USE_NERTC_SERVER_AEC
        case kAecOnNertc:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
#endif
        default:
            break;
        }


        // Re-apply output volume so AEC-mode-dependent scaling takes effect immediately.
        if (auto codec = board.GetAudioCodec(); codec) {
            codec->SetOutputVolume(codec->output_volume());
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PhotoExplain(const std::string& request, const std::string& pre_answer, bool network_image) {
    if (network_image) { // 测试代码
        Schedule([this, request]() {
            std::string img_url = "https://pics5.baidu.com/feed/5ab5c9ea15ce36d3f5f01f21f488f897e850b1b3.jpeg";
            protocol_->SendLlmImage(img_url.c_str(), img_url.size(), 0, request, 1);
        });
    } else {
        Schedule([this, request, pre_answer]() {
            if (!pre_answer.empty()) {
                protocol_->SendTTSText(pre_answer, 2, false);
            }

            Camera* camera = Board::GetInstance().GetCamera();
            if (camera) {
                camera->Capture();
                JpegChunk jpeg = {nullptr, 0};
                if (camera->GetCapturedJpeg(jpeg.data, jpeg.len)) {
                    ESP_LOGI(TAG, "Captured JPEG size: %d", jpeg.len);

                    // 检查输入参数有效性
                    if (!jpeg.data || jpeg.len == 0) {
                        ESP_LOGE(TAG, "Invalid JPEG data: data=%p, len=%d", jpeg.data, jpeg.len);
                        if (jpeg.data) {
                            heap_caps_free(jpeg.data);
                        }
                        return;
                    }

                    const char* image_format = "data:image/jpeg;base64,";

                    // 计算需要的Base64缓冲区大小 (输入长度 * 4/3 + 填充)
                    size_t olen = ((jpeg.len + 2) / 3) * 4 + strlen(image_format) + 1;  // +1 for null terminator

                    // 分配内存并检查是否成功
                    unsigned char *base64_image = (uint8_t*)heap_caps_aligned_alloc(16, olen, MALLOC_CAP_SPIRAM);
                    if (!base64_image) {
                        ESP_LOGE(TAG, "Failed to allocate memory for base64 encoding: %u bytes", olen);
                        if (jpeg.data) {
                            heap_caps_free(jpeg.data);
                        }
                        return;
                    }

                    // 清零缓冲区
                    memset(base64_image, 0, olen);

                    // 添加Base64前缀
                    strcpy((char *)base64_image, image_format);

                    // 进行Base64编码
                    size_t encoded_size = 0;
                    int ret = mbedtls_base64_encode(base64_image + strlen(image_format), olen - strlen(image_format), &encoded_size, jpeg.data, jpeg.len);
                    if (ret == 0) {
                        protocol_->SendLlmImage((const char *)base64_image, strlen(image_format) + encoded_size, 0, request, 0);
                        ESP_LOGI(TAG, "Successfully encoded JPEG to base64, size: %u", olen);
                    } else {
                        ESP_LOGE(TAG, "mbedtls_base64_encode failed: %d", ret);
                    }

                    // 释放内存
                    heap_caps_free(base64_image);
                    if (jpeg.data) {
                        heap_caps_free(jpeg.data);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to get captured JPEG");
                }
            }
        });
    }
}

void Application::SetAISleep() {
    if (!protocol_) {
        ESP_LOGE(TAG, "SetAISleep: Protocol not initialized");
        return;
    }

    protocol_->SetAISleep();
}

void Application::Close() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    SetDeviceState(kDeviceStateIdle);

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    Schedule([this]() {
        protocol_->CloseAudioChannel();
    });
}
