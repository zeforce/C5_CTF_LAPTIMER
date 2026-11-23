# C5 SPI 当前规范：合约5 / 固件1.4

本文对应本仓库源码。线协议版本、合约revision和固件版本是三个不同字段。C5提供采集与通用事件，不处理圈数、队伍或积分。

## 传输

SPI mode0、100kHz已验证，固定32字节全双工事务。8字节头依次为magic `CV`、线版本1、command、sequence、status、payload_length、reserved。请求status / reserved必须0，payload最多22字节；CRC-16/CCITT-FALSE，初值0xffff、多项式0x1021，覆盖头和实际payload，CRC低字节在前。余字节填0，多字节载荷均little-endian。

READY高允许一个事务，完成后先观察低再等待高；C5低保持至少2ms。C3等待READY和事务完成各有2秒超时。结果出现在下一事务，发送NOP取得；请求及NOP需同一总线互斥锁。短事务返回command=NOP、sequence=0、BAD_LENGTH，随后完整事务可恢复。线版本1与合约5不同；先读能力再调用新增接口，构建以镜像SHA定位。

## 命令及载荷

长度单位字节；字段按列出顺序编码，u8/u16/u32/u64均无符号。

| ID | 命令 | 请求 / 成功响应 | 字段及行为 |
| --- | --- | --- | --- |
| 0 | NOP | 0 / 0 | 取上一事务响应 |
| 1 | INFO | 0 / 17 | MAC6、session u32、配置channel / rf_active / faults各u8、transactions u32 |
| 2 | RF_STATS | 0 / 20 | windows / valid_windows / receive_errors / last_power / last_clip各u32 |
| 3 | GET_CONFIG | 0 / 8 | channel / gain各u8，enter / exit / confirm_ms各u16 |
| 4 | SET_CONFIG | 8 / 0 | 同3；成功停止、清校准 / 队列 / 候选 |
| 5 | OBSERVATION | 0 / 16 | power / clip各u16，samples / overflow各u32，depth / active / calibrated / reserved各u8 |
| 6 | EVENT | 0 / 18 | sequence u32、time_us u64、power / clip各u16、edge / valid各u8；空队列全0，读取不出队 |
| 7 | ACK_EVENT | 4 / 0 | 队首sequence；最后成功ACK的非零序号可重复确认 |
| 8 | ACTIVE | 2 / 0 | active / calibrated各u8，限0/1；启动需校准且无溢出锁存 |
| 9 | SCAN | 1 / 16 | start=0,8…56；8个u16功率，同步扫描后恢复频道 |
| 10 | CAPABILITIES | 0 / 22 | 见能力字段 |
| 11 | CLOCK | 0 / 12 | node_time_us u64、session u32 |
| 12 | OBSERVATION_DETAIL | 0 / 22 | time_us u64、power / clip各u16、samples / session各u32、quality / 配置channel各u8 |
| 13 | GET_LIMITS | 0 / 12 | observation_ms / clip_limit_pm / varied_min_pm / min_peak_delta各u16，max_hold_ms u32 |
| 14 | SET_LIMITS | 12 / 0 | 同13；清校准 / 队列 / 候选 / 旧观测 |
| 15 | SCAN_START | 2 / 0 | start / count各u8；count>=1，start+count<=64 |
| 16 | SCAN_STATUS | 0 / 12 | state / completed / total / channel各u8，power / clip各u16，generation u32 |
| 17 | SCAN_CANCEL | 0 / 0 | 请求取消；查询状态确认结束 |
| 18 | SCAN_RESULT | 1 / 8 | 请求channel；响应channel / ready / quality / reserved各u8、power / clip各u16 |
| 19 | HEALTH | 0 / 22 | faults / active / calibrated / depth各u8；overflow / rx_errors / malformed / transactions各u32；RF样本年龄ms u16饱和65535 |
| 20 | RECOVER | 1 / 0 | mask1清溢出锁存、2重施频道增益、3两者；停止且队列空 |
| 21 | CHANNEL_INFO | 1 / 8 | 请求index；响应table_revision / index / band / position各u8、MHz u16、flags / reserved各u8 |

## 参数与能力

channel=0..63，gain=0..63，enter>=exit+当前min_peak_delta（宽整数比较），confirm_ms=1..1000。C5内置R5 index36 / gain52 / enter20 / exit10 / confirm30ms。C5配置仅运行期保存，C3持久化期望参数并在发现/恢复后重下发；实际参数以回读为准。

LIMITS默认10 / 100 / 500 / 3 / 3000；范围：周期10..1000ms且为10倍数，削顶1..1000‰，变化比例0..1000‰，最小峰差>=1且<=当前enter-exit，保持100..60000ms。

