#pragma once
#include <vector>
#include <functional>
#include <cstdlib>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/temperature_sensor.h>
#include <esp_sleep.h>
#include "config.h"

class PowerManager {
private:
    // 定时器句柄
    esp_timer_handle_t timer_handle_;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;
    std::function<void(float)> on_temperature_changed_;
    std::function<void()> on_battery_shutdown_request_;

    gpio_num_t charging_pin_ = CHARGER_DETECT_PIN;
    std::function<bool()> charging_status_read_func_;  // 用于读取充电状态的函数指针
    bool use_pca9557_ = false;  // 是否使用PCA9557
    std::vector<uint16_t> adc_values_;
    std::vector<uint16_t> ref_adc_values_;  // 参考电压ADC值队列
    uint32_t battery_level_ = 0;
    float current_battery_voltage_ = 0.0f;  // 当前电池电压
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    float current_temperature_ = 0.0f;
    int ticks_ = 0;
    int zero_battery_seconds_ = 0;   // 连续 0% 电量计数（秒）
    const int kBatteryAdcInterval = 60;
    const int kBatteryAdcDataCount = 3;
    const int kLowBatteryLevel = 5;  // 低电量阈值：5%
    const int kTemperatureReadInterval = 10; // 每 10 秒读取一次温度
    const float kRefVoltage = 1.24f;  // 参考电压固定值 1.24V
    const float kBatteryMinVoltage = 3.4f;  // 电池最低电压 (V)
    const float kBatteryMaxVoltage = 4.15f;  // 电池最高电压 (V)
    const float kVoltageDividerRatio = 2.0f;  // 分压比，电池电压减半后输入ADC

    adc_oneshot_unit_handle_t adc_handle_;
    temperature_sensor_handle_t temp_sensor_ = NULL;  

    void CheckBatteryStatus() {
        // Get charging status
        bool new_charging_status;
        if (use_pca9557_ && charging_status_read_func_) {
            // 使用PCA9557读取充电状态
            new_charging_status = charging_status_read_func_();
        } else {
            // 使用GPIO读取充电状态
            new_charging_status = gpio_get_level(charging_pin_) == 1;
        }
        
        if (new_charging_status != is_charging_) {
            is_charging_ = new_charging_status;
            if (use_pca9557_) {
                ESP_LOGI("PowerManager", "Charging status changed: %s (PCA9557 IO6 level: %d)", 
                        is_charging_ ? "Charging" : "Not charging", is_charging_ ? 1 : 0);
            } else {
                ESP_LOGI("PowerManager", "Charging status changed: %s (GPIO%d level: %d)", 
                        is_charging_ ? "Charging" : "Not charging", charging_pin_, gpio_get_level(charging_pin_));
            }
            if (on_charging_status_changed_) {
                on_charging_status_changed_(is_charging_);
            }
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据不足，则读取电池电量数据
        if (adc_values_.size() < kBatteryAdcDataCount) {
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据充足，则每 kBatteryAdcInterval 个 tick 读取一次电池电量数据
        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0) {
            ReadBatteryAdcData();
        }

        // 新增：周期性读取温度
        if (ticks_ % kTemperatureReadInterval == 0) {
            ReadTemperature();
        }

        // 电量为 0% 时计时，连续 5 秒则触发关机请求
        if (!is_charging_ && battery_level_ == 0) {
            zero_battery_seconds_++;
        } else {
            zero_battery_seconds_ = 0;
        }
        if (zero_battery_seconds_ >= 5) {
            ESP_LOGW("PowerManager", "Battery level is 0%% for %d seconds, requesting shutdown", zero_battery_seconds_);
            if (on_battery_shutdown_request_) {
                on_battery_shutdown_request_();
            }
        }
    }

    void ReadBatteryAdcData() {
        // 读取参考电压 ADC 值 (ADC1_4, 固定1.24V)
        int ref_adc_value;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, BATTERY_REF_ADC_CHANNEL, &ref_adc_value));
        
