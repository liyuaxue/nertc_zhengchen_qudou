#include "lvgl_font.h"
#include <cbin_font.h>
#include <esp_heap_caps.h>

LvglCBinFont::LvglCBinFont(void* data, bool owns_data) 
    : font_(nullptr), data_copy_(nullptr), owns_data_(owns_data) {
    font_ = cbin_font_create(static_cast<uint8_t*>(data));
    if (owns_data) {
        data_copy_ = data;
    }
}

LvglCBinFont::~LvglCBinFont() {
    if (font_ != nullptr) {
        cbin_font_delete(font_);
    }
    if (owns_data_ && data_copy_ != nullptr) {
        heap_caps_free(data_copy_);
    }
}