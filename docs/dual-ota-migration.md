# 双槽迁移构建与启动确认

当前产品默认构建为双槽 OTA。新版显式工程化入口为 `tools/migrate_layout.py`，详见 [工具说明](../tools/README.md)。它默认离线；正常 USB/OTA 更新在未迁移时拒绝，不自动迁移。以下旧阶段模型属于历史设计参考，不能代替当前工具的身份、双备份、签名与阶段校验。

`ble_dual_ota` 是独立的 BLE 构建 profile，版本 0.2.4。它启用 IDF 应用回滚及启动确认，不实现无线 OTA 入口，也不修改 eFuse、安全启动或 Flash 加密策略。

## 布局和首次迁移

4 MB Flash：NVS 0x9000/0x6000、PHY 0xf000/0x1000、ota_0 0x10000/0x170000、ota_1 0x180000/0x170000、config 0x300000/0x2800（保持 subtype 6）、otadata 0x303000/0x2000。

首次迁移保留原应用地址 0x10000；只有确认原应用完整、能装入 ota_0，且 staging/otadata 区为空时才生成候选包。`tools/ota_migration_artifacts.py` 仅离线读取，不提供设备执行接口。它核对指定双读快照、原应用哈希、实际构建分区表的准确条目/MD5、bootloader 公开 Flash 参数及安全构建选项，产物保存在独立目录。

| 次序 | 写入对象 | 中间状态 |
|---|---|---|
| 1 | 新 app 写 ota_1：0x180000 | 旧表继续指向原 factory |
| 2 | otadata：0x303000，两条记录 | seq=1/VALID 指向原 ota_0；seq=2/NEW 指向新 ota_1；旧表忽略它 |
| 3 | 匹配的 rollback bootloader：0 | 完整新 bootloader 能按旧表启动原 factory |
| 4 | 新分区表：0x8000，最后激活 | 新 bootloader 选择 ota_1，NEW→PENDING_VERIFY |

每阶段完整回读校验后才能继续。任何传输、地址、安全状态、占用或哈希不符都停止，不盲重试。写过程中不主动退出 ROM；最后一次正常启动前应比较受保护区域。实际设备操作必须另有受审执行流程。

**不能使用 `idf.py flash`、默认 `flash_args` 或普通初始 otadata**：默认 app 地址为 0x10000，会覆盖原回退槽。默认 reset/stub 也不是本次审查的 USB/no-stub 路径。

应用回滚无法保护首次 bootloader/分区表半写。损坏这两处可能无法应用启动，须经 ROM 按当前完整备份恢复必要区域；不全片擦除、不覆盖 NVS/PHY/config。正常自动进出 ROM 已验证不等于损坏 bootloader 后的恢复已现场验证。

## 启动确认

确认前关闭 BLE 控制门禁，仅接受正在运行 OTA 槽的 VALID/PENDING_VERIFY 状态，核对精确布局。共享 NVS 初始化不自动擦除；缺少持久身份时不生成新身份。检查配置/身份有效、BLE 广播成功、控制任务周期推进、至少 32 KiB 8-bit 空闲堆；连续五个采样通过才确认槽并开放 BLE 控制。最长 15 秒，失败保持停止，待确认镜像调用官方 rollback API。

检查不初始化/驱动舵机，不要求手机连接或 ARM，不新增网络服务。已有人工 USB 维护命令仍存在，本门禁不声明阻止所有人为 USB 管理操作。

原应用明确标记 VALID，不要求旧应用实现确认 API。若新应用未确认即复位，IDF 会将其标为 ABORTED 并回退原 ota_0。真实设备自检、复位回滚和断电恢复仍须独立验收。

## 离线验证

- 八个 host 脚本，包括实际启动确认源码的 SDK mock 测试。
- Python 构建 profile 隔离与迁移测试：非法布局/MD5、初始 otadata、损坏记录、分阶段写入的区域保护。
- 从本机固定 IDF 5.5.4 源码提取官方选择器编译运行，验证自定义记录 CRC/选择及 ABORTED/INVALID 回退。
- 固件构建及官方镜像/分区尺寸校验。

故障注入仅验证模型和地址保护，不代表真实掉电已测试。签名空间只是对齐后的未来容量预留，不代表实现了验签或安全无线更新。
