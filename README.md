# ESP-3RDEYE 项目

## 项目简介

本项目是“机械觉之瞳”的ESP32版本配套工程，基于ESP-IDF开发。本项目支持多种ESP32系列芯片，包括ESP32-C3，并使用LEDC作为PWM源来控制三路舵机。

## 开发环境

- **开发语言**：C/C++，使用了部分C++特性，如虚函数、`std::thread`等。使用时请注意兼容性。
- **通信协议**：通过UDP协议与APP进行交互，详细内容见【报文格式】。
- **分区表**：使用自定义的`partitions.csv`分区表，分配2MB空间用于存放代码。

## 注意事项

在开始之前，请确保已配置好Wi-Fi、舵机引脚、LED引脚以及UDP端口号。

### 配置步骤

1. 使用`idf.py menuconfig`配置项目。
2. 配置Wi-Fi：在`Example Configuration`菜单中，选择`WiFi SSID`和`WiFi Password`。
3. 配置舵机PWM引脚和LED引脚：同样在`Example Configuration`菜单中，设置`Servo pulse GPIO pitch`、`Servo pulse GPIO roll`、`Servo pulse GPIO yaw`和`Blink GPIO number`。
4. 配置UDP端口号：在`Example UDP Configuration`菜单中，设置`Port`。

### 编译与烧录

配置完成后，您可以编译代码并烧录到ESP32-C3开发板上。

### 报文格式

| 报文类型 | 报文内容 | 说明 |
| --- | --- | --- |
| 发现请求 | SatoriEye_DISCOVERY_REQUEST | 客户端发送的请求，寻找可用的机器人服务器。 |
| 发现响应 | SatoriEye_DISCOVERY_RESPONSE,<电量信息> | 服务器的响应，包含电量信息（如 85 表示 85%）。 |
| 心跳请求 | SatoriEye_HEARTBEAT_REQUEST | 客户端发送的心跳请求，确保连接的有效性。 |
| 心跳响应 | SatoriEye_HEARTBEAT_RESPONSE,<电量信息（可选）> | 服务器的响应，可能包含电量信息，表示连接正常。 |
| 设置模式命令 | SET_MODE:<模式> | 客户端发送的请求，用于设置机器人工作模式（如 Auto）。 |
| 设置模式成功响应 | SET_MODE_SUCCESS:<模式> | 服务器确认模式设置成功（如 SET_MODE_SUCCESS:Auto）。 |
| 断开连接 | SatoriEye_DISCONNECT | 客户端发送的请求，通知服务器断开连接。 |
| 眨眼命令 | WINK | 客户端发送的命令，指示机器人执行眨眼动作。 |
| PWM 控制消息 | CH1:<值>CH2:<值>CH3:<值> | 发送通道 PWM 值，用于控制眼部运动（范围 500-2500 微秒）。 |

**假设要设置通道 PWM 值如下：**

- CH1: 600（水平眼球运动）
- CH2: 2200（垂直眼球运动）
- CH3: 1600（上眼皮运动）

**发送格式为：**

```
CH1:600CH2:2200CH3:1600
```
其中通道的数值代表PWM脉宽，单位为us，范围为500~2500

## 相关资源

- **「开源」机械觉之瞳**：[哔哩哔哩视频](https://www.bilibili.com/video/BV1rN1gYJE3K)
- **配套安卓APP**：由【阿卡林真的是太可爱了】开发，[GitHub项目地址](https://github.com/AkazaAkali)

祝您玩得开心！
