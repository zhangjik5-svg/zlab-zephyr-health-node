# ZLab Zephyr 自适应健康监测节点

这是第三个个人化嵌入式项目：基于 **Zephyr 4.4.2** 和 **ESP32-S3-DevKitC-1**，使用 BME280 采集环境数据，通过多线程流水线进行异常检测，并根据健康状态自动调整采样频率。项目没有网页，也不依赖云端才能运行。

它和第一个 ESP-IDF 环境节点使用相同硬件，但关注点不同：第一个展示 ESP-IDF 驱动、MQTT 和离线缓存；本项目展示 Zephyr 的线程、消息队列、设备树、Sensor API、Shell、任务看门狗和 `native_sim` 自动化测试。

## 个人新增内容

- 三线程消息流水线：`sampler → analyzer → reporter`；
- 两级有界 `k_msgq`，满队列时不阻塞实时采样并记录丢包；
- 基于整数 EWMA 的异常检测器，不需要浮点运算；
- `CALIBRATING / NORMAL / ALERT` 状态机，支持连续异常确认和延迟恢复；
- ALERT 状态自动把采样周期从 5 秒缩短到 1 秒；
- BME280 缺失时自动切换到明确标记的模拟数据；
- Zephyr Shell 支持状态查看、阈值修改、测试数据注入和快速采样；
- ESP32-S3 BOOT 按键通过设备树 GPIO 控制 500 ms 快速采样模式；
- 每个工作线程注册 Task Watchdog；
- 异常检测算法在 `native_sim/native/64` 上运行 Ztest 单元测试；
- 同一份应用同时构建 Native Simulator 和 ESP32-S3 固件。

## 硬件与接线

- ESP32-S3-DevKitC-1；
- BME280 I2C 模块；
- USB 数据线。

| ESP32-S3 | BME280 |
| --- | --- |
| 3V3 | VIN/3V3 |
| GND | GND |
| GPIO 8 | SDA |
| GPIO 9 | SCL |

BME280 默认地址为 `0x76`。本项目使用设备树 Overlay 描述 GPIO8/9 和传感器，不在业务代码中硬编码 I2C 控制器。

## 获取与构建

```bash
west init -l .
west update
west zephyr-export
pip install -r ../zephyr/scripts/requirements.txt
```

运行算法测试：

```bash
west build -b native_sim/native/64 tests/anomaly_detector -d build/test -t run
```

构建 Native Simulator 应用：

```bash
west build -b native_sim/native/64 . -d build/native
```

构建 ESP32-S3 固件：

```bash
west build -b esp32s3_devkitc/esp32s3/procpu . -d build/esp32s3 -- \
  -DDTC_OVERLAY_FILE=boards/esp32s3_devkitc_bme280.overlay
west flash -d build/esp32s3
```

## Shell 命令

串口为 `115200 8N1`。输入 `help` 或：

| 命令 | 作用 |
| --- | --- |
| `zhealth status` | 查看状态、队列丢包、传感器回退次数和最新数据 |
| `zhealth config 5000 2 85` | 设置采样周期、温度偏差阈值和湿度上限 |
| `zhealth inject 35 90` | 注入 35℃、90%RH 的测试数据 |
| `zhealth fast` | 切换 500 ms 快速采样模式 |

连续执行两次 `zhealth inject 35 90` 可以让状态进入 `ALERT`。之后注入或采集正常数据，满足恢复计数后回到 `NORMAL`。

## 数据流

```text
BME280 / simulator
        │
        ▼
 sampler thread ── raw_queue ──> analyzer thread ── event_queue ──> reporter thread
                                      │
                                      ├─ EWMA + state machine
                                      └─ adaptive sampling interval
```

两个队列均为固定容量，不使用动态分配。Shell 注入数据也进入同一条 `raw_queue`，所以测试走的是真实分析路径。

## 测试范围

GitHub Actions 会执行：

1. Native Simulator 上的 Ztest 状态机测试；
2. 完整应用的 Native Simulator 编译；
3. 带 BME280 设备树 Overlay 的 ESP32-S3 固件编译；
4. 上传 `zephyr.bin`、`zephyr.elf` 和 `zephyr.map`。

CI 不能替代开发板验收。烧录后还需要检查 BME280、BOOT 按键、串口 Shell 和长时间看门狗行为。

许可证：Apache-2.0。

