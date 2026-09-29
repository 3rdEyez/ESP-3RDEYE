# SatoriEye BLE Control v1.2：固件与 App 共用协议

日期：2026-09-29；状态：A0/A1 软件实现契约；硬件验收另行记录。
配套：`SatoriEye_BLE_Firmware_App_Plan_v3.md`、`satori_ble_v1_golden_vectors.json`。
本协议的时间与容量是工程初值。阶段 A 需冻结两端使用的相同修订与测试向量；改字段必须两端同步修改，不能由任一仓库悄悄解释成另一种含义。

## 1. 范围和安全前提

仅覆盖 BLE 直接控制：读取设备能力、取得控制会话、显式启动输出、三通道目标、短程插值、暂停、心跳、释放会话。自动转动、眨眼、预设名称和关键帧时序由 App 编排。不包含 Wi-Fi 密码、SoftAP、OTA、硬件标定写入、恢复出厂、未认证的所有权转移、人脸识别或真实转轴遥测。

所有控制、事件订阅和状态读操作必须先满足标准 BLE LE Secure Connections Passkey Entry 配对、加密、认证及设备保存的手机 bond。具体 SMP 配置及 Android 行为必须通过实机确认。禁止回退到 Just Works、Legacy 配对或调试密钥。会话令牌和设备 ID **不是密码**，不代替这些安全前提。

v1.2 全新且健康的空设备自动生成并持久化随机身份，初始六位码为 **123456**。已有身份、配对码和 bond 升级保留；损坏身份/NVS 不得触发默认开放或自动擦除。最多保存8台手机的SC bond，同一时间仅允许一条连接。空闲时，新手机可用当前配对码注册，无需原手机授权窗口；已有手机使用保存的bond恢复加密。任一当前已认证绑定手机可修改配对码，改码不撤销任何已有bond。知道当前码即可在空闲且未满时加入，因此默认码建议初次使用后修改；不存在通过公开设备ID或token绕过SMP认证的入口。满额或手机丢失后的清理通过USB维护完成。

## 2. GATT 表

以下 UUID 是本项目拟定的私有 128-bit UUID，不是 Bluetooth SIG 分配的标准服务。阶段 A 冻结后固定使用。

| 对象 | UUID | 属性 | 长度 | 访问限制 |
|---|---|---|---:|---|
| Service | `4d89f6a0-73b9-4f14-9d3e-63b2145a0000` | Primary Service | — | 发现入口 |
| DeviceIdentity | `4d89f6a0-73b9-4f14-9d3e-63b2145a0001` | Read | 16 B | 可公开读取，不能据此鉴权 |
| DeviceInfo | `4d89f6a0-73b9-4f14-9d3e-63b2145a0002` | Read | 20 B | 可公开读取，不含秘密 |
| ControlRX | `4d89f6a0-73b9-4f14-9d3e-63b2145a0003` | Write With Response | 20 B | 安全连接；再做会话验证 |
| EventTX | `4d89f6a0-73b9-4f14-9d3e-63b2145a0004` | Notify | 20 B | 安全连接及受保护 CCCD |
| StateSnapshot | `4d89f6a0-73b9-4f14-9d3e-63b2145a0005` | Read | 20 B | 安全连接；未 CLAIM 也可读 |

不启用控制 Write Without Response，不依赖长写或应用分片。客户端读取能力、完成安全连接并订阅事件后，再发送 CLAIM。20 B 特征值在 ATT MTU 23 的基本写/通知容量内；大 MTU 协商失败不应阻塞首版控制。特征 UUID 文本按通常形式记录；固件库的字节数组顺序必须按该库要求转换，不能把整段 UUID 简单当作小端整数。

DeviceIdentity 是初始化时生成并持久化的 16 B 不透明 ID。不是广播 MAC，不随 BLE 地址变化；保留前导零。首次添加由用户选择设备并完成配对后收藏 ID；以后重新建立加密连接后再读 ID 验证匹配，不能仅凭可伪造的公开 ID 把陌生设备当成已授权设备。

