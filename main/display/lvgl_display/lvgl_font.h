#pragma once

#include <lvgl.h>


class LvglFont {
public:
    virtual const lv_font_t* font() const = 0;
    virtual ~LvglFont() = default;
};

// Built-in font
class LvglBuiltInFont : public LvglFont {
public:
    LvglBuiltInFont(const lv_font_t* font) : font_(font) {}
    virtual const lv_font_t* font() const override { return font_; }

private:
    const lv_font_t* font_;
};


class LvglCBinFont : public LvglFont {
public:
    LvglCBinFont(void* data, bool owns_data = false);
    virtual ~LvglCBinFont();
    virtual const lv_font_t* font() const override { return font_; }

private:
    lv_font_t* font_;
    void* data_copy_;  // Owned copy of font data if owns_data is true
    bool owns_data_;
};
