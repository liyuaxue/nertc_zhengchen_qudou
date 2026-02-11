import sys
import os
import glob
import struct

def get_gif_dimensions(gif_path):
    """从GIF文件头读取宽度和高度（不依赖PIL）"""
    try:
        with open(gif_path, 'rb') as f:
            # 读取GIF签名（6字节）
            signature = f.read(6)
            if signature not in [b'GIF87a', b'GIF89a']:
                raise ValueError("不是有效的GIF文件")
            
            # 读取宽度和高度（各2字节，little-endian）
            width, height = struct.unpack('<HH', f.read(4))
            return width, height
    except Exception as e:
        print(f"警告: 无法读取GIF尺寸: {e}")
        return None, None

def gif_to_c_array(gif_path, output_path):
    try:
        # 读取GIF文件获取尺寸
        gif_width, gif_height = get_gif_dimensions(gif_path)
        if gif_width is None or gif_height is None:
            # 如果无法读取尺寸，使用默认值
            print(f"警告: 无法读取 {os.path.basename(gif_path)} 的尺寸，使用默认值 320x240")
            gif_width, gif_height = 320, 240
        
        # 读取GIF二进制数据
        with open(gif_path, 'rb') as f:
            gif_data = f.read()
        
        # 从GIF文件名生成数组名称（标准：文件名直接作为数组名基础）
        # 例如：boot_animation.gif -> boot_animation_map, charging.gif -> charging_map
        array_name = os.path.splitext(os.path.basename(gif_path))[0]
        # 将数组名称转换为大写，用于生成常量宏名
        array_name_upper = array_name.upper().replace('-', '_')
        macro = f"LV_ATTRIBUTE_IMG_{array_name_upper}"
        
        # 生成C文件头部（包含头文件和宏定义）
        header = f'''#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif


#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef {macro}
#define {macro}
#endif
const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST {macro} uint8_t {array_name}_map[] = {{
'''
        
        # 生成数组数据（每行16个字节）
        array_data = ""
        for i in range(0, len(gif_data), 16):
            hex_line = ", ".join(f"0x{byte:02x}" for byte in gif_data[i:i+16])
            array_data += f"  {hex_line},\n"
        
        # 移除最后一个逗号并闭合数组
        array_data = array_data.rstrip(",\n") + "\n};\n"
        
        # 生成 lv_img_dsc_t 结构体（用于GIF原始数据）
        # 注意：对于GIF，我们使用原始数据指针，而不是标准的图像描述符
        # 但为了匹配头文件声明，我们创建一个简单的结构体
        struct_name = array_name
    
        
        struct_def = f'''
const lv_img_dsc_t {struct_name} = {{
  .header.cf = LV_COLOR_FORMAT_RAW,
  .header.w = {gif_width},
  .header.h = {gif_height},
  .data_size = {len(gif_data)},
  .data = {array_name}_map,
}};
'''
        
        # 组合完整的C代码
        c_code = header + array_data + struct_def
        
        # 写入输出文件
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(c_code)
            
        print(f"✓ 成功转换: {os.path.basename(gif_path)} -> {os.path.basename(output_path)}")
        print(f"  尺寸: {gif_width}x{gif_height}")
        print(f"  数组大小: {len(gif_data)} 字节")
        return True
        
    except Exception as e:
        print(f"✗ 转换失败: {gif_path}")
        print(f"  错误: {str(e)}")
        return False

def convert_all_gifs(directory=None):
    """自动识别目录中的所有GIF文件并转换为C数组"""
    if directory is None:
        directory = os.path.dirname(os.path.abspath(__file__))
    
    # 查找所有GIF文件
    gif_pattern = os.path.join(directory, "*.gif")
    gif_files = glob.glob(gif_pattern)
    
    if not gif_files:
        print(f"在目录 {directory} 中未找到GIF文件")
        return
    
    print(f"找到 {len(gif_files)} 个GIF文件，开始转换...\n")
    
    success_count = 0
    for gif_path in gif_files:
        # 生成对应的.c文件名
        base_name = os.path.splitext(gif_path)[0]
        output_path = base_name + ".c"
        
        if gif_to_c_array(gif_path, output_path):
            success_count += 1
        print()  # 空行分隔
    
    print(f"转换完成: {success_count}/{len(gif_files)} 个文件成功转换")

if __name__ == "__main__":
    # 如果提供了命令行参数，使用旧的方式（兼容性）
    if len(sys.argv) == 3:
        gif_to_c_array(sys.argv[1], sys.argv[2])
    else:
        # 自动模式：转换当前目录下的所有GIF文件
        convert_all_gifs()