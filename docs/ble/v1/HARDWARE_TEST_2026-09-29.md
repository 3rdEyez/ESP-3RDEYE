# 本机蓝牙实测：2026-09-29

设备：ESP32-C3，BLE 地址 `98:3D:AE:B5:D7:22`，广播名称 `SatoriEye`。
固件：BLE 0.2.2 / 协议 1.2，镜像 SHA-256
`81556796e455d7ff8a64ca063635a6d08960fad0ec8f09145438180ce3614039`。
客户端：satori 的 Intel AX201 蓝牙、Linux BlueZ、Python 3.12.13、Bleak 3.0.2。

测试于 18:58 开始，全部通过：

| 项目 | 实测结果 |
| --- | --- |
| 发现与版本 | 扫描发现设备，GATT DeviceInfo 为 1.2 / 0.2.2 |
| 配对与保护特征 | KeyboardOnly 代理完成一次 Passkey Entry；认证后可订阅 EventTX、读取 StateSnapshot |
| 系统绑定状态 | BlueZ 显示 Paired=yes、Bonded=yes、LegacyPairing=no |
| CLAIM 重试 | 相同帧重发返回完全相同确认及令牌 |
| 心跳 | 每 2 秒发送一次，8 秒后仍保持会话，超过 6 秒租期 |
| GET_STATUS | 收到成功业务确认 |
| 错误令牌 | KEEPALIVE 返回 BAD_SESSION（6） |
| HALT | 成功确认，仍保留当前连接和会话；输出掩码保持 0 |
| RELEASE 与重试 | 成功确认，短窗口内相同 RELEASE 返回同一确认 |
| 绑定重连 | 无新配对码请求；新 CLAIM 获得不同令牌，旧令牌被拒绝 |
| 心跳超时 | CLAIM 后不续租，6.042 秒后主动断开 |
| 超时后恢复 | 再次连接读取：UNCLAIMED、令牌 0、输出有效及插值掩码均为 0，电量未知 |

整个测试没有发送 ARM、SET_TARGET、改码、解绑或配置写入，没有启用舵机输出。
HALT/超时通过仅证明未启用输出时的会话行为，不能替代运动中的停止与断链验证。
未捕获 SMP 空口包，因此没有单独据空口验证 Secure Connections 协商细节。
手机端、MTU 23、运动执行、移动端后台/锁屏、两小时运行仍未验收。

测试结束时已断开连接，临时 BlueZ 配对代理已注销。保留本机绑定，设备占用一个已绑定客户端名额，便于后续免重新配对测试。
测试脚本及逐项结果保存在本机私有目录：
`/home/kyle/.local/share/satori-backups/2026-09-29/ble-smoke/`。
