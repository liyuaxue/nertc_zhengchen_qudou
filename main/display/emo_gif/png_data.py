#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
自动转换同目录下所有PNG为C数组
"""

import sys
import os
from PIL import Image

# 添加Image_Converter路径（向上查找 scripts/Image_Converter）
script_dir = os.path.dirname(os.path.abspath(__file__))
image_converter_path = None
current_dir = script_dir
while True:
    candidate = os.path.join(current_dir, 'scripts', 'Image_Converter')
    if os.path.isdir(candidate):
        image_converter_path = candidate
        break
    parent = os.path.dirname(current_dir)
    if parent == current_dir:
        break
    current_dir = parent

if image_converter_path:
    sys.path.insert(0, image_converter_path)

def png_to_c_array(png_path, output_path):
    """转换指定PNG为C数组"""
    try:
        from LVGLImage import LVGLImage, ColorFormat, CompressMethod  # type: ignore
    except ImportError as e:
        print(f"✗ 导入错误: {str(e)}")
        print("请确保已安装所需依赖:")
        print("  pip install pillow pypng lz4")
        return False
    except Exception as e:
        print(f"✗ 转换失败: {str(e)}")
        import traceback
        traceback.print_exc()
        return False

    if not os.path.exists(png_path):
        print(f"错误: 找不到文件 {png_path}")
        return False

    print(f"正在读取 {os.path.basename(png_path)}...")

    # 检测PNG是否有透明通道
    has_alpha = False
    try:
        with Image.open(png_path) as pil_img:
            has_alpha = pil_img.mode in ('RGBA', 'LA') or (pil_img.mode == 'P' and 'transparency' in pil_img.info)
    except Exception as e:
        print(f"警告: 无法检测透明度，使用RGB565格式: {e}")

    # 根据是否有透明通道选择格式
    if has_alpha:
        print("检测到透明通道，使用RGB565A8格式（支持透明度）...")
        color_format = ColorFormat.RGB565A8
    else:
        print("使用RGB565格式（适合LCD显示，节省内存）...")
        color_format = ColorFormat.RGB565
    
    img = LVGLImage().from_png(png_path, color_format)

    print("正在生成C数组文件...")
    img.to_c_array(output_path, compress=CompressMethod.NONE)

    print(f"✓ 成功转换: {os.path.basename(png_path)} -> {os.path.basename(output_path)}")

    if os.path.exists(output_path):
        file_size = os.path.getsize(output_path)
        print(f"  生成的文件大小: {file_size} 字节")

    return True


def convert_all_pngs(directory=None):
    """自动识别目录中的所有PNG文件并转换为C数组"""
    if directory is None:
        directory = os.path.dirname(os.path.abspath(__file__))

    png_files = []
    for entry in os.listdir(directory):
        if entry.lower().endswith(".png"):
            png_files.append(os.path.join(directory, entry))

    if not png_files:
        print(f"在目录 {directory} 中未找到PNG文件")
        return False

    print(f"找到 {len(png_files)} 个PNG文件，开始转换...\n")

    success_count = 0
    for png_path in png_files:
        output_path = os.path.splitext(png_path)[0] + ".c"
        if png_to_c_array(png_path, output_path):
            success_count += 1
        print()

    print(f"转换完成: {success_count}/{len(png_files)} 个文件成功转换")
    return success_count == len(png_files)

if __name__ == "__main__":
    print("=" * 50)
    print("PNG 转换工具")
    print("=" * 50)
    print()

    if convert_all_pngs():
        print()
        print("=" * 50)
        print("转换完成！")
        print("=" * 50)
        sys.exit(0)
    else:
        print()
        print("=" * 50)
        print("转换失败，请检查错误信息")
        print("=" * 50)
        sys.exit(1)