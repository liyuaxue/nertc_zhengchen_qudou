#include "device_state.h"
#include "alarm.h"
#include "dual_network_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "display/clock_desktop_ui.h"
#include "display/settings_page_ui.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"
#include "power_manager.h"
#include "power_save_timer.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <functional>
#include <string>

#define TAG "Zhengchen_Qudou"

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        // 配置寄存器（0x03）：0=输出，1=输入
        // IO0: LCD控制（输出，bit 0 = 0）
        // IO1: 音频功放（输出，bit 1 = 0）
        // IO2: 相机电源（输出，bit 2 = 0）
        // IO3: 音量加按键（输入，bit 3 = 1）
        // IO4: 音量减按键（输入，bit 4 = 1）
        // IO5: 关机控制（输出，bit 5 = 0）
        // IO6: 充电检测（输入，bit 6 = 1）
        // IO7: 未使用（输出，bit 7 = 0）
        // 0x58 = 0b01011000，表示IO3、IO4和IO6为输入，其他为输出
        WriteReg(0x03, 0x58);
        WriteReg(0x01, 0x03);
        SetOutputState(5, 0);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }

    bool GetInputState(uint8_t bit) {
        uint8_t data = ReadReg(0x00);
        return (data & (1 << bit)) != 0;
    }
};

class Pca9557Button {
private:
    Pca9557* pca9557_;
    uint8_t pin_bit_;
    bool active_high_;
    bool last_state_;
    bool current_state_;
    bool is_pressed_;
    esp_timer_handle_t timer_handle_;
    uint32_t debounce_count_;
    static constexpr uint32_t DEBOUNCE_THRESHOLD = 1; 

    std::function<void()> on_click_;

    static void TimerCallback(void* arg) {
        Pca9557Button* button = static_cast<Pca9557Button*>(arg);
        button->CheckState();
    }

    void CheckState() {
        bool pin_state = pca9557_->GetInputState(pin_bit_);
        bool pressed = active_high_ ? pin_state : !pin_state;

        if (pressed != last_state_) {
            debounce_count_++;
            if (debounce_count_ >= DEBOUNCE_THRESHOLD) {
                current_state_ = pressed;
                last_state_ = pressed;
                debounce_count_ = 0;

                if (pressed) {
                    if (!is_pressed_) {
                        is_pressed_ = true;
                    }
                } else {
                    if (is_pressed_) {
                        if (on_click_) {
                            on_click_();
                        }
                        is_pressed_ = false;
                    }
                }
            }
        } else {
            if (pressed == current_state_) {
                debounce_count_ = 0;
            }
        }
    }

public:
    Pca9557Button(Pca9557* pca9557, uint8_t pin_bit, bool active_high = false)
        : pca9557_(pca9557), pin_bit_(pin_bit), active_high_(active_high),
          last_state_(false), current_state_(false),
          is_pressed_(false), debounce_count_(0) {
        
       
      
        bool state1 = active_high_ ? pca9557_->GetInputState(pin_bit_) : !pca9557_->GetInputState(pin_bit_);
        vTaskDelay(pdMS_TO_TICKS(10));
        bool state2 = active_high_ ? pca9557_->GetInputState(pin_bit_) : !pca9557_->GetInputState(pin_bit_);
        vTaskDelay(pdMS_TO_TICKS(10));
        bool state3 = active_high_ ? pca9557_->GetInputState(pin_bit_) : !pca9557_->GetInputState(pin_bit_);
        
        last_state_ = (state1 && state2) || (state2 && state3) || (state1 && state3);
        current_state_ = last_state_;

        esp_timer_create_args_t timer_args = {
            .callback = TimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pca9557_button",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 20000)); 
    }

    ~Pca9557Button() {
        if (timer_handle_ != nullptr) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
    }

    void OnClick(std::function<void()> callback) {
        on_click_ = callback;
    }
};

class CustomAudioCodec : public BoxAudioCodec {
private:
    Pca9557* pca9557_;

public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557) 
        : BoxAudioCodec(i2c_bus, 
                       AUDIO_INPUT_SAMPLE_RATE, 
                       AUDIO_OUTPUT_SAMPLE_RATE,
                       AUDIO_I2S_GPIO_MCLK, 
                       AUDIO_I2S_GPIO_BCLK, 
                       AUDIO_I2S_GPIO_WS, 
                       AUDIO_I2S_GPIO_DOUT, 
                       AUDIO_I2S_GPIO_DIN,
                       GPIO_NUM_NC, 
                       AUDIO_CODEC_ES8311_ADDR, 
                       AUDIO_CODEC_ES7210_ADDR, 
                       AUDIO_INPUT_REFERENCE),
          pca9557_(pca9557) {
    }

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            pca9557_->SetOutputState(1, 1);
        } else {
            pca9557_->SetOutputState(1, 0);
        }
    }
};

