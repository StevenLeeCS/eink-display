# ZJY420S08W0G01 + XIAO ESP32-C3

这个工程让 Seeed Studio XIAO ESP32-C3 驱动中景园 ZJY420S08W0G01
4.2 英寸黑白墨水屏。驱动 IC 是 SSD1683，分辨率为 400x300。

## 接线

请以屏幕 PCB 上的丝印为准。模块必须使用 **3.3V**，不能接 5V。

| 屏幕引脚 | XIAO 引脚 | ESP32-C3 GPIO | 说明 |
| --- | --- | ---: | --- |
| VCC | 3V3 | - | 3.3V 电源 |
| GND | GND | - | 两块板必须共地 |
| DIN / SDA | D10 / MOSI | 10 | SPI 数据 |
| CLK / SCL | D8 / SCK | 8 | SPI 时钟 |
| CS | D1 | 3 | 片选，低有效 |
| DC | D2 | 4 | 命令/数据选择 |
| RST / RES | D3 | 5 | 复位，低有效 |
| BUSY | D4 | 6 | 屏忙信号，高表示忙 |

MISO 不需要连接。接线应尽量短，首次测试建议使用 USB 给 XIAO 供电。

## 第一次点亮

1. 安装 VS Code、PlatformIO 插件，并用 VS Code 打开本目录。
2. 按上表接好全部 8 根线，尤其检查 VCC 是 `3V3` 而不是 `5V`。
3. 在 PlatformIO 中执行 `Upload`，然后打开 115200 波特率的串口监视器。
4. 未转换图片时，屏幕会显示边框和棋盘测试图。刷新约需 3 秒，期间闪烁是正常现象。

也可以在终端中执行：

```powershell
pio run -t upload
pio device monitor -b 115200
```

## 显示自己的图片

先安装图片转换依赖：

```powershell
python -m pip install -r requirements.txt
```

把图片转换为 400x300、1 bit 的固件数组：

```powershell
python tools/image_to_header.py path\to\photo.jpg
```

脚本会覆盖 `include/image_data.h`，同时生成
`include/image_data.preview.png` 供检查。默认完整保留图片并添加白边，使用
Floyd-Steinberg 抖动表现灰度。常用选项：

```powershell
# 裁切铺满屏幕
python tools/image_to_header.py photo.jpg --fit cover

# 不抖动，按固定阈值转黑白
python tools/image_to_header.py logo.png --threshold 150

# 顺时针旋转 90 度
python tools/image_to_header.py photo.jpg --rotate 90
```

转换后重新执行 `Upload`。固件在刷新完成后会让屏幕进入深度睡眠；墨水屏断电后
仍会保留图像。要换图需重新复位或上电。

## 故障排查

- 串口显示 `BUSY timeout`：先检查 `BUSY`、`RST`、3.3V 和 GND，确认没有把
  VCC 接到 5V。
- 屏幕完全不刷新：检查 `DIN` 是否接 D10、`CLK` 是否接 D8，以及 CS/DC 是否
  接反。
- 图像黑白颠倒：转换时加 `--invert`。
- 图像方向不对：转换时用 `--rotate 90`、`180` 或 `270`。
- 刷新时屏幕闪黑/闪白：这是全刷波形的正常过程，不要在 BUSY 为高时断电。
- 不要快速循环全刷。电子纸适合低频更新，当前示例只在启动时刷新一次。

初始化和刷新命令按 `docs` 中厂家 STM32 示例移植：软复位 `0x12`、黑白 RAM
`0x24`、第二 RAM `0x26`、显示控制 `0x22=0xF7`、启动刷新 `0x20`。
