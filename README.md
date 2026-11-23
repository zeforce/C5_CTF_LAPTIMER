# C5 CTF Laptimer

基于 ESP32-C5 射频检测与 ESP32-C3 控制器的 FPV 计圈、夺旗实验项目。

本仓库公开 **C5/C3 完整应用源码、构建配置、接口说明与宿主机规则测试**，采用 [GPL-3.0-only](LICENSE)。不提供预编译固件，不包含微信小程序、成品硬件原理图/PCB、设备配置备份和生产密钥。源代码可自行编译及修改。

## 架构与功能

`5.8GHz信号 → C5接收/检测 → SPI → C3规则引擎 → BLE客户端`

多个 C3 可通过 ESP-NOW 组成固定主从网络：主机开启 BLE，最多8个成员通过主机同步比赛配置、开场和成绩。默认独立模式。

- C5：64项频道表、增益/门限/确认时间配置、观测与质量、ENTER/EXIT队列、扫描、时钟、健康与恢复、UART进入下载器。
- C3：四槽软件模型，默认启用两颗C5；配置持久化、各槽采集/校准、自动增益、事件/时间映射。
- 六模式：自由计圈、驻留占领、快速团队夺旗、单人守点、个人混战、首次占领锁定；最多8名业务飞手、红蓝队、场次控制、历史成绩。
- WS2812/有源蜂鸣器反馈、锁定频道寻机；BLE与ESP-NOW接口可供独立客户端实现。

8名业务飞手不等于8路并行硬件；计圈要求每名启用飞手独占接收槽。数字图传频道条目只提供频点，**不解码数字图传**。功率是未标定IQ代理值，不是dBm或距离。

## 文档

- [构建、刷写与依赖](docs/build.md)
- [接线与开发板引脚](docs/wiring.md)（不是成品原理图）
- [使用与调试](docs/usage.md)
- [六模式与业务边界](docs/modes.md)
- [协议索引](docs/protocols/README.md)：[C5 SPI](docs/protocols/c5-spi.md)、[C3 BLE](docs/protocols/c3-ble.md)、[ESP-NOW](docs/protocols/espnow.md)
- [许可、来源和分发要求](docs/licensing.md)、[第三方声明](NOTICE.md)
- [验证范围](docs/validation.md)

## 快速构建

安装 ESP-IDF **v6.0.2** 并激活其环境；C5要求匹配的官方PHY库，详情见构建文档。

```sh
python tools/build.py c3_controller
python tools/build.py c5_detector
```

脚本使用仓库内完整基线配置`sdkconfig.release`；首次构建生成本地sdkconfig。构建产物只留在本地。运行可移植C规则测试：

```sh
python tools/test_core.py --cc gcc
```

## 当前状态

这是软件框架开发基线，C3/C5已完成构建和核心规则测试，双C5基本通信已跑通。真实RF计分、手机BLE、实体声光、多成员无线负载及掉电仍需各自实物验收；不宣称达到商业最终发布标准。硬件版本和接线不同需要自行核对。

## 来源

C5射频接收改编自 **Twotoz/C5VRX及其贡献者**，版本`100bf7bfa34e0892a3ab1efd856564d0df37e92a`，详见[声明](NOTICE.md)。本项目不是官方C5VRX发布，也不代表其认可。

CTF玩法参考 SeekND/CaptureTheFlag；配置交互思路参考 LouisHitchcock/FPVGate。未将两者程序源码纳入本次发布。请遵守当地无线电法规，在允许的频段和条件下实验。