class Zhengchen_Qudou: public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    Esp32Camera* camera_;
    Button boot_button_;
    Button cam_button_;
    Button vib_button_;
    Pca9557Button* volume_up_button_;
    Pca9557Button* volume_down_button_;
    Display* display_;
    Pca9557* pca9557_;
    int64_t boot_time_us_; 
    bool is_settings_page_visible_ = false;
    AlarmManager* alarm_manager_ = nullptr;
    
    void InitializePowerManager() {
        // 使用PCA9557 IO6读取充电状态（低电平=充电，高电平=未充电）
        power_manager_ = new PowerManager([this]() -> bool {
            bool io6_state = pca9557_->GetInputState(6);
            bool is_charging = !io6_state;  // IO6低电平表示充电
            return is_charging;
        });
        
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
                // 充电时重置低电量弹窗标志位，方便下次再次提醒
                auto* lv_display = dynamic_cast<LvglDisplay*>(GetDisplay());
                if (lv_display) {
                    lv_display->ResetLowBatteryPopup();
                }
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });

        // 电量为 0% 且持续 5 秒时，触发关机：通过 PCA9557 IO5 拉高 50ms 再拉低
        power_manager_->OnBatteryShutdownRequest([this]() {
            ESP_LOGW(TAG, "Battery 0%% for 5s, powering off via PCA9557 IO5");
            pca9557_->SetOutputState(5, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            pca9557_->SetOutputState(5, 0);
        });
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 20,60);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            
            // 显示时钟界面
            auto* lcd_display = dynamic_cast<LcdDisplay*>(GetDisplay());
            if (lcd_display != nullptr) {
                auto* clock_ui = lcd_display->GetClockDesktopUI();
                if (clock_ui != nullptr) {
                    auto* theme = static_cast<LvglTheme*>(lcd_display->GetTheme());
                    if (theme != nullptr) {
                        clock_ui->SetTheme(theme);
                    }
                    clock_ui->Show();
                }
            }
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
            // 隐藏时钟界面
            auto* lcd_display = dynamic_cast<LcdDisplay*>(GetDisplay());
            if (lcd_display != nullptr) {
                auto* clock_ui = lcd_display->GetClockDesktopUI();
                if (clock_ui != nullptr) {
                    clock_ui->Hide();
                }
            }
        });
        power_save_timer_->OnShutdownRequest([this]() {
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->SetEnabled(true);
    }


    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
                .allow_pd = 0,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        pca9557_ = new Pca9557(i2c_bus_, 0x19);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_SPI_MISO_PIN;
        buscfg.sclk_io_num = DISPLAY_SPI_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // 如果摄像头预览正在运行，忽略 boot 按键单击
            auto* esp32_camera = dynamic_cast<Esp32Camera*>(camera_);
            if (esp32_camera != nullptr && esp32_camera->IsPreviewRunningFlag()) {
                return;
            }
            // 如果设置界面正在显示，忽略 boot 按键单击
            auto* lcd_display = dynamic_cast<LcdDisplay*>(GetDisplay());
            if (lcd_display != nullptr) {
                auto* settings_ui = lcd_display->GetSettingsPageUI();
                if (settings_ui != nullptr && settings_ui->IsVisible()) {
                    return;
                }
            }
            power_save_timer_->WakeUp();
            app.ToggleChatState();
        });

        

        cam_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            if (is_settings_page_visible_) {
                auto* lcd_display = dynamic_cast<LcdDisplay*>(GetDisplay());
                if (lcd_display != nullptr) {
                    auto* settings_ui = lcd_display->GetSettingsPageUI();
                    if (settings_ui != nullptr) {
                        if (settings_ui->OnCameraClick()) {
                            return;  // 已处理，不执行原始功能
                        }
                    }
                }
            }

            auto& app = Application::GetInstance();
            if(app.GetDeviceState()== kDeviceStateListening || app.GetDeviceState()== kDeviceStateSpeaking) {
                app.SetDeviceState(kDeviceStateIdle);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            
            if(app.GetDeviceState() == kDeviceStateIdle) {
                auto* esp32_camera = dynamic_cast<Esp32Camera*>(camera_);
                if (esp32_camera != nullptr) {
                    if (esp32_camera->IsPreviewRunning()) {
                        esp32_camera->StopPreview();
                        //拍照并上传图片到api接口
#if CONFIG_CONNECTION_TYPE_NERTC
                        app.SetMicDisabledForNextListening(true);
                        app.ToggleChatState();
                        std::string question = "请详细描述一下你看到的画面";
                        std::string query = "围绕这个主题《" + question + "》，分析并描述你看到了什么。";
                        std::string pre_answer = "让我看看";
                        app.PhotoExplain(query, pre_answer, false);
                        return;
#endif
                    } else {
                        // 如果预览未运行，则开始预览
                        esp32_camera->StartPreview();
                    }
                }
            }
        });

        cam_button_.OnDoubleClick([this]() {
            power_save_timer_->WakeUp();
            auto* esp32_camera = dynamic_cast<Esp32Camera*>(camera_);
                if (esp32_camera != nullptr) {
                    if (esp32_camera->IsPreviewRunning()) {
                        esp32_camera->StopPreview();
                    }
                }
        });

        cam_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if(app.GetDeviceState()== kDeviceStateListening || app.GetDeviceState()== kDeviceStateSpeaking) {
                app.SetDeviceState(kDeviceStateIdle);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            
            // 如果摄像头预览正在运行，直接切换前后置镜头
            auto* esp32_camera = dynamic_cast<Esp32Camera*>(camera_);
            if (esp32_camera != nullptr && esp32_camera->IsPreviewRunning()) {
                Settings settings("camera", true);
                bool camera_is_front = settings.GetBool("is_front", false);
                camera_is_front = !camera_is_front;
                settings.SetBool("is_front", camera_is_front);
                
                esp32_camera->SetHMirror(!camera_is_front);
                esp32_camera->SetVFlip(!camera_is_front);
                return;  // 直接返回，不进入菜单栏，无语音提示
            }
            
            // 否则，显示/隐藏设置页面
            auto* lcd_display = dynamic_cast<LcdDisplay*>(GetDisplay());
            if (lcd_display != nullptr) {
                auto* settings_ui = lcd_display->GetSettingsPageUI();
                if (settings_ui != nullptr) {
                    // Always trust UI visibility to avoid state desync (e.g. "Exit settings" menu item).
                    if (settings_ui->IsVisible()) {
                        settings_ui->Hide();
                    } else {
                        settings_ui->Show();
                    }
                    is_settings_page_visible_ = settings_ui->IsVisible();
                }
            }
        });

       
        vib_button_.OnClick([]() {
            ESP_LOGI(TAG, "Vibration button clicked");
        });
    }

    void InitializeVolumeButtons() {
        volume_up_button_ = new Pca9557Button(pca9557_, 3, false); 
        volume_down_button_ = new Pca9557Button(pca9557_, 4, false);

        volume_up_button_->OnClick([this]() {
            power_save_timer_->WakeUp();
            auto* lcd_display = dynamic_cast<LcdDisplay*>(GetDisplay());
            if (lcd_display != nullptr) {
                auto* settings_ui = lcd_display->GetSettingsPageUI();
                if (settings_ui != nullptr && settings_ui->IsVisible()) {
                    is_settings_page_visible_ = true;
                    if (settings_ui->OnVolumeUp()) {
                        return;
                    }
                } else {
                    is_settings_page_visible_ = false;
                }
            }
            // 原始功能
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->UpdateVolume(volume);
        });

       
        volume_down_button_->OnClick([this]() {
            power_save_timer_->WakeUp();
            auto* lcd_display = dynamic_cast<LcdDisplay*>(GetDisplay());
            if (lcd_display != nullptr) {
                auto* settings_ui = lcd_display->GetSettingsPageUI();
                if (settings_ui != nullptr && settings_ui->IsVisible()) {
                    is_settings_page_visible_ = true;
                    if (settings_ui->OnVolumeDown()) {
                        return;
                    }
                } else {
                    is_settings_page_visible_ = false;
                }
            }
            // 原始功能
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->UpdateVolume(volume);
        });

    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RESET_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        pca9557_->SetOutputState(0, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }


    void InitializeAlarmManager() {
        alarm_manager_ = new AlarmManager();
        alarm_manager_->SetAlarmCallback([this](const std::string& name, const std::string& format_time) {
            ESP_LOGI(TAG, "Alarm triggered: %s at %s", name.c_str(), format_time.c_str());
            auto& app = Application::GetInstance();
            app.Alert("闹钟", name.c_str(), "bell",Lang::Sounds::OGG_BELL);
        });
    }

    void InitializeCamera() {
        // Open camera power
        pca9557_->SetOutputState(2, 0);

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;  
        config.ledc_timer = LEDC_TIMER_2; 
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;   
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;   
        config.jpeg_quality = 9;             
        config.fb_count = 2;                 // 增加帧缓冲数量，提供更平滑的预览
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;  // 按顺序获取帧，避免跳跃

        camera_ = new Esp32Camera(config);
        
        vTaskDelay(pdMS_TO_TICKS(200));
        Settings settings("camera", false);
        bool camera_is_front = settings.GetBool("is_front", false);
        if (camera_ != nullptr) {
            auto* esp32_camera = dynamic_cast<Esp32Camera*>(camera_);
            if (esp32_camera != nullptr) {
                esp32_camera->SetHMirror(!camera_is_front);
                esp32_camera->SetVFlip(!camera_is_front);
            }
        }
    }

public:
    Zhengchen_Qudou() : 
        DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN),
        boot_button_(BOOT_BUTTON_GPIO),
        cam_button_(CAM_BUTTON_GPIO),
        vib_button_(VIB_BUTTON_GPIO),
        boot_time_us_(esp_timer_get_time()) {
        InitializeI2c();
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeVolumeButtons();
        InitializeCamera();
        InitializeAlarmManager();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_, 
            pca9557_);
        return &audio_codec;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging)  override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }
    
    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual AlarmManager* GetAlarmManager() override {
        return alarm_manager_;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        DualNetworkBoard::SetPowerSaveMode(enabled);
    }
};

DECLARE_BOARD(Zhengchen_Qudou);

