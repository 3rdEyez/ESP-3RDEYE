# 觉瞳本地开发工具

统一入口：`python3 tools/satori_dev.py --help`。从任意目录调用均可。
本工具不安装 SDK、不生成密钥、不扫描或配对设备、不推送代码。
`check`、`inspect`、`rollback-plan` 全部离线；`build` 只编译。
`package` 默认只验签，`sign-package` 默认只检查计划。
`maintenance`、`status`、`usb-upgrade` 默认不建立设备连接。

## 日常离线开发

先激活已有 ESP-IDF **5.5.4**，设置 `IDF_PATH` / `IDF_PYTHON_ENV_PATH`。
完整检查需要 Linux/POSIX、C++ 编译器和本机 loopback socket 权限；没有 IDF 时拒绝运行完整检查，避免静默跳过。

```sh
python3 tools/satori_dev.py check
python3 tools/satori_dev.py check --app-path ../SatoriManager/flutter_app --flutter flutter
python3 tools/satori_dev.py build ble_primary
python3 tools/satori_dev.py inspect build/ble_primary/app.bin
python3 tools/satori_dev.py rollback-plan --current-slot 1 --current-sequence 4
```

编译配置可选 `ble_primary`、`legacy_udp`、`ble_dual_ota`、`ble_wifi_ota_prototype`。
完整检查包含 Python、九组原生 C++ 回归；提供 App 时再运行 App 工具回归、Flutter test/analyze、BLE contract。
检查不会调用设备 transport、私钥、安装或编译发布包。

## 签名与包

先独立确认既有公钥的 SHA-256 信任指纹；不从候选包自取信任根。
以下命令默认不写包。显式加 `--execute` 才写新 `.sota`；输出禁止覆盖。

```sh
python3 tools/satori_dev.py package --image SIGNED_IMAGE --public-key PUBLIC_KEY --trust-sha256 TRUST_DIGEST --output NEW_PACKAGE
python3 tools/satori_dev.py sign-package --image UNSIGNED_IMAGE --public-key PUBLIC_KEY --trust-sha256 TRUST_DIGEST --output NEW_PACKAGE
```

签名执行另需 `SATORI_SIGNING_KEY_FILE` 指向用户已有私钥文件。该环境变量仅放文件路径，绝不能放密钥内容；不要启用 shell tracing。
SDK 从文件读取私钥，脚本不读取/输出密钥正文或子进程输出。
签名后再次调用官方 `espsecure verify_signature`，验证成功才编码包。
`inspect` 仅解析元数据，不能证明签名有效。
同版本、相同包重复提交、降级和升级均允许；签名、SHA、板型、芯片、项目及 security_version 检查仍保留。

## 维护与单次无线安装（实机验收尚未完成）

Linux BlueZ + 已有绑定；不新建配对。私有 JSON 保存在忽略的 `.local/device-config.json`：

```json
{"device_address":"USER_DEVICE_ADDRESS","adapter":"hci0","ssid":"USER_2_4_GHZ_SSID"}
```

JSON 禁止密码/令牌/私钥字段。默认只显示计划：

```sh
python3 tools/satori_dev.py status --config .local/device-config.json
python3 tools/satori_dev.py maintenance --config .local/device-config.json
```

显式 `--execute` 才连接。维护要求私有交互 TTY，密码隐藏输入且不得放 argv、环境变量或日志；用户亲自输入 `CONNECT`。
默认临时网络；明确 `--remember-network` 才保存，`--saved-network` 复用已保存网络且不询问密码。
保存/复用要求设备 0009 能力；版本兼容线为固件/App 0.2.x、协议 1.2。
可加 `--package PACKAGE --public-key PUBLIC_KEY --trust-sha256 TRUST_DIGEST`：配网前验包，GET 检查页面并关闭连接，然后用户亲自输入 `INSTALL`。
窗口/授权再核对，剩余时间至少45秒，只提交一次 POST；未知 POST 结果不重试、不发 CLOSE，须另行确认实际版本/槽状态。
未 POST 的取消只关闭本次窗口，不关闭他人的窗口。锁屏自动动作等控制需求不由此工具处理。

## 正常 USB 升级（显式复位/写入，实机验收尚未完成）

仅支持现有 **4MB ESP32-C3 双 OTA 已迁移布局**，不支持分区迁移、空白设备或熔丝安全状态改变。
使用已有独立 ROM 环境：`esptool==4.8.1`、pySerial；不能假定 IDF Python 的 esptool 版本相同。不安装依赖。
公钥验签所用 IDF SDK 仍由环境配置。
私有 JSON 的路径相对于配置文件，所有 SHA 是用户独立核验的真实产物指纹：

```json
{
  "candidate":{"path":"SIGNED_CANDIDATE","sha256":"CANDIDATE_DIGEST"},
  "current_app":{"path":"SIGNED_CURRENT_APP","sha256":"CURRENT_DIGEST"},
  "bootloader":{"path":"REVIEWED_BOOTLOADER","sha256":"BOOT_DIGEST"},
  "partition_table":{"path":"REVIEWED_PARTITION_TABLE","sha256":"TABLE_DIGEST"},
  "public_key":"PUBLIC_KEY", "trust_sha256":"TRUST_DIGEST",
  "current_slot":1, "port":"USER_SERIAL_PORT",
  "usb_identity":{"vid":"USER_VID","pid":"USER_PID","serial":"USER_USB_SERIAL"},
  "flash_id":"USER_FLASH_ID_HEX"
}
```

```sh
python3 tools/satori_dev.py usb-upgrade --config .local/usb-config.json
# 实际执行须用户另行明确授权后加 --execute --backup-dir NEW_PRIVATE_DIRECTORY
```

执行核 USB 身份/占用/ROM/闪存，读取两份完整4MB备份并 fsync 保存，核真实分区、旧 VALID 记录及旧签名 app。
备份必须是仓库外的新目录（0700，文件0600），包含凭据/绑定/标定，应私有保存，不发日志或 Git。
非活动 app 写入完整回读后，只擦写备用选择记录的一个4096扇区；旧 VALID 扇区始终保留。
全 Flash 比对、第一次 PENDING 确认、实际 VALID 记录回读、第二次 VALID 启动均须通过。
失败/一次 Ctrl+C 进入限时恢复；只有保护区和旧 VALID 记录原样才恢复备用记录，禁止盲目恢复。
恢复失败需要人工调查，不重试、不 erase、不重写 bootloader/table/NVS/config。
这是正常升级流程；故障注入、旧迁移实验和历史私有备份不接入日常入口。
旧 `backup_device.sh` 固定偏移入口已停用，不再打开串口。

## App 开发交付包

配套 App 的 `flutter_app/tool/package_ble_delivery.py` 默认计划，显式 `--execute` 才打 ZIP。
需指定 `--firmware-root`、`--output`；可选 `--apk`、`--profile`、`--aapt`。
只收公开白名单，拒绝输入或父目录符号链接；一次冻结字节快照用于 APK 版本验证、清单摘要和归档。
要求 APK 实际 versionName/versionCode 与 pubspec 相符。不会声称 Android 签名、固件签名或硬件验收已验证。
此 ZIP 不是自动刷写/OTA 包。不收 sdkconfig、NVS、私钥、备份、设备信息或原始日志。