首版服务表保持固定。未来更改 GATT 布局须处理手机服务缓存及 Service Changed，或者明确使用新主版本服务 UUID；不要求用户盲目反复重装 App。

## 3. DeviceInfo：20 字节

所有多字节无符号整数采用 little-endian。未定义的能力位和保留字节必须为零。

| Offset | 类型 | 字段 | v1 含义 |
|---:|---|---|---|
| 0 | u8 | protocol_major | 1 |
| 1 | u8 | protocol_minor | 0 = 原控制协议；1 = 单主控配对管理；2 = 多手机轮流使用 |
| 2 | u8 | firmware_major | 实际发布版本，不伪造 |
| 3 | u8 | firmware_minor | 同上 |
| 4 | u8 | firmware_patch | 同上 |
| 5 | u8 | hardware_profile | 1 = 当前 C3 三逻辑通道映射；不代表接了三只实际舵机 |
| 6–9 | u32 | capabilities | 见下表 |
| 10 | u8 | recommended_target_hz | 建议 20 |
| 11 | u8 | max_target_hz | v1 初值 20；客户端不得超过 |
| 12–13 | u16 | max_transition_ms | 初值 2000 |
| 14–15 | u16 | lease_timeout_ms | 初值 6000 |
| 16 | u8 | security_policy | 1 = SC Passkey 单主控；2 = SC Passkey 多 bond、单连接 |
| 17 | u8 | logical_channels | 3 |
| 18–19 | u16 | reserved | 0 |

能力：bit0 SET_TARGET；bit1 HALT；bit2 短程平滑；bit3 已实现受保护的 bond 授权策略；bit4 StateSnapshot；bit5 真实电量采样；bit6 ARM；bit7 PAIRING_CODE_MANAGEMENT（v1.1包含换绑）；bit8 SHARED_PAIRING（v1.2，最多8个bond、单连接；此位出现时不支持换绑操作9/10）。BLE 首版必须实现 bits 0/1/2/3/4/6；没有电量传感器则 bit5=0。其余位为零，不在这里声称旧 UDP 已同时启用。

App 必须检查 major 和必需能力，不只判断服务名。major 不兼容时禁止运动并展示升级说明。minor 在主版本兼容前提下用于新增可选能力，不能静默重定义既有字段。

## 4. 控制帧：20 字节

| Offset | 类型 | 字段 |
|---:|---|---|
| 0 | u8 | protocol_version，固定 1 |
| 1 | u8 | opcode |
| 2–5 | u32 | sequence，从 1 开始 |
| 6–9 | u32 | session_token |
| 10–19 | 10 B | payload |

帧中不追加 CRC 或自定义加密。BLE 标准链路提供其相应完整性/安全机制，本应用的长度、版本、范围和会话检查仍然必需。首版不接受多帧拼接，一次写恰好一帧；读取 NimBLE mbuf 时按整个链总长度复制到有界缓冲，不能假设只有一个连续块。

### 4.1 操作码

| Opcode | 名称 | Token | Payload | 执行规则 |
|---:|---|---|---|---|
| `0x01` | CLAIM | 必须 0 | 全零 | 仅安全连接且无会话；创建非零随机 token，不运动 |
| `0x02` | SET_TARGET | 当前 token | 见 4.2 | 已 ARM 才接受，更新完整三通道绝对目标 |
| `0x03` | HALT | 当前 token | 全零 | 清除待发目标/插值，保持最后下发输出，保留会话 |
| `0x04` | RELEASE | 当前 token | 全零 | 执行 HALT，撤销会话，短暂发送确认后断开 BLE |
| `0x05` | KEEPALIVE | 当前 token | 全零 | 更新控制会话存活时间，不运动 |
| `0x06` | GET_STATUS | 当前 token | 全零 | 返回事件快照，不更新存活时间 |
| `0x07` | ARM | 当前 token | 全零 | App按连接自动启动策略发送，启用内置或合法维护启动姿态；已经启用则不改变输出 |
| `0x08` | SET_PAIRING_CODE | 当前 token | u32 六位码 + 6 B 零 | 仅当前已认证绑定手机；持久化成功后确认 |
| `0x09` | OPEN_TRANSFER | 当前 token | 全零 | 仅v1.1单主控模式；v1.2返回BAD_OPCODE |
| `0x0A` | CANCEL_TRANSFER | 当前 token | 全零 | 仅v1.1单主控模式；v1.2返回BAD_OPCODE |

