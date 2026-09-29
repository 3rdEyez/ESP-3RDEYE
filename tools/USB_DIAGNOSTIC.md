# Ciallo USB 断线取证

目标是把 Ciallo 的 USB 内核事件、esptool 原始输出和手动操作标记放到同一条时间线，确定失联发生在哪个阶段。脚本不写入 flash；`--probe` 会通过 esptool 切换芯片模式，因此目前设备不在现场、且 `/dev/ttyACM0` 未出现时，不要运行探测。仅采集模式不会碰串口设备。

先在 Ciallo 的一个终端开始采集（每次使用新的目录）：

```bash
python3 ~/下载/设置Wifi/usb_diagnostic_capture.py capture \
  --output ~/下载/设置Wifi/usb-diag-$(date +%Y%m%d-%H%M%S) \
  --idle-seconds 120 --after-seconds 0
```

若有人在设备旁进行拔线、插线、按键或断电，在另一个终端**操作当时**分别记录：

```bash
python3 ~/下载/设置Wifi/usb_diagnostic_capture.py mark 'USB 拔出'
python3 ~/下载/设置Wifi/usb_diagnostic_capture.py mark 'USB 插入'
python3 ~/下载/设置Wifi/usb_diagnostic_capture.py mark 'RESET 按下'
```

当设备重新稳定枚举、且有人能够在现场恢复设备时，可以做一次短的只读探测：

```bash
python3 ~/下载/设置Wifi/usb_diagnostic_capture.py capture \
  --output ~/下载/设置Wifi/usb-probe-$(date +%Y%m%d-%H%M%S) \
  --idle-seconds 30 --after-seconds 30 --probe
```

探测依次执行 `flash-id` 和读取配置分区起始 4096 字节。`--probe` 若看不到 `/dev/ttyACM0` 会跳过，不会尝试其他端口。esptool 的连接过程可能复位/切换 ESP32-C3 下载模式；出现持续 USB `-71` 时停止，不继续刷写或擦除。

每次采集输出私有目录，含 `timeline.txt`（便于人工看先后顺序）、`events.jsonl`（该时段完整内核消息及标记的墙上时钟和单调时钟时间戳）以及探测成功时的 `config-sector-readback.bin`。配置扇区可能含 Wi-Fi 凭据，不要公开上传整个目录；分享时间线前也先检查是否包含敏感信息。手动标记同时记录操作端时间 `operator_time` 和 journald 收到消息的时间，可以看到标记传递延迟。esptool 每行的时间是采集进程收到该行的时间，`phase` 行记录命令开始与退出时间；它不能还原一行内部每个字节发生的精确时间。

判读时先比较内核首次 `disconnect`/`-71`、esptool 启动、`Connecting`/stub/读取开始、错误和手动标记的顺序。若内核先报错，esptool 的串口 I/O 错误是后果；若 USB 在空闲观察时稳定、切换模式后开始报错，则继续区分模式切换和读传输阶段。单次相关性不能单独证明原因。
