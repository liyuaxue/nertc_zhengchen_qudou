# 固件打包工具使用说明

## 功能

`package_firmware.py` 脚本用于将xiaozhi项目固件与 `config.bin`、`blufi_app.bin` 打包成一个大固件文件。

## 使用方法

### 基本用法

```bash
# 使用默认配置打包
python scripts/package_firmware.py
```

### 指定参数

```bash
# 指定自定义config.bin路径
python scripts/package_firmware.py --config config.bin

# 指定输出文件路径
python scripts/package_firmware.py --output firmware.bin

# 指定目标芯片类型
python scripts/package_firmware.py --target esp32-c3

# 指定分区表
python scripts/package_firmware.py --partition-table partitions/v2/8m.csv

# 完整示例
python scripts/package_firmware.py \
    --config config.bin \
    --output firmware_combined.bin \
    --target esp32-s3 \
    --partition-table partitions/v2/16m.csv
```

## 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--config` | config.bin文件路径 | `config.bin` |
| `--blufi` | blufi_app.bin文件路径 | 根据target自动选择 |
| `--build-dir` | 构建目录 | `build` |
| `--output`, `-o` | 输出文件路径 | `firmware_combined.bin` |
| `--partition-table` | 分区表文件路径 | `partitions/v2/16m.csv` |
| `--target` | 目标芯片类型 | `esp32-s3` |
| `--keep-flash-args` | 保留临时flash_args文件 | 否 |

## 工作流程

1. **解析分区表**: 读取分区表文件，获取各分区的offset和size
2. **检查文件**: 验证所有必要的bin文件是否存在
3. **创建flash_args**: 生成包含所有bin文件地址的临时文件
4. **合并固件**: 使用esptool的merge_bin功能合并所有bin文件
5. **清理**: 删除临时文件（除非使用--keep-flash-args）

## 打包内容

脚本会将以下文件合并到输出固件中：

- **bootloader.bin** (0x0) - 引导程序
- **partition-table.bin** (0x8000) - 分区表
- **ota_data_initial.bin** (0xd000) - OTA数据
- **config.bin** (custom分区, 默认0x10000) - 配置文件
- **xiaozhi.bin** (ota_0分区, 默认0x30000) - 主应用固件
- **blufi_app.bin** (blufi分区, 默认0x5B0000) - 蓝牙配网固件
- **assets.bin** (assets分区, 如果存在) - 资源文件

## 注意事项

1. **构建项目**: 使用前需要先运行 `idf.py build` 构建项目
2. **config.bin**: 确保 `config.bin` 文件已生成（可通过 `python config.py --build` 生成）
3. **分区表**: 确保使用的分区表与实际硬件配置匹配
4. **ESP-IDF环境**: 需要激活ESP-IDF环境，确保esptool可用

## 示例输出

```
============================================================
xiaozhi固件打包工具
============================================================
分区表: partitions/v2/16m.csv
目标芯片: esp32-s3
构建目录: build
输出文件: firmware_combined.bin

解析分区表...
找到 7 个分区
  nvs: offset=0x9000, size=0x4000
  otadata: offset=0xD000, size=0x2000
  phy_init: offset=0xF000, size=0x1000
  custom: offset=0x10000, size=0x20000
  ota_0: offset=0x30000, size=0x580000
  blufi: offset=0x5B0000, size=0x280000
  assets: offset=0x830000, size=0x7D0000

检查必要文件...
✓ 所有必要文件检查通过

创建flash_args文件...
已创建flash_args文件: build/package_flash_args
包含以下bin文件:
  --flash_mode dio --flash_freq 80m --flash_size 16MB
  0x0 D:\...\build\bootloader\bootloader.bin
  0x8000 D:\...\build\partition_table\partition-table.bin
  0xd000 D:\...\build\ota_data_initial.bin
  0x10000 D:\...\config.bin
  0x30000 D:\...\build\xiaozhi.bin
  0x5B0000 D:\...\third_party\blufi_app\bin\blufi_app.bin

开始合并bin文件...
✓ 固件合并成功: firmware_combined.bin
  文件大小: 15.23 MB (15974400 字节)

============================================================
打包完成!
============================================================
```

## 故障排除

### 错误: 未找到esptool

确保已安装并激活ESP-IDF环境：
```bash
. $IDF_PATH/export.sh  # Linux/Mac
# 或
$IDF_PATH/export.bat    # Windows
```

### 错误: 构建目录不存在

先构建项目：
```bash
idf.py build
```

### 错误: config.bin不存在

生成config.bin：
```bash
python config.py --build
```

### 警告: 某些bin文件不存在

某些文件（如assets.bin）是可选的，如果不存在会跳过，不影响主要功能。