未知操作码拒绝；所有要求全零的 payload 出现非零即 BAD_PAYLOAD。不存在把模式名字、WINK 字符串或 JSON 发送到 ControlRX 的兼容解释。

### 4.2 SET_TARGET payload：10 字节

| Payload offset | 类型 | 字段 | 合法范围 |
|---:|---|---|---|
| 0–1 | u16 | CH1 | 500–2500 |
| 2–3 | u16 | CH2 | 500–2500 |
| 4–5 | u16 | CH3 | 500–2500 |
| 6–7 | u16 | transition_ms | 0–DeviceInfo.max_transition_ms |
| 8–9 | u16 | reserved | 0 |

500–2500 是继承旧 App 的**逻辑输入编码**，不是最终舵机真实脉宽。仍经原 PulseWidth2Angle、CH3/CH2 耦合和每通道机械标定/限位，见联合方案。非法值整个请求拒绝，不执行其他两个通道，也不静默截断为合法值。

App 的 `-1` 哨兵只用于本地动作合并，不上线路；生成帧时合并成完整合法三通道。缺少某通道已知目标时先 ARM 并读 StateSnapshot，不能拿 0 或任意中点填入。

### 4.3 ARM、首次输出与停止

ARM 仍是独立的输出命令，不是固件配对或 CLAIM 的副作用。依用户授权，App在每次连接及重连完成认证/订阅/CLAIM，并载入内置逻辑范围或既有维护覆盖后自动发送ARM；不要求每次点击开始或填写配置。默认使用satori_c3_v1内置启动姿态与现有机械标定；显式配置损坏/非法时返回NOT_CONFIGURED并显示原因。用户在连接过程中暂停会撤销本次待启动流程。

- 冷启动，输出有效掩码为 0；不发未经确认的新姿态。
- ARM 在控制任务中使用合法的维护启动覆盖或内置satori_c3_v1启动逻辑值；显式非法配置返回NOT_CONFIGURED，PWM仍禁用。内置默认不等同于已经过当前装配的实测验证。
- 第一次 ARM 采用该板卡启动值并标记有效，不声称真实转轴已到位。发送 ACK 后，App 读取 StateSnapshot，以实际已下发逻辑值初始化合并器。
- 后续 ARM，若已有有效输出则幂等保持，不反复回到启动姿态。设备断开再连接时仍可保留上次输出；设备重启才重新进入未 ARM。
- 未 ARM 的 SET_TARGET 返回 NOT_ARMED，不缓存为未来自动执行的目标。
- HALT/RELEASE 不等于关闭 PWM，也不等于物理急停；无输出时执行它们也不能意外启动 PWM。

这避免“第一次手动调眼皮时 App 顺带把另外两个未知通道强制设成 1500”。实体无位置传感器仍有上电位置不确定性，第一次 ARM 必须在实际装配上验证。

## 5. 会话、序号、幂等和租约

### 5.1 会话生命周期

每条新 BLE 连接只建立一个控制会话。配对/bond 成功并订阅后，App 使用 sequence=1、token=0 CLAIM。成功响应带新 token；token 与当前连接句柄、已授权手机绑定。它只是代际标签，32-bit 随机数不被当作密码。

只有当前安全连接和 token 才能修改输出。断链、lease 过期和 RELEASE 都使会话失效、撤销目标；断链清空响应缓存。lease 过期后设备主动断开，重新控制必须建立新连接、新 token。避免同一连接上反复 CLAIM 与旧回调混用。

