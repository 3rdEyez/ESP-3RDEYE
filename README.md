# ESP-3RDEYE 项目

本项目是“机械觉之瞳”的 ESP32 固件，使用 ESP-IDF 控制三路舵机。固件提供 BLE 与旧版 UDP 两种互斥通信配置；新设备和日常使用推荐 **BLE（`ble_primary`）**。`legacy_udp` 仅用于需要兼容旧版 APP 或受控网络环境的场景。

## 构建

使用 **ESP-IDF 5.5.4**，先打开并激活 ESP-IDF 终端，再运行：

```sh
python tools/build_firmware.py
```

该命令默认只构建 BLE 固件，不会烧录设备。旧版 UDP 使用 `python tools/build_firmware.py legacy_udp`。脚本在 Windows、Linux 和 macOS 上调用已激活的 ESP-IDF，并分别保存两种配置的构建目录与 `sdkconfig`。Windows 请在 ESP-IDF PowerShell 或命令提示符中运行；ESP-IDF 5.5 的项目、工具链和 Python 安装路径不能包含空格或括号。

## 配对与使用

BLE 首次配对、连接和固件设置请参阅[固件设置与配对指南](docs/ble/v1/FIRMWARE_SETUP.md)。BLE 帧格式和连接流程见 [SatoriEye BLE Control v1.2 协议](docs/ble/v1/SatoriEye_BLE_Protocol_v1.md)。固件构建、备份、烧录和回滚说明见[烧录指南](FLASHING.md)。

## 旧版 UDP 报文（legacy）

下表只描述 `legacy_udp` 配置使用的旧版文本报文，不适用于推荐的 BLE 配置。只有构建并启用 `legacy_udp` 时，才按对应旧版客户端约定使用这些报文。

| 报文类型 | 报文内容 | 说明 |
| --- | --- | --- |
| 发现请求（legacy） | `SatoriEye_DISCOVERY_REQUEST` | 客户端搜索旧版 UDP 服务。 |
| 发现响应（legacy） | `SatoriEye_DISCOVERY_RESPONSE,<电量信息>` | 服务端响应，可能包含电量信息。 |
| 心跳请求（legacy） | `SatoriEye_HEARTBEAT_REQUEST` | 旧版客户端心跳。 |
| 心跳响应（legacy） | `SatoriEye_HEARTBEAT_RESPONSE,<电量信息（可选）>` | 旧版服务端心跳响应。 |
| 设置模式（legacy） | `SET_MODE:<模式>` | 旧版模式设置命令。 |
| 设置模式成功（legacy） | `SET_MODE_SUCCESS:<模式>` | 旧版模式设置确认。 |
| 断开连接（legacy） | `SatoriEye_DISCONNECT` | 旧版断开通知。 |
| 眨眼命令（legacy） | `WINK` | 旧版眨眼命令。 |
| PWM 控制（legacy） | `CH1:<值>CH2:<值>CH3:<值>` | 旧版三通道控制报文，逻辑值范围为 500–2500。 |

## 相关资源

- [「开源」机械觉之瞳视频](https://www.bilibili.com/video/BV1rN1gYJE3K)
- [配套安卓 APP 项目](https://github.com/AkazaAkali)
