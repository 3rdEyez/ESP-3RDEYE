# ESP32-C3 应用固件烧录记录

2026-09-29 使用 ESP-IDF v5.5.4 构建，目标 ESP32-C3、4 MB 闪存。
构建命令：激活 `/home/kyle/esp/esp-idf/export.sh` 后在工程根目录运行 `idf.py build`。
产物为 `build/app.bin`（1,059,696 字节）。新生成的分区表与 Ciallo 上
`~/下载/设置Wifi/full_flash.bin` 中的分区表逐字节一致：`factory` 位于
`0x10000`，大小 `0x200000`；`config` 位于 `0x300000`，大小 `0x2800`。
镜像已复制到 Ciallo 的 `~/下载/设置Wifi/app-heartbeat-20260929.bin`，两端
SHA-256 均为 `f3c0b50b03a0a9c82781f022bc61bc7358eec897685e905447c43aa5a8e5e376`。

本次仅需烧录 `factory` 应用分区，**不**覆盖启动加载器、分区表、NVS 或
`config`。设备当前 Wi-Fi 和舵机设置保存在 `config`；其完整备份为 Ciallo 上的
`~/下载/设置Wifi/backup-config-before-stage-a-20260929.bin`。

烧录前必须从设备完整读出 `0x10000` 起的 `0x200000` 字节，保存并核对大小。
目前原生 USB Serial/JTAG 在读取时反复报 `-71`，仅成功读出首块 64 KiB，
**尚无完整的现运行应用镜像备份，因此尚未烧录新应用**。Ciallo 的旧
`full_flash.bin` 与现场首块逐字节一致，但剩余部分尚未核实，不能代替现场备份。
远程禁用并启用 Ciallo 的 USB 1-3 端口后，设备仍无法枚举。随后在用户授权下
重启 Ciallo；2026-09-29 10:08 开机时 USB 1-3 仍连续报告描述符错误 `-71`，
没有生成 `/dev/ttyACM0`，而设备 Wi-Fi 仍在线。设备由自身电池供电，主机
USB 重置及主机重启并未让主控断电。现有分区表只有 `factory` 应用分区，
运行固件没有已知的 OTA/网络升级入口，不能通过现有 Wi-Fi 会话替代串口烧录。

当 USB 下载模式稳定且完整备份完成后，先将 `build/app.bin` 复制到 Ciallo，
核对 SHA-256，再用 esptool 仅写入 `0x10000`，随后回读已写入长度进行逐字节
比较，并重启设备。若新应用无法启动，在下载模式把现场备份的应用分区写回
`0x10000`，回读核验后重启。当前没有 OTA 回退路径。

设备上现运行的固件仍将心跳请求原样回显。新镜像将返回独立的
`SatoriEye_HEARTBEAT_RESPONSE`；电量测量尚未实现，发现与心跳响应不附带
虚构的百分比。烧录成功后需要重新抓包确认。