有效的新 CLAIM/ARM/SET_TARGET/HALT/KEEPALIVE 更新最后活跃时间；GET_STATUS、读快照、错误帧、旧序号和缓存重放不续租。每 2 秒 KEEPALIVE，6 秒无有效更新即超时，是建议初值，真实参数由 DeviceInfo 告知。超时不能通过不断发送畸形包来续命。

RELEASE 完成后保留短暂的同连接响应窗口，例如最多 500 ms；同一请求可取缓存确认，不接受新控制，再断链。HALT 保持连接及心跳，因此“暂停”与“断开”是不同操作。

### 5.2 顺序规则

App 单一发送器在真正提交帧时分配递增 u32 sequence。抛弃未发送的触摸点不会占用大量序号。1 开始，0 保留（收到 0 返回 OLD_SEQUENCE）；同连接不允许回绕，接近 u32 上限应先停止并重新连接。

固件按连接/会话维护已成功受理的最高序号及有界响应缓存。精确重放缓存内的同一帧可以返回原 ACK，但不再次执行或续租；同序号不同内容返回 SEQ_CONFLICT。已成功受理但离开缓存的旧帧返回 OLD_SEQUENCE，绝不当新命令执行。比最高受理序号小的未缓存帧也返回 OLD_SEQUENCE。

长度、版本、参数、安全或会话验证失败时不更新最高受理序号，也不改变输出。App 收到错误后仍用下一个新序号，而不是编辑同序号帧不断试探。缓存至少容纳最近 16 个已完成响应，内存有上限。

同序号同内容仍在控制任务处理时，不二次入队：复用该 pending 请求的结果。CLAIM 的重复请求返回第一次生成的 token；不能每次重试换 token。未订阅 EventTX 的 CLAIM 返回 SUBSCRIPTION_REQUIRED。

对已经释放或过期的会话，旧 CLAIM 不能因有缓存而重新取得控制权。RELEASE 短窗口中只重发 RELEASE 原确认；其余旧请求 BAD_SESSION。断链后所有旧缓存作废。

### 5.3 原子停止和迟到任务

NimBLE 回调只负责安全检查、完整复制、解析和提交有界任务，不做长时间插值。控制任务是输出唯一写入者。

HALT/RELEASE/断链/lease 过期都提升内部执行代数，清掉未执行目标和插值。已交给其他回调/队列的任务在执行前再次核对代数与 session，防止“HALT 后一个迟到的 SET 又让设备动起来”。内部代数不需要上线路。

SET_TARGET 按单个最新目标槽位合并，但必须保留 HALT/RELEASE 等屏障的顺序。ACK 可表示目标已受理，StateSnapshot 才显示控制任务已执行到的序号。安全停止事件不排在大量目标后等待；应至迟在下一控制周期处理。若任务队列满，返回 BUSY 或丢弃尚未执行的旧目标，不能无界占内存。

## 6. EventTX：20 字节

沿用控制帧头部：version、opcode、sequence、session_token。回复 opcode = 请求 opcode OR `0x80`，因此 CLAIM_REPLY=`0x81`、ARM_REPLY=`0x87` 等。异步状态事件 opcode=`0xE0`、sequence=0。

回复的 sequence 与请求完全相同。CLAIM 成功回复头部 token 为新 token；RELEASE 回复使用刚释放的旧 token 便于关联；普通成功回复为当前 token。错误回复使用所收请求 token 方便定位，但不表示认可它；客户端以请求匹配为准，不能从错误回复更新 token。

Payload 为 10 字节：

| Payload offset | 类型 | 字段 |
|---:|---|---|
| 0 | u8 | result |
| 1 | u8 | control_state |
| 2–5 | u32 | last_applied_sequence |
| 6 | u8 | flags |
| 7 | u8 | battery_percent；255 = unknown |
| 8–9 | u16 | reserved = 0 |

