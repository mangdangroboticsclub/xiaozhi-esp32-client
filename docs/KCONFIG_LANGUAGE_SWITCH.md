 # Kconfig Language Switch Guide

This document explains how to switch between Chinese and English versions of the Kconfig.projbuild file.

## Overview

The project now supports two language versions of the configuration interface:
- **Chinese version** (`Kconfig.projbuild.zh`) - Default version with Chinese board names
- **English version** (`Kconfig.projbuild.en`) - English translation of all board names

## How to Switch Languages
### Method 1: Using the Switch Script (Recommended)

1. **Switch to English:**
   ```bash
   python scripts/switch_kconfig_language.py en
   ```

2. **Switch to Chinese:**
   ```bash
   python scripts/switch_kconfig_language.py zh
   ```


### Method 2: Manual Switch

1. **Backup current file:**
   ```bash
   cp main/Kconfig.projbuild main/Kconfig.projbuild.zh
   ```

2. **Switch to English:**
   ```bash
   cp main/Kconfig.projbuild.en main/Kconfig.projbuild
   ```

3. **Switch to Chinese:**
   ```bash
   cp main/Kconfig.projbuild.zh main/Kconfig.projbuild
   ```

## Usage Workflow

1. **For English-speaking users:**
   ```bash
   python scripts/switch_kconfig_language.py en
   idf.py menuconfig
   ```

2. **For Chinese-speaking users:**
   ```bash
   python scripts/switch_kconfig_language.py zh
   idf.py menuconfig
   ```

## File Structure

```
main/
├── Kconfig.projbuild          # Current active version (Chinese by default)
├── Kconfig.projbuild.en       # English version
└── Kconfig.projbuild.zh       # Chinese version
```

## Important Notes

- The script automatically creates backups when switching
- Only one version can be active at a time
- The active version is always named `Kconfig.projbuild`
- After switching, run `idf.py menuconfig` to see the changes
- The configuration values themselves remain the same - only the display names change
- if the change in language didn't reflect in the configuration editor called by the shortcut at the bottm of vs code window, close the window and folder then open again.
- change will be saved and repetitive action is not required.

## Board Name Translations

| Chinese Name | English Name |
|--------------|--------------|
| 面包板新版接线（WiFi） | Breadboard New Wiring (WiFi) |
| 面包板新版接线（WiFi）+ LCD | Breadboard New Wiring (WiFi) + LCD |
| 面包板新版接线（ML307 AT） | Breadboard New Wiring (ML307 AT) |
| 虾哥 Mini C3 | Xiaoge Mini C3 |
| ESP32S3_KORVO2_V3开发板 | ESP32S3_KORVO2_V3 Development Board |
| 立创·实战派ESP32-S3开发板 | Lichuang Practical ESP32-S3 Development Board |
| 神奇按钮 Magiclick_2.4 | Magic Button Magiclick_2.4 |
| 无名科技星智0.85(WIFI) | Wuming Technology Xingzhi 0.85(WIFI) |
| 征辰科技1.54(WIFI) | Zhengchen Technology 1.54(WIFI) |
| 敏思科技K08(DUAL) | Minsi Technology K08(DUAL) |
| 四博智联AI陪伴盒子 | Sibozhilian AI Companion Box |
| 元控·青春 | Yuankong Youth |
| 亘具科技1.54(s3) | Genjutech 1.54(s3) |
| 乐鑫ESP S3 LCD EV Board开发板 | Espressif ESP S3 LCD EV Board Development Board |
| 太极小派esp32s3 | Taiji Xiaopai ESP32S3 |
| 嘟嘟开发板CHATX(wifi) | Dudu Development Board CHATX(wifi) |
| 正点原子DNESP32S3开发板 | Zhengdianzi DNESP32S3 Development Board |
| 鱼鹰科技3.13LCD开发板 | Yuying Technology 3.13LCD Development Board |
| Movecall Moji 小智AI衍生版 | Movecall Moji Xiaozhi AI Derivative |
| Movecall CuiCan 璀璨·AI吊坠 | Movecall CuiCan Brilliant AI Pendant |

## Troubleshooting

### Script not found
Make sure you're running the script from the project root directory.

### Permission denied
On Linux/Mac, you may need to make the script executable:
```bash
chmod +x scripts/switch_kconfig_language.py
```

### Backup not found
If you manually deleted the backup file, you can restore the original Chinese version by copying from the repository.

## Development

To add new board types:
1. Add the Chinese name to `Kconfig.projbuild`
2. Add the English translation to `Kconfig.projbuild.en`
3. Update the translation table in this document