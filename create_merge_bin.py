#!/usr/bin/env python3
import subprocess, sys, csv, io, os, argparse, shutil
from datetime import datetime


CHIP        = "esp32s3"
DEFAULT_PARTITION_TABLE = "partitions/v2/16m.csv"
PART_TBL    = "build/partition_table/partition-table.bin"
BOOTLOADER  = "build/bootloader/bootloader.bin"
OUT         = "build/merged-bin.bin"

parser = argparse.ArgumentParser(description="ESP32-S3 create merged-bin")
parser.add_argument("-v", "--version", required=True,
                        help="input your verison like 1.0.0")
parser.add_argument( "-c", "--clean",
    action="store_true",      # 关键：存布尔值，不消耗参数
    required=False,           # 可选
    help="执行清理操作（无需额外数值）"
)
args = parser.parse_args()

# 分区名 -> 本地文件名
FILE_MAP = {
    "custom": "config.bin",
    "ota_0":  "build/xiaozhi.bin",
    "blufi":  "third_party/blufi_app/bin/blufi_app.bin",
    "assets": "main/assets.bin",
}

# 检查文件是否存在
for name, fname in FILE_MAP.items():
    if not os.path.exists(fname):
        print(f"Error: File for partition '{name}' not found: {fname}")
        sys.exit(1)

# 1. 读取分区表
try:
    with open(DEFAULT_PARTITION_TABLE, "r") as csvfile:
        reader = csv.reader(csvfile)
        entries = {}
        for row in reader:
            if not row or row[0].startswith("#"):   # 跳过空行和注释
                continue
            name, typ, sub, offset, size, flags = row
            entries[name.strip()] = {
                "Type":    typ.strip(),
                "SubType": sub.strip(),
                "Offset":  offset.strip(),
                "Size":    size.strip(),
                "Flags":   flags.strip(),
            }
except Exception as e:
    print(f"Error parsing partition table: {e}")
    sys.exit(1)
#打印分区表信息
for name, entry in entries.items():
    print(f"Partition '{name}': Offset={entry['Offset']} Size={entry['Size']}")

# 2. 拼命令
cmd = ["esptool.py", "--chip", CHIP, "merge_bin", "-o", OUT,
       "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
       "0x0000", BOOTLOADER,
       "0x8000", PART_TBL]

for name, fname in FILE_MAP.items():
    off = int(entries[name]["Offset"], 16)
    cmd += [hex(off), fname]

# 3. 执行 并打印命令
print("Running command:", " ".join(cmd))
subprocess.check_call(cmd)

# 4. 将merge-bin、xiaozhi.bin elf文件统一整理到build/release_YY:MM:DD-HH:MM::SS这目录下
now_time = datetime.now()
ts = now_time.strftime("%y:%m:%d-%H:%M:%S")
dir_name = f"release_v{args.version}_{ts}"
build_root = "build"
os.makedirs(build_root, exist_ok=True)   # 确保 build 存在
target_path = os.path.join(build_root, dir_name)
os.makedirs(target_path)

ts_compact = now_time.strftime("%y%m%d%H%M%S")
new_name = f"xiaozhi_zhengchen_v{args.version}_{ts_compact}_full.bin"
dst_path = os.path.join(target_path, new_name)
if not os.path.isfile(OUT):
    raise SystemExit(f"源文件不存在：{OUT}")
shutil.copy2(OUT, dst_path)
new_name_ota = f"xiaozhi_zhengchen_v{args.version}_{ts_compact}_ota.bin"
dst_ota_path = os.path.join(target_path, new_name_ota)
if not os.path.isfile("build/xiaozhi.bin"):
    raise SystemExit(f"源文件不存在：build/xiaozhi.bin")
shutil.copy2("build/xiaozhi.bin", dst_ota_path)
new_name_elf = f"xiaozhi_zhengchen_v{args.version}_{ts_compact}.elf"
dst_elf_path = os.path.join(target_path, new_name_elf)
if not os.path.isfile("build/xiaozhi.elf"):
    raise SystemExit(f"源文件不存在：build/xiaozhi.elf")
shutil.copy2("build/xiaozhi.elf", dst_elf_path)

print("打包成功，文件存放在", {target_path})