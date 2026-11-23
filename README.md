# C5 CTF Laptimer

基于 ESP32-C5 射频检测与 ESP32-C3 控制器的 FPV 计圈、夺旗实验项目。

包含 C5/C3 固件源码、构建配置、接口文档和核心测试，采用 [GPL-3.0-only](LICENSE)。

## 架构与功能

`5.8GHz信号 → C5接收/检测 → SPI → C3规则引擎 → BLE客户端`

多个 C3 可通过 ESP-NOW 组成固定主从网络：主机开启 BLE，最多8个成员通过主机同步比赛配置、开场和成绩。默认独立模式。

- C5：64项频道表、增益/门限/确认时间配置、观测与质量、ENTER/EXIT队列、扫描、时钟、健康与恢复、UART进入下载器。
- C3：四槽软件模型，默认启用两颗C5；配置持久化、各槽采集/校准、自动增益、事件/时间映射。
- 六模式：自由计圈、驻留占领、快速团队夺旗、单人守点、个人混战、首次占领锁定；最多8名业务飞手、红蓝队、场次控制、历史成绩。
- WS2812/有源蜂鸣器反馈、锁定频道寻机；BLE与ESP-NOW接口可供独立客户端实现。

计圈时，每名启用飞手需要独立接收槽。数字图传频道表用于频点选择，检测输出为未标定的IQ功率值。

## 文档

- [构建、刷写与依赖](docs/build.md)
- [接线与开发板引脚](docs/wiring.md)
- [使用与调试](docs/usage.md)
- [六模式规则](docs/modes.md)
- [协议索引](docs/protocols/README.md)：[C5 SPI](docs/protocols/c5-spi.md)、[C3 BLE](docs/protocols/c3-ble.md)、[ESP-NOW](docs/protocols/espnow.md)
- [许可说明](docs/licensing.md)、[第三方声明](NOTICE.md)
- [验证范围](docs/validation.md)

## 快速构建

安装 ESP-IDF **v6.0.2** 并激活其环境；C5要求匹配的官方PHY库，详情见构建文档。

```sh
python tools/build.py c3_controller
python tools/build.py c5_detector
```

脚本使用仓库内完整基线配置`sdkconfig.release`；首次构建生成本地sdkconfig。运行可移植C规则测试：

```sh
python tools/test_core.py --cc gcc
```

## 当前状态

项目处于开发阶段，C3/C5构建、核心规则测试和双C5通信已通过。实物验证进度见[验证范围](docs/validation.md)。

## 来源

C5射频接收改编自 **Twotoz/C5VRX及其贡献者**，版本`100bf7bfa34e0892a3ab1efd856564d0df37e92a`，详见[来源声明](NOTICE.md)。

CTF玩法参考 SeekND/CaptureTheFlag；配置交互思路参考 LouisHitchcock/FPVGate。
