# GIF表情组件
# 
# 这个组件包含了3个GIF表情资源：
# - staticstate (静态状态/中性表情)
# - sad (悲伤表情)
# - happy (开心表情)

# 收集所有GIF源文件
file(GLOB GIF_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
# 添加当前目录下的GIF源文件（包括开机动画）
file(GLOB GIF_SOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
list(APPEND GIF_SOURCES ${GIF_SOURCES_DIR})

# 检查是否找到GIF源文件
list(LENGTH GIF_SOURCES GIF_COUNT)
if(GIF_COUNT EQUAL 0)
    message(WARNING "未找到GIF源文件，请确保src目录中包含.c文件")
endif()

# 注册组件
idf_component_register(
    SRCS ${GIF_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES "lvgl"
)

# 添加编译定义
target_compile_definitions(${COMPONENT_LIB}
    PRIVATE EMOJI_GIF_VERSION="2.0.5"
)

# 输出调试信息
message(STATUS "Emoji GIF Component:")
message(STATUS "  Found ${GIF_COUNT} GIF source files")
message(STATUS "  Include directory: ${CMAKE_CURRENT_SOURCE_DIR}/include") 