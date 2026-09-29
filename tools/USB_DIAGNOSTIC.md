# Ciallo USB 断线取证

目标是把 Ciallo 的 USB 内核事件、esptool 原始输出和手动操作标记放到同一条时间线，确定失联发生在哪个阶段。脚本不写入 flash；各探测选项会通过 esptool 切换芯片模式，因此设备不在现场、且 `/dev/ttyACM0` 未出现时，不要运行探测。仅采集模式不会碰串口设备。

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

另有 `--rom-probe`（不上传 stub，读取 factory 起始 4096 字节）和 `--rom-backup`（不上传 stub，读取完整 2 MiB factory 分区）。这两个选项只用于排查/备份，读取到的 factory 内容也应按私有数据保护。`--rom-backup` 假定芯片已处于下载模式，使用 `--before no-reset`；如果前一个命令没有留下下载模式，就不能直接运行它。

2026-09-29 实测：设备完全断电重开且换线后，仍在 esptool 上传 stub 时发生 I/O 错误和 USB `-71`；`--rom-probe` 在 18:24:08 成功读出 4096 字节，无新 USB 错误；18:25:04 的 `--rom-backup` 在实际读取前再次发生 I/O 错误，随后 `-71`。只有 4 KiB 短读，与旧 `full_flash.bin` 同地址开头一致；完整的当前 factory 备份仍未取得。测试期间舵机始终连接并供电；目前无法安全断开舵机，因此负载影响仍未排除。不要以这次短读作为完整备份或烧录安全的证据。原始时间线保存在本地 `USB_ROM_PROBE_2026-09-29_1824.timeline.txt` 与 `USB_ROM_BACKUP_2026-09-29_1825.timeline.txt`。

每次采集输出私有目录，含 `timeline.txt`（便于人工看先后顺序）、`events.jsonl`（该时段完整内核消息及标记的墙上时钟和单调时钟时间戳）以及探测成功时的 `config-sector-readback.bin`。配置扇区可能含 Wi-Fi 凭据，不要公开上传整个目录；分享时间线前也先检查是否包含敏感信息。手动标记同时记录操作端时间 `operator_time` 和 journald 收到消息的时间，可以看到标记传递延迟。esptool 每行的时间是采集进程收到该行的时间，`phase` 行记录命令开始与退出时间；它不能还原一行内部每个字节发生的精确时间。

判读时先比较内核首次 `disconnect`/`-71`、esptool 启动、`Connecting`/stub/读取开始、错误和手动标记的顺序。若内核先报错，esptool 的串口 I/O 错误是后果；若 USB 在空闲观察时稳定、切换模式后开始报错，则继续区分模式切换和读传输阶段。单次相关性不能单独证明原因。