`control_state`：0=UNCLAIMED；1=CLAIMED_HOLDING；2=INTERPOLATING；3=FAULT。INTERPOLATING 仅表示软件正在推进输出，不证明真实转轴正在运动。是否已经 ARM 由 flags 的输出有效位及 StateSnapshot 掩码区分；CLAIMED_HOLDING 可以尚未 ARM。

`flags`：bit0 三通道下发逻辑值有效；bit1 至少一通道插值中；bit2 当前连接已通过保存的 bond 授权；bit3 当前会话有效；其余位零。

`last_applied_sequence` 是控制任务最近实际处理的 ARM、SET_TARGET、HALT、RELEASE 的序号，不是所有通信包的最新序号，也不是电机完成序号。收到 SET ACK 时其 last_applied 可以尚未推进到本次 sequence；之后异步事件/状态读会反映执行进度。

首版每个受理命令均有业务回复。状态变化可以主动通知；普通周期状态最多 2 Hz，静止时可以更低。响应缓存返回的是该命令原回复快照，不能把它误当当前状态；当前状态由新 GET_STATUS 或 StateSnapshot 获得。

### 6.1 result

| 数值 | 名称 | 含义 |
|---:|---|---|
| 0 | OK | 业务已受理/操作完成，具体见下文 |
| 1 | BAD_VERSION | 协议版本不支持 |
| 2 | BAD_LENGTH | 消息长度错误 |
| 3 | BAD_OPCODE | 未知操作 |
| 4 | BAD_PAYLOAD | 值、时长或保留位不合法 |
| 5 | NOT_AUTHORIZED | 未获授权；通常由 ATT 安全错误更早阻止 |
| 6 | BAD_SESSION | token/会话不匹配或已过期 |
| 7 | OLD_SEQUENCE | 旧序号且无可重放缓存 |
| 8 | SEQ_CONFLICT | 同序号不同内容 |
| 9 | BUSY | 当前请求不能受理，不能据此视为已执行 |
| 10 | INTERNAL_ERROR | 内部错误，进入可诊断安全状态 |
| 11 | NOT_ARMED | 当前没有已初始化的有效输出 |
| 12 | NOT_CONFIGURED | 没有有效启动配置或关键板卡参数异常 |
| 13 | SUBSCRIPTION_REQUIRED | 未订阅业务结果通知 |

无法安全解析请求头的短帧、超长写、未加密访问，可以只返回对应 ATT 错误，不拼接虚假的业务序号。20 B 且头部可解析的参数/版本等错误可返回相应事件；无论采用哪一层拒绝，不能执行输出。

### 6.2 三种成功不能混淆

1. GATT Write Response：蓝牙属性写入操作完成，不代表控制命令已执行。
2. Event OK：CLAIM/ARM/HALT/RELEASE 等操作已经在相应状态机落实；SET_TARGET 表示通过验证并受理进入最新目标槽位，可能被更新目标替代。
3. 真实舵机到位：本硬件没有传感器依据，不能报告。

ARM ACK 只在启动输出/有效状态已建立后发送；HALT ACK 只在控制任务已取消插值后发送；RELEASE ACK 只在已停止并撤销 token 后发送。不能在进入任务队列时就提前发这些 OK。

## 7. StateSnapshot：20 字节

| Offset | 类型 | 字段 |
|---:|---|---|
| 0 | u8 | protocol_version = 1 |
| 1 | u8 | control_state |
| 2–5 | u32 | 当前 token，无会话则 0 |
| 6–9 | u32 | last_applied_sequence |
| 10–11 | u16 | last_commanded_CH1 |
| 12–13 | u16 | last_commanded_CH2 |
| 14–15 | u16 | last_commanded_CH3 |
| 16 | u8 | valid_channel_mask；bits0/1/2 |
| 17 | u8 | interpolating_channel_mask；bits0/1/2 |
| 18 | u8 | battery_percent；255=unknown |
| 19 | u8 | reserved=0 |

