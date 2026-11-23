# C3 BLE设备接口

## 服务与会话

广播名`C5-VRX-CTF`，服务`c3c50000-0000-0080-0040-000000c57e10`。以下特征UUID使用同一前缀，只替换末尾四位：

| 后缀 | 内容 | 长度 |
| --- | --- | --- |
| 7e11 | 基本节点/RF/采样统计 | 32 |
| 7e12 | 命令写入 | 6..20 |
| 7e13 | 采集/校准/频道/增益/AGC/圈数/门限 | 32 |
| 7e14 | 原始计圈分页 | 22 |
| 7e15 | 扫描结果，8个u16 | 18 |
| 7e16 | 质量/缓存年龄/配置保存错误/契约版本 | 20 |
| 7e17 | 最近命令结果 | 12 |
| 7e18 | 观测读/notify，每250ms | 20 |
| 7e19 | 游戏/接收器页 | 32 |
| 7e1a | 网络/汇总/历史/诊断页 | 32 |

支持单手机，业务特征要求本地授权后的加密绑定。首次待机串口`blepair`打开120秒窗口；SC/128位加密、Just Works，无MITM防护。成员角色不开BLE。绑定删除/修复须本地维护准入，不允许手机任意覆盖绑定。

读32字节值需使用客户端长读或合适ATT MTU，默认MTU23不能把第一次20字节当完整值。

## 命令与结果

命令：`02 / opcode u8 / id u32非零 / payload`，最大20字节。下面表中的payload不含6字节头。最多一条未完成命令；轮询7e17至完成，匹配id/opcode。结果：`02 / state / opcode / reserved / id u32 / esp_err u32`，state0无命令、1执行中、2完成、3失败。

同连接代次只记最近一条：相同id和完全相同请求可重取结果；相同id不同内容拒绝。断线后结果缓存失效；已执行动作可能仍完成，重连先读取实际状态，不自动重发不确定命令。

内部串口/网络使用`01 / opcode / payload`，不同于BLE线格式。BLE后台转换后进入`network_ble_control`；不能把内部格式直接写7e12。

## 公共操作

`config8 = channel u8 / gain u8 / enter u16 / exit u16 / confirm_ms u16`。

| opcode | payload | 含义 |
| --- | --- | --- |
| 1 | config8 | 保存并等待主槽实际应用验证 |
| 2 / 3 | on u8 | 主槽校准标志 / 运行控制 |
| 4 | offset u16 | 原始圈时偏移0..199 |
| 5 / 6 | 空 / start u8 | 扫描 / 扫描分页0,8..56 |
| 7 | on u8 | AGC请求；完成请求不等于增益搜索结束，读扩展状态 |
| 8 | mode u8,min u16,duration u32,capture u16 | 短规则入口，时间单位ms |
| 9 | index,enabled,channel,team,slot各u8,RGB u24 | 修改飞手（仍保留名称） |
| 10 | action u8 | 0结束、1新场、2继续、3暂停 |
| 11 / 13 | page u8 / offset u16 | 游戏选页0..130 / 圈时偏移 |
| 12 | index u8,UTF8名称1..12字节 | 不允许内嵌NUL |
| 14 | role u8,channel u8,network u32 | 开始组网配置批次 |
| 15 | half u8,key8 | 分两半提供16字节密钥，half0/1 |
| 16 / 17 | hostMAC6 / 空 | 提供主机MAC / 提交配置并重启生效 |
| 18 / 46 | page u8 / page u16 | 网络选页0..244 / 256..416 |
| 19 | 空 | 主机同步完整已保存业务规则，不复制RF配置 |
| 20 / 21 | offset u16,chunk1..12 / checksum u32 | 完整草稿分片与提交，见下文 |
| 22 | config8 | 仅保存期望RF配置，后台应用 |
| 23 | resume u8 | 新场/继续预检0/1，不开始 |
| 25 / 37 | 空 | 保存网络结果 / 冻结网络结果读取页 |
| 26 | MAC6,config8 | 定向成员RF配置 |
| 27 | MAC6,on u8 | 定向成员主槽校准 |
| 31 | freshness u16,drain u16,recover u16 | 观测新鲜度ms及守点百分比1..1000 |
| 32 / 33 | 空 | 固定当前成员名单 / 解锁名单 |
| 34 / 35 / 36 | oldMAC6,newMAC6 / MAC6 / MAC6 | 替换 / 删除 / 添加成员 |
| 38 / 39 | ledOn,beepOn,brightness各u8 / MAC6加上述3字节 | 本地 / 成员声光开关与亮度 |
| 40 / 41 | 空 | 维护态重启 / 保存本机成绩 |
| 42 / 43 | 空 / on u8 | 冻结本机事件页 / 本机事件采集 |
| 44 / 45 | MAC6,on u8 / 空 | 成员采集 / 冻结网络事件页 |
| 47 | slot u8,config8 | 保存本机指定槽配置 |
| 48 | MAC6,channel u8,packed u8,enter/exit/confirm各u16 | 成员槽配置，packed=(slot<<6)|gain |
| 50 / 51 | slot,action各u8 / MAC6加上述2字节 | 本机/成员槽：0停、1校准、2采集、3清校准 |
| 52 | mode u8,min/duration/capture各u32 | 完整宽度规则入口，时间ms |
| 53 | on u8,channel u8 | 本地锁频道寻机，非模式7，不广播 |