        // 读取电池电压 ADC 值 (原通道)
        int battery_adc_value;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, BATTERY_LEVEL_ADC_CHANNEL, &battery_adc_value));
        
        // 将参考电压ADC值添加到队列中（用于平滑）
        ref_adc_values_.push_back(ref_adc_value);
        if (ref_adc_values_.size() > kBatteryAdcDataCount) {
            ref_adc_values_.erase(ref_adc_values_.begin());
        }
        
        // 将电池ADC值添加到队列中
        adc_values_.push_back(battery_adc_value);
        if (adc_values_.size() > kBatteryAdcDataCount) {
            adc_values_.erase(adc_values_.begin());
        }
        
        // 计算平均值（如果数据足够）
        if (adc_values_.size() >= kBatteryAdcDataCount && ref_adc_values_.size() >= kBatteryAdcDataCount) {
            // 计算参考电压ADC平均值
            uint32_t average_ref_adc = 0;
            for (auto value : ref_adc_values_) {
                average_ref_adc += value;
            }
            average_ref_adc /= ref_adc_values_.size();
            
            // 计算电池电压ADC平均值
            uint32_t average_battery_adc = 0;
            for (auto value : adc_values_) {
                average_battery_adc += value;
            }
            average_battery_adc /= adc_values_.size();
            
            // 使用参考电压校准计算实际电池电压
            // 电池电压 = (电池ADC值 / 参考ADC值) * 参考电压 * 分压比
            if (average_ref_adc > 0) {
                float battery_voltage = (static_cast<float>(average_battery_adc) / static_cast<float>(average_ref_adc)) 
                                       * kRefVoltage * kVoltageDividerRatio;
                
                // 保存当前电池电压
                current_battery_voltage_ = battery_voltage;
                
                // 根据电池电压计算电量百分比 (0-100%)
                if (battery_voltage <= kBatteryMinVoltage) {
                    battery_level_ = 0;
                } else if (battery_voltage >= kBatteryMaxVoltage) {
                    battery_level_ = 100;
                } else {
                    // 线性映射：从 [kBatteryMinVoltage, kBatteryMaxVoltage] 映射到 [0, 100]
                    float ratio = (battery_voltage - kBatteryMinVoltage) / (kBatteryMaxVoltage - kBatteryMinVoltage);
                    battery_level_ = static_cast<uint32_t>(ratio * 100.0f);
                    if (battery_level_ > 100) {
                        battery_level_ = 100;
                    }
                }
                
                ESP_LOGI("PowerManager", "Ref ADC: %lu, Battery ADC: %lu, Battery Voltage: %.2fV, Level: %lu%%", 
                        average_ref_adc, average_battery_adc, battery_voltage, battery_level_);
            } else {
                ESP_LOGW("PowerManager", "Reference ADC value is zero, cannot calculate battery voltage");
            }
        } else {
            // 采样数量不足时，使用“瞬时值”先计算一个临时电量，避免开机初期电量显示误差很大
            if (ref_adc_value > 0) {
                float battery_voltage = (static_cast<float>(battery_adc_value) / static_cast<float>(ref_adc_value))
                                       * kRefVoltage * kVoltageDividerRatio;

                current_battery_voltage_ = battery_voltage;

                if (battery_voltage <= kBatteryMinVoltage) {
                    battery_level_ = 0;
                } else if (battery_voltage >= kBatteryMaxVoltage) {
                    battery_level_ = 100;
                } else {
                    float ratio = (battery_voltage - kBatteryMinVoltage) / (kBatteryMaxVoltage - kBatteryMinVoltage);
                    battery_level_ = static_cast<uint32_t>(ratio * 100.0f);
                    if (battery_level_ > 100) {
                        battery_level_ = 100;
                    }
                }

                ESP_LOGI("PowerManager", "[instant] Ref ADC: %d, Battery ADC: %d, Battery Voltage: %.2fV, Level: %lu%%",
                        ref_adc_value, battery_adc_value, battery_voltage, battery_level_);
            } else {
                ESP_LOGW("PowerManager", "Reference ADC value is zero, cannot calculate instant battery voltage");
            }
        }
        
        // 检查是否达到低电量阈值
        if (adc_values_.size() >= kBatteryAdcDataCount) {
            bool new_low_battery_status = battery_level_ <= kLowBatteryLevel;
            if (new_low_battery_status != is_low_battery_) {
                is_low_battery_ = new_low_battery_status;
                if (on_low_battery_status_changed_) {
                    on_low_battery_status_changed_(is_low_battery_);
                }
            }
        }
    }

    void ReadTemperature() {
        float temperature = 0.0f;
        ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor_, &temperature));
        
        if (abs(temperature - current_temperature_) >= 3.5f) {  // 温度变化超过3.5°C才触发回调
            current_temperature_ = temperature;
            if (on_temperature_changed_) {
                on_temperature_changed_(current_temperature_);
            }
            ESP_LOGI("PowerManager", "Temperature updated: %.1f°C", current_temperature_);
        }      
    }