无效通道对应值写 0，仅表示未知；不违反 SET 的 500–2500 输入约束，因为这是不同报文类型。设备首次上电 valid mask=0；ARM 后通常=7。正在插值时这些值是最近下发的逻辑值，不是最终目标或物理传感器读数。快照必须在同一个控制任务版本上原子获取，不能拼接不同周期的通道和序号。

App 界面区分“期望目标”“已下发逻辑值”和“未知实际位置”。没有真实电量时显示未知，不伪造 50%。

## 8. App 发送、动作和重试

全 App 同时只有一个控制运行时和一个 GATT 写操作在途。可靠控制命令优先于普通目标。GATT 回调和业务事件可能先后顺序不同，使用 sequence 汇合，不依赖固定回调顺序。

CLAIM/ARM/HALT/RELEASE 使用业务确认；初始业务超时可取 500 ms、最多 3 次同字节重试。retry 使用原 sequence，不重复执行。安全配对等待不计入 500 ms 命令计时器，应在完成配对后才开始控制会话。重试耗尽则显示未确认、停止本地动作，必要时断链触发固件保护。

手动连续目标最多 20 Hz；待发送目标容量 1，只保留最新触摸。若底层写耗时变长就自然降频，不复制出一个无限发送队列。目标 ACK 丢失时可以查询状态判断健康，不补发已被新目标覆盖的历史位置。不能因 GATT 写成功就无限忽略没有业务回复的情况。

预设关键帧由动作播放器安排，不能直接套用手动 latest-only 规则丢弃闭眼/开眼帧。正常播放等待前一帧被受理，遵守 `duration` 的时间间隔；若传输过慢导致关键帧过期，取消该次预设并提示，而不是补播积压帧或悄悄跳过关键帧。`duration` 与 `transition_ms` 分别建模，保留旧默认平滑 200 ms 的有效语义。

手动操作优先；停止会撤销未提交的目标、预设回调和自动调度。每次重连完成自动发送ARM并读回StateSnapshot重新初始化目标合并器；不恢复旧Auto调度、预设或在途目标。用户暂停保持当前连接的输出暂停，下一次重新连接会重新自动启用输出。

## 9. 推荐连接顺序

```text
用户在前台点连接
  → 完成 Android 权限 / 启动 connectedDevice 服务
  → 服务运行时扫描并建立唯一 GATT 连接
  → 发现服务、读取版本与身份
  → 访问安全特征触发系统配对（首次）或恢复加密（已绑定）
  → 完成认证、检查身份、订阅 EventTX
  → CLAIM(seq=1, token=0)
  → 收到 CLAIM_REPLY，保存 token；读取 StateSnapshot
  → 载入按认证身份保存的维护范围；没有覆盖时采用内置逻辑范围
  → 自动 ARM（若本次连接已被用户暂停则不发送）
  → ARM确认后读取有效 StateSnapshot，初始化三通道合并器；无旧动作回放
  → SET_TARGET / KEEPALIVE / GET_STATUS
  → 暂停：HALT，清掉本地动作，心跳可继续
  → 结束：RELEASE；确认或超时后断开、停止前台服务
```

绑定、连接、控制权、输出有效、动作模式是不同状态。不要把一个 `isConnected=true` 同时解释成这些条件全部成立。

## 10. 双仓库测试要求

配套 JSON 是**编解码和状态机测试数据，不是机械标定，也不是允许在真机上执行的动作清单**。测试中示例 device ID、token、版本和通道值均为合成值。

两端读取同一份向量，分别验证编码字节、解码字段、错误拒绝、幂等和断链。使用各自语言直接实现显式 endian 编解码，不能将 C/C++ 内存结构体整体 memcpy 后声称跨平台兼容。