CAPABILITIES偏移：0 schema=1，1 contract=5，2 firmware_major=1，3 firmware_minor=4，4 channels=64，5 max_gain=63，6 FIFO=16，7 max_payload=22；8 features u32=255；12 observation_ms u16；14 settle_ms u16=60；16 clip_limit_pm u16；18 stale_ms u16=max(100,2×observation_ms)；20 bandwidth_mhz u16=40（固定）。feature bit0时间戳、1质量、2事件INT、3异步扫描、4limits、5最后变更重试、6健康恢复、7频道元数据。

频道table_revision=1，band按A/B/E/F/R/DJI/Avatar/HDZero排列，position=1..8，flags=1仅表示表项存在。数字条目不表示数字解码或RF表征通过，同频条目不能识别制式。功率为未标定IQ代理值，不是dBm；clip为触顶IQ对占比千分数；samples是统计窗口数，不是IQ对数。rf_active表示近期有变化采样，不代表存在VTX。

## 质量、检测与并发

quality bit0无样本或时间0；1超过动态stale_ms或时间非法；2削顶>=当前阈值；3变化窗口比例低于当前阈值；4溢出锁存；5切频稳定 / 跨采集代际 / 扫描中。扫描结果使用固定100‰削顶、500‰变化和100ms新鲜度，与可配检测阈值不同。ready=1仅表示采集完成，仍须检查quality。

进入 / 退出均确认计时；无样本、削顶、变化不足或稳定中打断确认。无样本不刷新观察时间。最大保持按实际经过时间检查，包括无效样本，到期重武装，不补造EXIT。观察时间是聚合完成的软件时间，事件时间是确认候选起点，不是硬件边沿锁存。

检测active时SET_CONFIG、SET_LIMITS、两种扫描及RECOVER拒绝；扫描运行时SET_CONFIG、SET_LIMITS、ACTIVE、同步扫描、再次启动和RECOVER返回BUSY。停止或新启动清队列，可能丢弃未处理事件，调用方须明确接受副作用。

异步state：0空闲、1运行、2完成、3取消、4错误；新任务清ready并增加generation，取消保留已完成结果。每点等80ms后聚合100ms，结束 / 取消恢复原频道并等80ms。GET_CONFIG及观测channel是配置频道，扫描中不等于瞬时调谐频道。

## 事件、健康与恢复

edge1 ENTER、2 EXIT，生产valid=1；空队列sequence=0。序号不因配置重置，以session区分重启；u32序号从0xffffffff回绕到1，跳过空队列保留值0。主机连续性检查必须遵循该规则，不能直接用大小比较判断先后；最后成功ACK仍可重复确认。session没有因回绕改变，主机不能将整次启动的所有历史序号永久作为唯一键。FIFO满不覆盖旧数据，累计溢出、锁存并停止。INT GPIO10高表示队列非空或溢出锁存；排空无故障则低，溢出排空仍高，RECOVER清锁存后低，累计溢出保留。主机仍需健康轮询。

faults bit0 RF初始化失败，1 RF采样缺失或超过1秒，2溢出锁存，3 SPI初始化失败，4累计接收错误非零。SPI初始化失败须读UART0。RF失败时配置 / 扫描 / 校准启动拒绝，停止和诊断允许。RECOVER使校准失效，不自动运行、不清累计，不能保证修复初始化失败；须排障重启。没有实际频率读回，配置回读不能代替物理频道验证。

重启丢失RAM事件及累计计数，session变化；C3须中断连续性，重新配置且保持停止 / 未校准。

## 错误与重试

status：0 OK、1 BAD_VERSION、2 BAD_LENGTH、3 BAD_CRC、4 BAD_COMMAND、5 BUSY、8 INVALID_ARG。C3映射1/4不支持、5状态不可执行、8参数错误、2/3无效响应；响应自身CRC错误另报INVALID_CRC。命令8/18/20/21长度错误返回INVALID_ARG，其余长度检查BAD_LENGTH；忙 / RF检查可能优先。

错误载荷通常空；旧同步扫描9执行失败仍返回16字节零数据和BUSY，必须先检查status，不能把零值当成功结果。

命令4/7/8/14/15/17/20缓存最后一条变更及结果。完全相同内容和sequence跨NOP / 读取重试返回原结果；下一变更替换缓存，包括失败结果。不跨重启、不去重任意历史，同序号改变参数不是重试，请求u8序号不作为长期唯一键。重复最后ACK不删除下一事件。

## 当前验证边界

CLOCK在命令处理时采样，主机可用事务前后时间估计偏移区间。硬件验证进度见[验证范围](../validation.md)。