public:
    // 使用GPIO引脚的构造函数
    PowerManager(gpio_num_t pin) : charging_pin_(pin), use_pca9557_(false) {
        
        // 初始化充电引脚
        if (pin != GPIO_NUM_NC) {
            gpio_config_t io_conf = {};
            io_conf.intr_type = GPIO_INTR_DISABLE;
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pin_bit_mask = (1ULL << charging_pin_);
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; 
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;     
            gpio_config(&io_conf);
        }
        
        InitializeCommon();
    }

    // 使用PCA9557的构造函数
    PowerManager(std::function<bool()> charging_status_read_func) 
        : charging_pin_(GPIO_NUM_NC), charging_status_read_func_(charging_status_read_func), use_pca9557_(true) {
        InitializeCommon();
    }

private:
    void InitializeCommon() {
        // 创建电池电量检查定时器
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                PowerManager* self = static_cast<PowerManager*>(arg);
                self->CheckBatteryStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));

        // 初始化 ADC
        adc_oneshot_unit_init_cfg_t init_config = {};
        init_config.unit_id = ADC_UNIT_1;
        init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));
        
        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        
        // 配置电池电压ADC通道
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, BATTERY_LEVEL_ADC_CHANNEL, &chan_config));
        
        // 配置参考电压ADC通道 (ADC1_4)
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, BATTERY_REF_ADC_CHANNEL, &chan_config));

        // 初始化温度传感器
        temperature_sensor_config_t temp_config = {
            .range_min = 10,
            .range_max = 80,
            .clk_src = TEMPERATURE_SENSOR_CLK_SRC_DEFAULT
        };
        ESP_ERROR_CHECK(temperature_sensor_install(&temp_config, &temp_sensor_));
        ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor_));
    }

public:

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_handle_) {
            adc_oneshot_del_unit(adc_handle_);
        }
        
        if (temp_sensor_) {
            temperature_sensor_disable(temp_sensor_);
            temperature_sensor_uninstall(temp_sensor_);
        }
  
    }

    bool IsCharging() {
        // 只要插着充电器就返回true，即使电量100%也要显示充电状态
        return is_charging_;
    }

    bool IsDischarging() {
        // 没有区分充电和放电，所以直接返回相反状态
        return !is_charging_;
    }

    // 获取电池电量
    uint8_t GetBatteryLevel() {
        // 返回电池电量
        return battery_level_;
    }

    // 获取电池电压
    float GetBatteryVoltage() const { return current_battery_voltage_; }  // 获取当前电池电压

    float GetTemperature() const { return current_temperature_; }  // 获取当前温度

    void OnTemperatureChanged(std::function<void(float)> callback) { 
        on_temperature_changed_ = callback; 
    }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = callback;
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }

    void OnBatteryShutdownRequest(std::function<void()> callback) {
        on_battery_shutdown_request_ = callback;
    }
};
