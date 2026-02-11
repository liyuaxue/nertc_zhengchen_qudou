
#include "qudou_board.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "display/lcd_display.h"
#include "i2c_device.h"


static const char* TAG = "QudouBoard";

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



QudouBoard::QudouBoard()
    : button_(BOOT_BUTTON_GPIO)
    , display_(nullptr) {

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

QudouBoard::~QudouBoard() {
    if (display_ != nullptr) {
        delete display_;
        display_ = nullptr;
    }
}

Button* QudouBoard::GetButton(int) const {
    return const_cast<Button*>(&button_);
}

void QudouBoard::InitializeButtons(ButtonClickCallback on_click) {
    button_.OnClick([this, on_click]() {
        if (on_click) on_click();
    });
}

void QudouBoard::InitializeSpi() {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
    buscfg.miso_io_num = DISPLAY_SPI_MISO_PIN;
    buscfg.sclk_io_num = DISPLAY_SPI_SCLK_PIN;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

void QudouBoard::InitializeLcdDisplay() {
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    // 液晶屏控制IO初始化
    ESP_LOGI(TAG, "Install panel IO");
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
    ESP_LOGI(TAG, "Install LCD driver");
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

    // 初始化并打开背光
    if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
        gpio_config_t bk_gpio_config = {
            .pin_bit_mask = 1ULL << DISPLAY_BACKLIGHT_PIN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
        
        // DISPLAY_BACKLIGHT_OUTPUT_INVERT 为 true 时，低电平点亮
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT ? 0 : 1);
        ESP_LOGI(TAG, "Backlight turned on (GPIO %d)", DISPLAY_BACKLIGHT_PIN);
    }
}

// 注册板子
DECLARE_BOARD(QudouBoard)