opcode24（明确session开始）、28..30（定时准备/提交/取消）是组网内部控制，不作为普通客户端绕过预检的入口。成员控制、定向命令和维护动作另有角色/场次限制，详见`network.c`与`command_policy.h`。

## 完整规则248字节

0 mode u8；1..3填0；4最短圈ms u32；8场次ms u32；12占领/生命ms u32；16新鲜度ms u32；20衰减百分比u16；22恢复百分比u16；24开始八个28字节飞手：enabled/channel/team/slot各u8、RGB u32、名称17字节含NUL、尾部3字节填0。名称实际限制12字节UTF-8。禁止发送未初始化填充。

20从offset0按序发送，整批120秒有效期；21对全部248字节作FNV-1a32（初值2166136261，每字节xor后乘16777619，模2^32）提交。错序/越界或提交后清暂存，重试从0开始。完整校验/保存成功后重读规则与飞手；它不是全网原子事务。主机开场前同步成员，独立节点跳过19。

## 游戏页7e19

| 页 | 内容/格式 |
| --- | --- |
| 0 | format1：mode/state/owner/challenger/fault/winner偏移2..7，session8、elapsed12、progress16、red20、blue24各u32，present28、known29 |
| 1 / 27 | 草稿/冻结规则：mode2；min4、duration8、capture12、freshness16、drain20、recover24各u32 |
| 2..9 / 28..35 | 草稿/冻结飞手：enabled2,channel3,team4,slot5,RGB u32@6,name17@10；冻结页session u32@28 |
| 10..17 | 飞手战果：index2，total4、fastest8、fastestLap12、captures16、ownedMs20各u32 |
| 18..25 | 圈时：index2,count3,offset u16@4,total u32@8，最多5个u32圈时@12 |
| 26 | pilotOwner2,soloStarted3,soloFailed4,lifeMs u32@8,modeMask u32@12,capacity16 |
| 36 / 37 | 本机成绩保存状态 / C5时钟映射诊断 |
| 38..70 / 71..104 / 105 | 实时事件 / 冻结事件快照 / 主槽采集状态，见event_service.h、event_delivery.h |
| 106..109 | format24接收器状态，slot1,enabled2,flags3,session u32@4,lastError@8,errors@12,ageMs@16,desired8@20,restarts@28 |
| 110..113 | format25配置：slot1,haveDesired2,haveObserved3,desired8@4,observed8@12,error@20,pending24,flags25,captureError@26,restorePending30 |
| 114..117 | format27各槽时钟，见receiver_service.h |
| 118..121 | format28采集：slot1,requested2,stopPending3,error u32@4,overflow u32@8 |
| 122 | format29堆/NVS资源，见controller_game_page |
| 123..130 | format30各飞手信号，见game_signal.h |

106页flags位：online0、RFvalid1、calibrated2、active3、haveDesired4、haveObserved5。配置pending与在线分别检查；保存成功不等于已经应用。配置页25 flags另有定义，见receiver_config_page，不能混用。

网络7e1a页及遥测见[ESP-NOW](espnow.md)。7e18观测：format2,quality1,flags2,channel3,session u32@4,seq@8,power u16@12,clip u16@14,cacheAgeMs u32@16；flags在线0/RF1/运行2。通知可丢，不代替事件队列，序号需按u32回绕比较。

## 最小客户端示例

已完成本地授权/加密连接后，读取游戏摘要的命令是`02 0b 01 00 00 00 00`：opcode11、id1、page0。先轮询7e17匹配id1且state2，再完整读取7e19并检查format1/page0。下一请求使用新非零id；选择不同页不能重复使用id1。

锁定R5寻机示例：`02 35 02 00 00 00 01 24`（opcode53，id2，on1，channel36）。完成后读网络页244查看寻机状态。关闭使用新id、on0；寻机需在比赛停止后启用。

错误处理至少区分：写入未接收、已接收执行中、业务失败、断线后执行结果不确定。客户端应显示服务端esp_err，不把重试按钮实现成无条件重复开始/停止命令。
