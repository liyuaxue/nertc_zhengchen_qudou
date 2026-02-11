#ifndef _BOARDS_BOARD_H_
#define _BOARDS_BOARD_H_

#include <functional>
#include "display/lcd_display.h"
#include "button/button.h"

// 按钮回调函数类型
using ButtonClickCallback = std::function<void()>;

// 板子创建函数声明（由具体板子实现）
void* create_board();

class Board {
public:
    // 单例模式获取板子实例
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    Board() = default;
    virtual ~Board() = default;

    // 禁用拷贝构造和赋值
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;

    virtual void InitializeButtons(ButtonClickCallback on_click) = 0;
    virtual void InitializeSpi() = 0;
    virtual void InitializeLcdDisplay() = 0;

    // 获取成员 - 纯虚函数，由子类实现
    virtual LcdDisplay* GetDisplay() const = 0;
    virtual Button* GetButton(int index) const = 0;
};

// 板子注册宏 - 在具体板子的 .cc 文件中使用
#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    static BOARD_CLASS_NAME instance; \
    return &instance; \
}

#endif // _BOARDS_BOARD_H_