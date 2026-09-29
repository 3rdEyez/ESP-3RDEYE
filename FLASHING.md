# 最新烧录结果：BLE 0.2.2（2026-09-29 18:44）

已在本机 satori 完成新版 BLE 应用烧录。用户换线并改接 USB 路径 `3-1` 后，使用 Python 3.12.13、esptool 4.8.1、115200、`--no-stub --before no_reset --after no_reset` 完成连续读写。18:35:21 至验证结束未记录新的 USB 断开或 `-71`。本次同时改变了主机、线/端口和工具版本，只能确认该组合可用，不能单独归因于线材或 esptool。ModemManager 未停用。

BLE 固件用 ESP-IDF 5.5.4 重新构建，现有配置/机械映射/启动目标/配对策略/BLE 协议与会话主机测试通过。镜像 `build/ble_primary/app.bin` 为 777440 字节，SHA-256：
`81556796e455d7ff8a64ca063635a6d08960fad0ec8f09145438180ce3614039`。
源码基线 `e8a493a`，构建时的已跟踪改动仅为 USB 诊断脚本。

只写入应用地址 `0x10000`，保留设备原启动加载器与分区表。写后 esptool 哈希校验通过；独立回读整片 4 MiB 后，应用逐字节匹配，应用擦除区域之外的数据全部与烧前一致（包括 NVS 和舵机/网络配置）。启动后 BLE 按设计初始化自己的 NVS 数据，因此“保持一致”指首次启动前的回读结果。

启动日志确认旧 ESP-IDF 5.3.1 bootloader 正常启动新 ESP-IDF 5.5.4 应用，BLE MAC `98:3D:AE:B5:D7:22`。本机扫描发现 `SatoriEye`，只读 GATT DeviceInfo 验证协议 1.2 / 固件 0.2.2。没有配对、ARM 或运动命令；手机配对、实际动作、后台与锁屏仍待验证。BLE 配置不启动 Wi-Fi。

## 私有回退备份

位于 `/home/kyle/.local/share/satori-backups/2026-09-29/`，目录受限，二进制不提交仓库：

- `full-read/full-flash-backup.bin`：烧前完整 4194304 字节，SHA-256 `2cb2622288e2dcc2ff0ca0c7278d954e47f268f2190a21aaee0944664cdf3d54`。
- `factory-before-flash.bin`：独立读取的 2097152 字节，与整片备份中的应用分区完全一致。
- `nvs-before-flash.bin`、`config-before-flash.bin`：从已核对整片备份提取。
- `ble-primary-to-flash.bin`：本次实际写入的固定镜像。
- `ble-flash-verified/verification.json`、`ble-check.json`、`boot-serial.log`、`timeline.txt`：回读、BLE、启动与时间线证据。

备份含凭据，不要公开上传。向 Ciallo 复制备份时 SSH 超时，因此本次完整备份目前只保存在本机。

若需回退应用，使用现已验证的线和端口、同一 esptool 4.8.1 环境，以 `--no-stub` 进入下载模式，只将 `factory-before-flash.bin` 写回 `0x10000`，独立回读 2 MiB 比较后复位。`--before no_reset` 仅适用于已在下载模式的设备；正在运行应用时需先正常进入下载模式。不要整片擦除，也不要覆盖 config 或 NVS 来代替应用回退。

本机 `/run/udev/rules.d/99-satori-serial-access.rules` 临时为该 VID/PID/序列号的 tty 节点授予 kyle 访问权限，重启后自动失效，不匹配其他设备。

---
以下为此前 Wi-Fi/UDP 版本准备及故障历史（已被上述实际烧录结果更新）：

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