必须覆盖：未授权/错误 token；未 ARM；错误长度/范围/保留位；重复同帧；同序号不同帧；过期帧；CLAIM ACK 丢失；暂停与在途目标竞争；lease 超时；设备重启；缓存淘汰；32-bit 序号边界；未知电量/输出；20 B 基本 MTU；UI 生命周期不改变会话所有者。

向量只提供最小起点，不替代属性测试、模糊测试及硬件联调。实现不得把这个 JSON 中的合成配置信息打包成真实设备的出厂身份。

## 11. 外部依据与版本边界

这些资料支持底层 API/限制，不代表本项目协议已经由其验证：

- ATT 写/通知及 MTU：`https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-61/out/en/host/attribute-protocol--att-.html`
- ESP32-C3 GATT 数据交换：`https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/ble/get-started/ble-data-exchange.html`
- ESP-IDF NimBLE：`https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/bluetooth/nimble/index.html`
- 官方 passkey 示例：`https://github.com/espressif/esp-idf/blob/v5.3.2/examples/bluetooth/nimble/bleprph/main/main.c`；其中固定码及自动覆盖旧 bond 不是本项目安全策略。
- Flutter BLE 插件：`https://pub.dev/packages/flutter_reactive_ble`

实施时以阶段 A 固定的 SDK/依赖版本为准；不得因为 stable 文档已经指向更新大版本，就把现有工程无条件升级到它。


## 12. v1.1 单主控配对管理（历史兼容模式）

本节仅适用于security_policy=1且未设置SHARED_PAIRING的历史固件；v1.2当前行为见第13节。该扩展由用户于 2026-09-29 明确授权：不增加按键、不修改硬件，采用默认首次码及已绑定手机管理。原七条控制命令、UUID、帧长度、状态布局、SC/MITM 保护保持不变。新增能力 bit7 置位且 protocol_minor=1；不支持该位的固件不可接收管理命令，App 隐藏/禁用相应操作。默认码属于公开引导信息，不是每台设备独立的安全凭据。

### 10.1 修改码

SET_PAIRING_CODE 的 payload[0..3] 是 u32 LE，范围 0..999999；App 显示/输入恰好六位 ASCII 数字，保留前导零。禁止重新设置出厂默认值 123456；payload[4..9] 必须为零。格式/范围/保留位违规返回 BAD_PAYLOAD。此命令只能由加密、认证、bonded 且匹配当前 owner 的连接携带有效 token 发出。更改期间 App 先 HALT 并保持暂停。换绑窗口仍开启时修改码返回 BUSY，须先取消换绑再修改，避免在配对验证过程中改变所用的码。

新码成功持久化后才返回 `0x88` OK；存储失败返回 INTERNAL_ERROR，不用未持久化值覆盖当前有效码。管理写入与可靠重试沿用序号/至少16项响应缓存：同字节重放不重复写 NVS，同序号不同内容返回 SEQUENCE_CONFLICT。回复不包含配对码；普通状态、日志、首选项、交付产物均不得保存/回显用户的新码。六位码只用于后续首次 SMP 配对；更改它不会撤销现有 bond，也不会要求当前主控重新配对。

### 10.2 更换手机

OPEN_TRANSFER（回复 `0x89`）先落实 HALT，清除旧目标和插值，输出保持，之后开启固定60秒窗口并确认。仍使用123456时返回 NOT_CONFIGURED，用户须先改码。窗口已打开的新 OPEN 返回 BUSY；同序号重试只返回缓存，不延长时间。成功确认后短暂保留原连接响应窗口（最多500ms）再断开，App 抑制自动重连和动作回放，提示新手机使用刚设置的码连接。窗口内不允许旧控制会话再次启动运动。

旧 owner 在新手机 SC 认证并成功持久化 owner 前一直保留。临时允许最多两个 bond 记录仅用于换绑事务，任何时刻只授权一个 owner；不自动驱逐旧 bond 为陌生手机腾位置。窗口外拒绝陌生手机。窗口内新手机成功通过 SC Passkey、加密、认证、bond 后，先持久化新 owner，再撤销旧 bond。持久化失败不能确认转移，也不能先删除旧 owner。新手机 CLAIM 后仍暂停，不自动 ARM。

