# 构建与刷写

## 工具链

使用官方ESP-IDF **v6.0.2**及其安装器匹配的Python、CMake、Ninja、RISC-V工具链。当前验证的SDK提交为`7101770dc6db2667b3c477cc31365dd1acd6db4e`。从 https://github.com/espressif/esp-idf 按官方安装文档取得该版本及子模块，再执行`export.sh`或`export.ps1`激活环境。仓库不绑定E盘或私人安装路径。

C5读取内部PHY符号/寄存器，不是跨SDK版本稳定接口。要求官方SDK内`components/esp_phy/lib/esp32c5/libphy.a` SHA256：

```text
dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb
```

CMake会拒绝其他库；不要删除校验来声称兼容。SDK及子模块依赖各自许可，不随仓库上传二进制。C5正常构建不启用C5_MINIMAL/C5_ACCEPTANCE_TEST/C5_RF_FAILURE_TEST等合成测试开关。

## 基线构建

在已激活的ESP-IDF环境，从仓库根目录运行：

```sh
python tools/build.py c3_controller
python tools/build.py c5_detector
```

每个工程的`sdkconfig.release`是完整公开基线，脚本仅在本地sdkconfig不存在时复制；已有个人配置不会覆盖。默认C3双C5、USB调试、灯GPIO20、蜂鸣器21；C5为4MB flash、UART0控制台。C3自定义2MB分区表见`partitions.csv`。需与实际芯片/Flash容量匹配。

也可以手动：进入工程，复制`sdkconfig.release`为`sdkconfig`，执行`idf.py build`。修改配置用`idf.py menuconfig`，单C5应关闭C3_DUAL_C5并重新确认声光GPIO允许范围。

## 安装

首次安装或明确需要更新bootloader/分区时，在对应工程执行：

```sh
idf.py -B build-public -p PORT flash monitor
```

使用tools/build.py默认生成的`build-public`。先核对目标芯片与串口。已有同分区设备通常仅更新应用，保留NVS：

```sh
python -m esptool --chip esp32c3 -p PORT --after hard-reset write-flash 0x10000 build-public/c3_controller.bin
python -m esptool --chip esp32c5 -p PORT --after hard-reset write-flash 0x10000 build-public/c5_detector.bin
```

两个命令分别在各自工程目录使用。不要对现有设备随意erase-flash；它会删除配对、网络密钥、配置和成绩。芯片eFuse/Secure Boot不属于普通刷写步骤。

C5运行固件时可通过UART0 115200发送`boot`加换行进入ROM下载器，随后esptool使用`--before no-reset`；控制台与刷写不能同时占用端口。原生USB可作为下载通道，应用日志仍走UART0。没有可运行固件时，首次下载可能仍需开发板的硬件下载入口。

## 可移植核心测试

安装Python3及支持C11的gcc/clang，运行`python tools/test_core.py --cc gcc`。也支持`--cc <tcc路径>`。此测试只覆盖纯C规则/协议核心，不调用小程序、串口、RF或SDK硬件驱动。测试清单来自当前工程47个宿主C场景集合。
