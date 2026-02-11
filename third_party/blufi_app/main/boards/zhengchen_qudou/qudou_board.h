
#ifndef _QUDOU_BOARD_H_
#define _QUDOU_BOARD_H_

#include "boards/board.h"
#include "config.h"
#include "esp_lcd_types.h"
#include <driver/i2c_master.h>

class Pca9557;
class QudouBoard : public Board {
public:
    QudouBoard();
    ~QudouBoard() override;

    // 实现 Board 接口
    void InitializeButtons(ButtonClickCallback on_click) override;
    void InitializeSpi() override;
    void InitializeLcdDisplay() override;

    LcdDisplay* GetDisplay() const override { return display_; }
    Button* GetButton(int index) const override;

private:
    Button button_;
    LcdDisplay* display_;
    Pca9557* pca9557_;
    i2c_master_bus_handle_t i2c_bus_;
};

#endif // _QUDOU_BOARD_H_
