# 协议索引

| 链路 | 版本/文档 | 权威实现 |
| --- | --- | --- |
| C3 ↔ C5 SPI | 线版本1、合约5、C5固件1.4；[SPI](c5-spi.md) | common/node_contract.h、C5 main.c、C3 node_spi.c |
| 手机/独立客户端 ↔ C3 BLE | 命令/结果格式2、设备服务revision2；[BLE](c3-ble.md) | ble_control.c、main.c、game_page.c |
| C3 ↔ C3 ESP-NOW | packet.version=1，同源码布局；[ESP-NOW](espnow.md) | network.c、network_*头文件 |

所有多字节整数均little-endian，偏移从0开始。ESP错误码为32位数值；先检查结果/格式/状态，再解释载荷。BLE“写成功”、SPI“请求已收到”、网络MAC发送回调都不等于业务执行完成。

相同页号在游戏特征与网络特征中含义不同，例如游戏106..109是接收器，网络106..113是成员声光。必须检查format字段，不能混读。分页通常不是原子快照，读取前后复核session/revision/成员boot，发生变化则重读。

本版不提供旧固件降级兼容。C3网络节点应使用同一源码版本；不得仅凭packet.version仍是1就混用不同结构布局。协议未声明所有参数对任意比赛状态有效，运行期保护由服务端执行。
