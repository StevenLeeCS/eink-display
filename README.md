# 语音任务墨水屏

基于 Seeed Studio XIAO ESP32-C3、INMP441 麦克风和中景园
ZJY420S08W0G01 4.2 英寸黑白墨水屏。设备按住按键录音，将音频流上传到电脑，
电脑使用 faster-whisper 识别中文，随后把结果返回设备并显示为任务。

## 当前功能

- 墨水屏按顺时针旋转后的 `300×400` 竖屏坐标显示。
- 屏幕分为上下两个 `300×200` 区域，A1 控制上区，A2 控制下区。
- 短按切换已有任务的未完成/已完成状态，分别显示空心/实心方块。
- 按住至少 500ms 开始语音输入，松开后提交识别，最长 60 秒。
- 保留按下后的 500ms 预录音频，避免丢失开头语音。
- 静音按住 3 秒会立即重置对应区域，不需要等待松开。
- 无语音和操作失败使用无方块的中文提示，技术错误仅写入串口。
- 支持 GB2312 一级汉字以及可打印 ASCII 字符的 16 点阵显示。
- 使用 SSD1683 `0x24/0x26` 双 RAM 黑白差分刷新，连续 4 次差分刷新后执行
  1 次全刷。
- 服务端识别完成后自动删除临时 WAV 文件。

当前使用厂商 `0xFF` 黑白差分刷新波形和完整 `400×300` RAM 窗口。A1/A2
只修改软件 framebuffer 中对应的半区，但每次仍传输完整新旧帧，因此这是视觉上的
局部更新，不是硬件子窗口局刷。该方案已通过实机测试，可避免非零 X 子窗口引起的
跨区更新、半屏反黑、旧内容迁移和文字叠加。

## 硬件接线

所有模块使用 `3.3V` 并共地，不要连接 `5V`。

### 墨水屏

| ZJY420S08W0G01 | XIAO 引脚 | GPIO | 说明 |
| --- | --- | ---: | --- |
| VCC | 3V3 | - | 3.3V 电源 |
| GND | GND | - | 公共地 |
| DIN / SDA | D10 | 10 | SPI MOSI |
| CLK / SCL | D8 | 8 | SPI SCK |
| CS | D1 | 3 | SPI 片选 |
| DC | D2 | 4 | 命令/数据选择 |
| RST / RES | D3 | 5 | 复位 |
| BUSY | D4 | 6 | 高电平表示忙 |

墨水屏的 MISO 不需要连接。面板按顺时针旋转 90° 使用，A1 位于上方，A2 位于
下方。

### INMP441

| INMP441 | XIAO 引脚 | GPIO | 说明 |
| --- | --- | ---: | --- |
| VDD | 3V3 | - | 3.3V 电源 |
| GND | GND | - | 公共地 |
| SCK / BCLK | D5 | 7 | I2S 位时钟 |
| WS / LRCL | D6 | 21 | I2S 左右声道时钟 |
| SD | D7 | 20 | I2S 数据输入 |
| L/R | GND | - | 选择左声道 |

### 按键

| 按键 | XIAO 引脚 | GPIO | 控制区域 | 接法 |
| --- | --- | ---: | --- | --- |
| A1 | D0 | 2 | 上半区 | 按键连接 D0 与 GND |
| A2 | D9 | 9 | 下半区 | 按键连接 D9 与 GND |

按键使用 ESP32-C3 内部上拉，按下时为低电平，不需要外接上拉电阻。

## 软件准备

需要安装 VS Code、PlatformIO 扩展和 Python 3。首次使用时创建 Python 虚拟环境并
安装识别服务依赖：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

复制网络配置示例：

```powershell
Copy-Item include\network_config.example.h include\network_config.h
```

编辑 `include/network_config.h`：

- `kWifiSsid` 和 `kWifiPassword`：ESP32 使用的 2.4GHz Wi-Fi。
- `kServerHost`：运行识别服务的电脑局域网 IPv4 地址，不能使用 `localhost`。
- `kServerPort`：默认 `18000`。

真实配置文件已被 Git 忽略，不会上传 Wi-Fi 密码。

## 运行流程

1. 启动电脑端接收和识别服务：

```powershell
.\.venv\Scripts\python.exe tools\audio_receiver.py
```

2. 编译并烧录设备：

```powershell
pio run
pio run -t upload
```

3. 根据需要打开串口监视器：

```powershell
pio device monitor -b 115200
```

4. 按住 A1 或 A2 并说话，松开后等待识别和屏幕刷新。短按已有任务对应的按键可以
   切换完成状态。

服务端通过 HTTP chunked encoding 边接收边写入临时 WAV。识别完成、识别失败或
设备判定本段为静音时都会删除临时文件，因此 `recordings` 目录通常为空。

## 字库与显示

- `assets/gb2312_level1_16.bin`：GB2312 一级汉字，3755 个字符。
- `assets/gb2312_level1_unicode_map.bin`：Unicode 到字库位置的映射。
- `assets/asc16_printable.bin`：英文、数字和可打印标点。
- 不在字库中的字符显示为 `?`。

字库二进制通过 `platformio.ini` 嵌入固件。它们是文字显示所需资源，不属于已删除的
通用图片显示功能。

## 验证

```powershell
.\.venv\Scripts\python.exe -m py_compile tools\audio_receiver.py tools\test_audio_receiver.py
.\.venv\Scripts\python.exe tools\test_audio_receiver.py
pio run
git diff --check
```

## 故障排查

- 串口没有输出：关闭并重新打开监视器，重新插拔 USB 后按一次 XIAO Reset。
- 上传时串口被占用：先用 `Ctrl+C` 关闭监视器，再执行上传。
- `BUSY timeout`：检查墨水屏的 3.3V、GND、BUSY 和 RST。
- Wi-Fi 超时：检查网络配置、2.4GHz Wi-Fi 频段和信号。
- 无法连接服务端：确认电脑与设备位于同一局域网、防火墙允许配置端口，并检查电脑
  当前 IPv4 地址。
- 服务端端口无权限或被占用：使用 `--port` 更换端口，并同步修改设备配置。
- 旧文字叠加或刷新异常：确认 `0x24` 保存新帧，刷新后将完整新帧同步到 `0x26`，
  并使用完整 `400×300` RAM 窗口。
- 静音长按没有自动重置：检查串口中的录音秒数，并根据现场底噪调整 VAD 阈值。

硬件规格书和厂商示例位于 `docs`，当前架构说明见 `AGENTS.md`，每日开发记录见
`DEVELOPMENT_LOG.md`。