超时、取消或设备重启均关闭窗口，未完成的换绑保留原 owner，并清理未被提交的候选 bond。旧手机可在窗口内重新连接，以 CANCEL_TRANSFER（`0x8A`）取消；无窗口时该命令幂等成功。不能通过知道 DeviceIdentity、token、默认码或调用公开 GATT 读取来开启换绑。旧手机丢失/系统bond丢失且无法认证时，只能使用 USB 恢复。

配对码修改/换绑状态均属于软件业务确认，不是物理运动完成或真实位置保证。原v1向量继续作回归；扩展向量在 `satori_ble_v1_1_management_vectors.json`，其中所有码均为合成测试输入，不可当作真实已配置设备的秘密。


## 13. v1.2 多手机轮流使用与连接自动启动

用户明确要求多手机使用同一码、不同时间连接，并在每次连接后直接开始。新固件DeviceInfo为major1/minor2、firmware0.2.2、capabilities=0x000001df、security_policy=2；无真实电量仍不设置bit5。UUID、原控制帧、SET_PAIRING_CODE以及快照长度保持不变。原v1.0/v1.1向量保留历史codec回归，新策略向量为`satori_ble_v1_2_shared_pairing_vectors.json`。

最多8个已保存bond、一个物理连接。连接被占用时停止可连接广播，其他手机不能排队接管或抢占；连接失败或断开后恢复广播。新手机以当前六位码完成SC Passkey、加密、认证及bond持久化后加入；已绑定手机恢复其加密bond。拒绝Just Works、未bond控制及存储已满的新注册；满额不驱逐已有手机。升级保留既有bond，不再按旧owner记录删除其他健康bond。

SET_PAIRING_CODE仍先暂停，校验六位ASCII数字、禁止123456、持久化后ACK，并使用同字节重试缓存。所有已绑定手机均有相同的控制与改码权限。旧码不能用于新注册；已有bond不因改码失效。当前版本不提供无线逐台删除bond功能，清理需USB维护。OPEN_TRANSFER/CANCEL_TRANSFER在此模式返回BAD_OPCODE，不产生暂停、断链、开窗或删bond等副作用；旧UI可通过bit8判断隐藏这两个入口。

固件上电仍不自行输出；App每次取得会话后按第4.3节发送ARM，实现连接自动开始。按用户追加要求，采用内置默认姿态和逻辑范围，不要求普通用户配置。未知通道仍必须等ARM后读设备快照，不能在App中用默认值伪装读数。自动启动失败不得假报输出已启用；NOT_CONFIGURED保持连接且显示配置异常。暂停不被当前连接的迟到自动ARM覆盖；下一次新连接/重连会再次自动启动。自动启动只启用合法输出，不恢复上一手机或上次会话的预设/自动动作。


### 13.1 内置产品默认参数

用户明确要求不把安全范围或启动配置作为日常使用必填步骤。内置profile `satori_c3_v1`沿用Qt生产客户端输入定义：三通道逻辑范围500..2500，首次ARM逻辑姿态[1500,1500,1500]（来源SatoriManagerContent/mobileclient.h中的MIN_PWM_VALUE/MAX_PWM_VALUE与mobileclient.cpp构造器currentCH1/2/3）。这是一份产品默认参数，不声称当前装配已做真机机械验收，也不是将fake样例作为已确认配置。

App无自定义收藏范围时采用内置范围，原有用户范围覆盖继续优先；维护字段默认折叠。固件无自定义启动配置时使用内置姿态，原有合法、明确选用的维护启动值继续优先；原GPIO、舵机scale/offset/zero/reverse/min/max与CH3/CH2耦合均保持，实际角度仍经机械标定与限位。损坏配置不能因内置默认而被静默忽略。上电仍不输出，只有连接认证后App发送ARM才启用。
