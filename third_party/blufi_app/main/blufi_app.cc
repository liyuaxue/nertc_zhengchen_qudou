#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <wifi_station.h>
#include <wifi_configuration_ap.h>

#include "settings.h"
#include "blufi_wifi.h"
#include "ssid_manager.h"
#include "ota.h"
#include "boards/board.h"

static const char *TAG = "BulfiApp";

struct TaskParam {
    std::string broad_name;
    std::string broad_info;
    std::string broad_type;
};

AppType_t GetAppType();

void RunBlufi();
void BlufiTask(const std::string& broad_name, const std::string& broad_info);
void RunWifiApMode(const std::string&& name);
static void OnBlufiCustomEventCallBack(BlufiCustomEvent_t event, void* data, int data_len);

void RunOta();
bool StartNetwork();

void RestartApp();

AppType_t g_app;
blufi_custom_event_callback_t g_custom_event_callback = nullptr;
static Ota g_ota;
static TaskHandle_t g_BlufiTask_handle = nullptr;
static TaskHandle_t g_wifi_ap_task_handle = nullptr;

extern const lv_image_dsc_t upgrade;  
extern const lv_image_dsc_t blufi;
extern const lv_image_dsc_t host;

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    /* 1. 初始化 NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    g_app = GetAppType();

    ESP_LOGI(TAG, "Starting BLUFI WiFi configuration... g_app:%d", (int)g_app);

    auto& board = Board::GetInstance();

    // 初始化按键，传递点击回调
    board.InitializeButtons([]() {
        ESP_LOGI(TAG, "Button clicked");
        
        if (g_ota.GetInstalling()) {
            ESP_LOGI(TAG, "OTA is installing, ignore button click");
            return;
        }

        if (g_app == APP_TYPE_OTA) {
            Settings settings("board", true);
            settings.SetInt("ota_fail", 1);
            ESP_LOGW(TAG, "Set ota_fail flag in NVS");
        }

        RestartApp();
    });

    // 初始化 SPI
    board.InitializeSpi();

    // 初始化 LCD 屏幕
    board.InitializeLcdDisplay();

    g_custom_event_callback = OnBlufiCustomEventCallBack;
    if (g_app != APP_TYPE_OTA) {
        ESP_LOGI(TAG, "App type is BLUFI, starting BLUFI mode...");
        RunBlufi();
    } else {
        ESP_LOGI(TAG, "App type is OTA, starting OTA mode...");
        RunOta();
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

AppType_t GetAppType() {
    Settings settings("board", false);
    int32_t app = settings.GetInt("app", 1);
    AppType_t app_type = static_cast<AppType_t>(app);
    ESP_LOGI(TAG, "Board app type from nvs: %d", (int)app_type);

    return app_type;
}

void RunBlufi() {
    Settings settings("board", true);

    int test_mode = settings.GetInt("test", 0);
    int blufi_mode = settings.GetInt("blufi", 1);
    ESP_LOGI(TAG, "Read board blufi mode from nvs: %d   test_mode: %d", blufi_mode, test_mode);

    auto display = Board::GetInstance().GetDisplay();
    display->SetTheme("dark");
    if (blufi_mode == 1) {
        display->SetBgImage(&blufi, true); 
        display->SetTitleText("使用微信扫描二维码配置设备");
    } else {
        display->SetBgImage(&host, true); 
        std::string activation_extra_msg = settings.GetString("extra_msg");
        ESP_LOGI(TAG, "Read board activation_extra_msg from nvs: %s", activation_extra_msg.c_str()); 
        if (activation_extra_msg.empty()) {
            display->SetTitleText("使用微信扫描二维码配置设备");
        } else {
            display->SetTitleText(activation_extra_msg.c_str());
            settings.EraseKey("extra_msg");
        }
    }
    display->SetVersionProfile("设备名:");

    std::string broad_name = settings.GetString("name");
    ESP_LOGI(TAG, "Read board name from nvs: %s", broad_name.c_str());
    display->SetVersionText(broad_name);

    std::string id = settings.GetString("id");
    ESP_LOGI(TAG, "Read board id from nvs: %s", id.c_str());
    
    std::string type = settings.GetString("type");
    ESP_LOGI(TAG, "Read board type from nvs: %s", type.c_str());

    std::string version = settings.GetString("version");
    ESP_LOGI(TAG, "Read board version from nvs: %s", version.c_str());

    int enable_4g = settings.GetInt("4g", 1);
    ESP_LOGI(TAG, "Read board enable_4g from nvs: %d", enable_4g);

    std::string code = settings.GetString("act_code");
    std::string activation_msg = settings.GetString("act_msg");
    size_t pos = activation_msg.find('\n');
    if (pos != std::string::npos) {
        activation_msg = activation_msg.substr(0, pos);
    }
    ESP_LOGI(TAG, "Read board activation_code from nvs: %s  activation_msg: %s", code.c_str(), activation_msg.c_str());
    if (!code.empty()) {
        std::string text = "设备码:" + code;
        display->SetChatMessage("system", text.c_str());
        display->SetTitleText(activation_msg.c_str());
        settings.EraseKey("act_code");
        settings.EraseKey("act_msg");
    }

    if (test_mode) {
        display->SetBgImage(nullptr, true); 
        display->SetTitleText("测试模式!!!!");
    }

    if (broad_name.empty()) {
        uint8_t mac[6];
    #if CONFIG_IDF_TARGET_ESP32P4
        esp_wifi_get_mac(WIFI_IF_AP, mac);
    #else
        ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    #endif
        char ssid[32];

        if (!type.empty()) {
            snprintf(ssid, sizeof(ssid), "%s-%02X%02X", type.c_str(), mac[4], mac[5]);
            broad_name = std::string(ssid);
        } else {
            snprintf(ssid, sizeof(ssid), "%s-%02X%02X", "小派", mac[4], mac[5]);
            broad_name = std::string(ssid);
        }
    }

    std::string broad_info = "{";
    broad_info += "\"id\":\"" + id + "\",";
    broad_info += "\"type\":\"" + type + "\",";
    broad_info += "\"version\":\"" + version + "\",";
    broad_info += "\"name\":\"" + broad_name + "\",";
    broad_info += "\"4g\":" + std::to_string(enable_4g) + "";
    broad_info += "}";

    ESP_LOGW(TAG, "Broad name: %s type: %s id: %s version: %s enable_4g: %d broad_info: %s", 
             broad_name.c_str(), type.c_str(), id.c_str(), version.c_str(), enable_4g, broad_info.c_str());

    TaskParam *param1 = new TaskParam{broad_name, broad_info, type};
    xTaskCreate([](void* args) {
        TaskParam* param = static_cast<TaskParam*>(args);
        BlufiTask(param->broad_name, param->broad_info);
        delete param;
        vTaskDelete(NULL);
    }, "BlufiTask", 1024 * 8, param1, 5, &g_BlufiTask_handle);

    TaskParam *param2 = new TaskParam{broad_name, broad_info, type};
    xTaskCreate([](void* args) {
        TaskParam* param = static_cast<TaskParam*>(args);
        RunWifiApMode(std::move(param->broad_type));
        delete param;
        vTaskDelete(NULL);
    }, "wifi_ap_task", 1024 * 8, param2, 5, &g_wifi_ap_task_handle);

    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "free sram: %u minimal sram: %u", free_sram, min_free_sram);
    int free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "free psram: %u minimal psram: %u", free_psram, min_free_psram);
}

void BlufiTask(const std::string& broad_name, const std::string& broad_info) {
    // bool res = blufi_initialise_bt(broad_name.c_str(), broad_info.c_str(), broad_info.size());
    ESP_LOGI(TAG, "run BlufiTask");

    wifi_credential_t wifi_cred = initialise_wifi_and_blufi(broad_name.c_str(), broad_info.c_str(), broad_info.size());
    if (wifi_cred.succ == 1) {
        ESP_LOGI(TAG, "BLUFI WiFi connected! SSID: %s", wifi_cred.ssid);
        SsidManager::GetInstance().AddSsid(wifi_cred.ssid, wifi_cred.password); //存储账号密码
        OnBlufiCustomEventCallBack(BLUFI_CUSTOM_WIFI_START_EVENT, NULL, 0);
        std::string wifi_str = "使用 " + std::string((char*)wifi_cred.ssid) + " wifi";
        Board::GetInstance().GetDisplay()->SetTitleText(wifi_str.c_str());
    } else {
        ESP_LOGE(TAG, "BLUFI WiFi connection failed");
    }

    ESP_LOGI(TAG, "BlufiTask done");
    RestartApp();
}

void RunWifiApMode(const std::string&& name) {
    ESP_LOGI(TAG, "run RunWifiApMode");
    // Start WiFi AP mode for configuration
    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage("zh-CN");
    wifi_ap.SetSsidPrefix(std::move(name));
    wifi_ap.SetJoinCallBack([](){
        ESP_LOGI(TAG, "wifi_ap join");
        uninitialise_blufi();
        Board::GetInstance().GetDisplay()->SetTitleText("收到连接，配网中");
        vTaskDelay(pdMS_TO_TICKS(50));
    });
    wifi_ap.SetDoneCallBack([](){
        ESP_LOGI(TAG, "wifi_ap done restart app");
        Board::GetInstance().GetDisplay()->SetTitleText("配网成功");
        RestartApp();
    });
    wifi_ap.Start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void OnBlufiCustomEventCallBack(BlufiCustomEvent_t event, void* data, int data_len) {
    ESP_LOGI(TAG, "Custom Event Received: %d, Data Length: %d", (int)event, data_len);

    if (event == BLUFI_CUSTOM_READY_EVENT)
        return;

    {
        Settings settings("network", true);
        std::string type = "type";
        settings.EraseKey(type.c_str());

        switch (event) {
        case BLUFI_CUSTOM_CONNECTED_EVENT: {
            auto& wifi_ap = WifiConfigurationAp::GetInstance();
            if (wifi_ap.IsStarted())
                wifi_ap.Stop();
            Board::GetInstance().GetDisplay()->SetTitleText("蓝牙连接成功");
            break;
        }
        case BLUFI_CUSTOM_READY_EVENT:
            break;          // 现在已经在 switch 里处理了，编译器不再抱怨
        case BLUFI_CUSTOM_4G_START_EVENT: {
            settings.SetInt(type.c_str(), 1);
            ESP_LOGI(TAG, "Set network type 1 to 4G in NVS");
            break;
        }
        case BLUFI_CUSTOM_WIFI_START_EVENT: {
            settings.SetInt(type.c_str(), 0);
            ESP_LOGI(TAG, "Set network type 0 to wifi in NVS");
            break;
        }
        default:
            break;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    {
        Settings settings("network", false);
        int network_type = settings.GetInt("type", -1);
        printf("!!!!!!! Current network type in NVS: %d\n", network_type);
    }

    if (event == BLUFI_CUSTOM_4G_START_EVENT) {
        Board::GetInstance().GetDisplay()->SetTitleText("打开4G");
        RestartApp();
    }
}

void RunOta() {
    auto display = Board::GetInstance().GetDisplay();
    display->SetTheme("dark");
    display->SetBgImage(&upgrade, false); //显示ota界面
    display->SetTitleText("系统更新中");
    display->SetStatusProfile("切勿切断电源，"); 
    display->SetVersionProfile("固件版本:");
    display->SetStatusText("请稍候...");

    if (!StartNetwork()) {
        ESP_LOGE(TAG, "Failed to start network, cannot proceed with OTA");
        return;
    }

    std::string url;
    {
        Settings settings("board", false);
        url = settings.GetString("ota_url");
    }

    std::string version;
    {
        Settings settings("board", false);
        version = settings.GetString("ota_v");
        if (version.empty()) {
            version = settings.GetString("version");
        }
    }

    std::string md5;
    {
        Settings settings("board", false);
        md5 = settings.GetString("ota_md5");
    }

    ESP_LOGI(TAG, "Starting OTA process with URL: %s, Version: %s md5: %s", url.c_str(), version.c_str(), md5.c_str());

    if (!url.empty()) {
        g_ota.SetFirmwareUrl(url);
        display->SetVersionText(version);

        for (int count = 0; count < 3; ++count) { //最多下载 3 次
            bool res = g_ota.StartUpgrade(md5, [](const std::string& status, int progress, size_t speed) {
                ESP_LOGI(TAG, "Upgrade progress: %s %d%%, speed: %u bytes/s", status.c_str(), progress, (unsigned int)speed);
                char buffer[32];
                if (speed > 0 ) {
                    snprintf(buffer, sizeof(buffer), "%s %d%% %uKB/s", status.c_str(), progress, speed / 1024);
                } else {
                    snprintf(buffer, sizeof(buffer), "%s %d%%", status.c_str(), progress);
                }
                Board::GetInstance().GetDisplay()->SetStatusText(std::string(buffer, strlen(buffer)));
            });

            if (!res) {
                // If upgrade success, the device will reboot and never reach here
                ESP_LOGW(TAG, "Firmware upgrade failed...  restart blufi process count:%d", count);
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                RestartApp();
                return;
            }
        }
    }

    ESP_LOGW(TAG, "OTA process completed with no action, restarting application...");

    {
        Settings settings("board", true);
        settings.SetInt("ota_fail", 1);
        ESP_LOGW(TAG, "Set ota_fail flag in NVS");
    }

    RestartApp();
}

bool StartNetwork() {
    // If no WiFi SSID is configured, enter WiFi configuration mode
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGE(TAG, "No WiFi SSID configured, cannot start network");
        return false;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([]() {
        ESP_LOGI(TAG, "Scanning WiFi networks...");
    });
    wifi_station.OnConnect([](const std::string& ssid) {
        ESP_LOGI(TAG, "Connecting to WiFi SSID: %s...", ssid.c_str());
    });
    wifi_station.OnConnected([](const std::string& ssid) {
        ESP_LOGI(TAG, "Connected to WiFi SSID: %s", ssid.c_str());
    });
    wifi_station.Start();

    // Try to connect to WiFi, if failed, launch the WiFi configuration AP
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        ESP_LOGE(TAG, "Failed to connect to WiFi within timeout");
        return false;
    }

    ESP_LOGI(TAG, "wifi start network success, ip address: %s", wifi_station.GetIpAddress().c_str());

    return true;
}

void RestartApp() {
    /* 设置下一次启动分区为 ota_0（主工程的应用在ota_0） */
    const esp_partition_t *ota0 = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP,
            ESP_PARTITION_SUBTYPE_APP_OTA_0,
            NULL);
    ESP_ERROR_CHECK( esp_ota_set_boot_partition(ota0) ); 

    ESP_LOGI(TAG, "BLUFI configuration completed, restarting...");
    ESP_LOGI(TAG, "BLUFI configuration completed restarting ota_0 partition:%s at offset 0x%lx subtype:%d, restarting...\n",
        ota0->label, ota0->address, ota0->subtype);
    vTaskDelay(pdMS_TO_TICKS(500));
    // 执行重启
    esp_restart();
}