#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
将xiaozhi项目固件与config.bin、blufi_app.bin打包成一个大固件
"""

import argparse
import os
import sys
import csv
import subprocess
from pathlib import Path

# 默认配置
DEFAULT_PARTITION_TABLE = "partitions/v2/16m.csv"
DEFAULT_TARGET = "esp32-s3"
DEFAULT_BUILD_DIR = "build"
DEFAULT_OUTPUT = "firmware_combined.bin"

def parse_partition_table(partition_table_path):
    """解析分区表，获取各分区的offset和size"""
    partitions = {}
    try:
        with open(partition_table_path, "r") as csvfile:
            reader = csv.reader(csvfile)
            for row in reader:
                # 跳过注释行和空行
                if not row or row[0].strip().startswith("#"):
                    continue
                if len(row) >= 5:
                    name = row[0].strip()
                    offset = int(row[3].strip(), 16) if row[3].strip() else None
                    size = int(row[4].strip(), 16) if row[4].strip() else None
                    if offset is not None and size is not None:
                        partitions[name] = {"offset": offset, "size": size}
    except Exception as e:
        print(f"错误: 解析分区表失败: {e}")
        sys.exit(1)
    return partitions

def check_file_exists(filepath, description):
    """检查文件是否存在"""
    if not os.path.exists(filepath):
        print(f"错误: {description} 不存在: {filepath}")
        sys.exit(1)

def get_blufi_bin_path(target):
    """根据目标芯片获取blufi固件路径"""
    if target == "esp32-c3":
        blufi_name = "blufi_app_c3.bin"
    else:
        blufi_name = "blufi_app.bin"
    blufi_path = f"third_party/blufi_app/bin/{blufi_name}"
    return blufi_path

def create_flash_args_file(flash_args_path, build_dir, config_bin, blufi_bin, partitions, target):
    """创建flash_args文件，包含所有需要合并的bin文件"""
    flash_args = []
    
    # 添加flash参数
    flash_mode = "dio"
    flash_freq = "80m"
    flash_size = "16MB"
    flash_args.append(f"--flash_mode {flash_mode} --flash_freq {flash_freq} --flash_size {flash_size}")
    
    # bootloader (通常在0x0)
    bootloader_path = os.path.join(build_dir, "bootloader", "bootloader.bin")
    if os.path.exists(bootloader_path):
        flash_args.append(f"0x0 {os.path.abspath(bootloader_path)}")
    else:
        print(f"警告: bootloader.bin 不存在: {bootloader_path}")
    
    # partition table (通常在0x8000)
    partition_table_path = os.path.join(build_dir, "partition_table", "partition-table.bin")
    if os.path.exists(partition_table_path):
        flash_args.append(f"0x8000 {os.path.abspath(partition_table_path)}")
    else:
        print(f"警告: partition-table.bin 不存在: {partition_table_path}")
    
    # ota_data (通常在0xd000)
    ota_data_path = os.path.join(build_dir, "ota_data_initial.bin")
    if os.path.exists(ota_data_path):
        flash_args.append(f"0xd000 {os.path.abspath(ota_data_path)}")
    else:
        print(f"警告: ota_data_initial.bin 不存在: {ota_data_path}")
    
    # config.bin (custom分区)
    if "custom" in partitions:
        custom_offset = partitions["custom"]["offset"]
        check_file_exists(config_bin, "config.bin")
        flash_args.append(f"{hex(custom_offset)} {os.path.abspath(config_bin)}")
    else:
        print("警告: 分区表中未找到custom分区，跳过config.bin")
    
    # xiaozhi固件 (ota_0分区)
    if "ota_0" in partitions:
        ota_0_offset = partitions["ota_0"]["offset"]
        xiaozhi_bin = os.path.join(build_dir, "xiaozhi.bin")
        check_file_exists(xiaozhi_bin, "xiaozhi固件")
        flash_args.append(f"{hex(ota_0_offset)} {os.path.abspath(xiaozhi_bin)}")
    else:
        print("错误: 分区表中未找到ota_0分区")
        sys.exit(1)
    
    # blufi_app.bin (blufi分区)
    if "blufi" in partitions:
        blufi_offset = partitions["blufi"]["offset"]
        check_file_exists(blufi_bin, "blufi_app.bin")
        flash_args.append(f"{hex(blufi_offset)} {os.path.abspath(blufi_bin)}")
    else:
        print("警告: 分区表中未找到blufi分区，跳过blufi_app.bin")
    
    # assets (如果存在)
    if "assets" in partitions:
        assets_path = os.path.join(build_dir, "..", "main", "assets.bin")
        # 也尝试在build目录中查找
        if not os.path.exists(assets_path):
            assets_path = os.path.join(build_dir, "assets.bin")
        if os.path.exists(assets_path):
            assets_offset = partitions["assets"]["offset"]
            flash_args.append(f"{hex(assets_offset)} {os.path.abspath(assets_path)}")
        else:
            print(f"警告: assets.bin 不存在，跳过")
    
    # 写入flash_args文件
    with open(flash_args_path, "w") as f:
        f.write("\n".join(flash_args))
    
    print(f"已创建flash_args文件: {flash_args_path}")
    print("包含以下bin文件:")
    for line in flash_args:
        print(f"  {line}")

def merge_binaries(flash_args_path, output_path, target):
    """使用esptool合并bin文件"""
    # 使用esptool的merge_bin功能
    # 格式: esptool.py --chip <target> merge_bin -o <output> @<flash_args_file>
    esptool_cmd = [
        "esptool.py",  # 修改这里
        "--chip", target,
        "merge_bin",
        "-o", os.path.abspath(output_path),
        f"@{os.path.abspath(flash_args_path)}"
    ]
    
    print(f"\n执行合并命令:")
    print(" ".join(esptool_cmd))
    print()
    
    try:
        result = subprocess.run(esptool_cmd, check=True, capture_output=True, text=True)
        print(result.stdout)
        if result.stderr:
            print(result.stderr)
        print(f"\n✓ 固件合并成功: {output_path}")
        file_size = os.path.getsize(output_path)
        print(f"  文件大小: {file_size / 1024 / 1024:.2f} MB ({file_size} 字节)")
    except subprocess.CalledProcessError as e:
        print(f"错误: 合并失败")
        print(f"stdout: {e.stdout}")
        print(f"stderr: {e.stderr}")
        sys.exit(1)
    except FileNotFoundError:
        print("错误: 未找到esptool.py，请确保已安装ESP-IDF并激活环境")
        print("提示: 请运行 '. $IDF_PATH/export.sh' (Linux/Mac) 或 'call %IDF_PATH%\\export.bat' (Windows) 激活ESP-IDF环境")
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(
        description="将xiaozhi项目固件与config.bin、blufi_app.bin打包成一个大固件",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 使用默认配置打包
  python scripts/package_firmware.py

  # 指定自定义路径
  python scripts/package_firmware.py --config config.bin --output firmware.bin

  # 指定目标芯片和分区表
  python scripts/package_firmware.py --target esp32-c3 --partition-table partitions/v2/8m.csv
        """
    )
    parser.add_argument(
        "--config",
        default="config.bin",
        help="config.bin文件路径 (默认: config.bin)"
    )
    parser.add_argument(
        "--blufi",
        default=None,
        help="blufi_app.bin文件路径 (默认: 根据target自动选择)"
    )
    parser.add_argument(
        "--build-dir",
        default=DEFAULT_BUILD_DIR,
        help=f"构建目录 (默认: {DEFAULT_BUILD_DIR})"
    )
    parser.add_argument(
        "--output", "-o",
        default=DEFAULT_OUTPUT,
        help=f"输出文件路径 (默认: {DEFAULT_OUTPUT})"
    )
    parser.add_argument(
        "--partition-table",
        default=DEFAULT_PARTITION_TABLE,
        help=f"分区表文件路径 (默认: {DEFAULT_PARTITION_TABLE})"
    )
    parser.add_argument(
        "--target",
        default=DEFAULT_TARGET,
        help=f"目标芯片类型 (默认: {DEFAULT_TARGET})"
    )
    parser.add_argument(
        "--keep-flash-args",
        action="store_true",
        help="保留临时flash_args文件"
    )
    
    args = parser.parse_args()
    
    # 切换到项目根目录
    script_dir = Path(__file__).resolve().parent.parent
    os.chdir(script_dir)
    
    print("=" * 60)
    print("xiaozhi固件打包工具")
    print("=" * 60)
    print(f"分区表: {args.partition_table}")
    print(f"目标芯片: {args.target}")
    print(f"构建目录: {args.build_dir}")
    print(f"输出文件: {args.output}")
    print()
    
    # 解析分区表
    print("解析分区表...")
    partitions = parse_partition_table(args.partition_table)
    print(f"找到 {len(partitions)} 个分区")
    for name, info in partitions.items():
        print(f"  {name}: offset=0x{info['offset']:X}, size=0x{info['size']:X}")
    print()
    
    # 检查必要文件
    print("检查必要文件...")
    check_file_exists(args.config, "config.bin")
    
    if args.blufi:
        check_file_exists(args.blufi, "blufi_app.bin")
        blufi_bin = args.blufi
    else:
        blufi_bin = get_blufi_bin_path(args.target)
        check_file_exists(blufi_bin, "blufi_app.bin")
    
    xiaozhi_bin = os.path.join(args.build_dir, "xiaozhi.bin")
    check_file_exists(xiaozhi_bin, "xiaozhi固件")
    
    # 检查build目录是否存在
    if not os.path.exists(args.build_dir):
        print(f"错误: 构建目录不存在: {args.build_dir}")
        print("提示: 请先运行 'idf.py build' 构建项目")
        sys.exit(1)
    
    print("✓ 所有必要文件检查通过")
    print()
    
    # 创建临时flash_args文件
    flash_args_path = os.path.join(args.build_dir, "package_flash_args")
    print("创建flash_args文件...")
    create_flash_args_file(
        flash_args_path,
        args.build_dir,
        args.config,
        blufi_bin,
        partitions,
        args.target
    )
    print()
    
    # 合并bin文件
    print("开始合并bin文件...")
    merge_binaries(flash_args_path, args.output, args.target)
    
    # 清理临时文件
    if not args.keep_flash_args and os.path.exists(flash_args_path):
        os.remove(flash_args_path)
        print(f"已清理临时文件: {flash_args_path}")
    
    print()
    print("=" * 60)
    print("打包完成!")
    print("=" * 60)

if __name__ == "__main__":
    main()

