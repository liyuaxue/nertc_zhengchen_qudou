# Set default BUILTIN_TEXT_FONT and BUILTIN_ICON_FONT
if(NOT BUILTIN_TEXT_FONT)
    set(BUILTIN_TEXT_FONT font_puhui_14_1)
endif()

# 基础源文件
set(SOURCES
    "blufi_app.cc"
    "blufi_wifi.c"
    "blufi_init.c"
    "blufi_security.c"
    "settings.cc"
    "blufi_http_client.cc"
    "ota.cc"
    "display/display.cc"
    "display/lcd_display.cc"
    "display/lvgl_display/lvgl_display.cc"
    "display/lvgl_display/lvgl_theme.cc"
    "display/lvgl_display/lvgl_font.cc"
    "display/lvgl_display/lvgl_image.cc"
    "display/esp_lcd_panel_st7789_fix.c"
    "button/button.cc"
    "boards/i2c_device.cc"
    "assert/upgrade.c"
    "assert/blufi.c"
    "assert/host.c"
)

# 基础头文件目录
set(INCLUDE_DIRS_LIST
    "."
    "display"
    "display/lvgl_display"
    "button"
    "boards"
)

# 根据板子类型添加对应的源文件和头文件目录
if(CONFIG_BOARD_TYPE_ZHENGCHEN_QUDOU)
    list(APPEND SOURCES "boards/zhengchen_qudou/qudou_board.cc")
    list(APPEND INCLUDE_DIRS_LIST "boards/zhengchen_qudou")
endif()

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS_LIST}
    REQUIRES bt esp_wifi mbedtls nvs_flash app_update json esp-tls esp_http_client efuse esp-wifi-connect esp_psram
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT}
)