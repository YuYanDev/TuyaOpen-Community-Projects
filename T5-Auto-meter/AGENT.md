# T5-Auto-meter · AI Agent 项目说明书

> 本文是面向 AI Coding Agent（Claude Opus / Cursor 等）的"项目宪法"。
> 任何对本项目的代码生成、重构、审查，必须先完整阅读本文，再开始动手。
> 如果本文与某次用户请求冲突，**以最新的用户请求为准**，但必须显式说明冲突点。

- **项目代号**: T5-Auto-meter
- **所在分支**: `feature/t5-auto-meter`（基于 `master` 切出）
- **目录位置**: `T5-Auto-meter/`（仓库根目录下的独立子项目）
- **代码语言**: C（TuyaOS 风格，参见仓库根 always_applied_workspace_rules）
- **图形栈**: LVGL v9（启用 LVGL 的 LVGL_OS_FREERTOS 模式）

---

## 1. 产品定位（What & Why）

把 Waveshare 「T5-E1-Touch-AMOLED-1.75」开发板做成一台**车载圆形仪表**：通过蓝牙连接到 ELM327 v1.5 OBD-II 适配器，从 ECU 实时读取车辆参数，再以仿机械指针仪表盘的方式渲染到 466×466 圆形 AMOLED 屏上。

核心体验关键词：
- **复古仪表感**：仿汽油表/水温表的指针、刻度、暖色背光
- **一屏专注**：每屏只显示一个仪表（再加一两个辅助小数字）
- **快速切换**：右键 KEY 一键切换下一仪表
- **可配置**：左键 PWR 长按/短按调出菜单，能勾选启用哪些仪表、调阈值
- **仪式感**：开机有"扫针自检"动画

### 1.1 支持的仪表（首版目标）

| 编号 | 仪表名     | OBD-II PID                | 单位     | 量程参考         | 备注                     |
|------|-----------|---------------------------|----------|------------------|--------------------------|
| 1    | 水温       | `0x05` ECT                | °C       | -40 ~ 130        | A - 40                   |
| 2    | 进气温     | `0x0F` IAT                | °C       | -40 ~ 130        | 备选油温（多数 OBD 不直接给出油温，先用 IAT 占位/可选 0x5C 引擎油温） |
| 3    | 引擎油温   | `0x5C`                    | °C       | -40 ~ 215        | 部分 ECU 不支持           |
| 4    | 油量       | `0x2F` Fuel Level         | %        | 0 ~ 100          | A * 100/255              |
| 5    | 机油压力   | 多为厂商专属 PID（Mode 22） | kPa/bar  | 0 ~ 7 bar        | 多数车不通过标准 PID 提供，**首版用占位+菜单中标记"不支持"** |
| 6    | 控制模块电压 | `0x42` Control Module Voltage | V    | 0 ~ 15           | ((A*256)+B)/1000         |
| 7    | 涡轮压力   | `0x70`/`0x33`             | kPa/bar  | -1.0 ~ 2.0 bar   | Boost = MAP - BARO；MAP=0x0B，BARO=0x33 |
| 8    | G 值       | 来自 QMI8658（板载）        | g        | -2g ~ +2g（线性） | 不依赖 OBD，离线也能用    |

> **注**：除非车辆 ECU 实际支持，否则油压/油温这类非强制 PID 在 UI 上要显示 `--`，且菜单中标灰，避免误导用户。

---

## 2. 硬件平台（Where）

### 2.1 板子：Waveshare T5-E1-Touch-AMOLED-1.75

- **MCU**: Tuya T5（双核 Cortex 系列，原生 Wi-Fi + BLE 5.0）
- **屏幕**: 1.75" 圆形 AMOLED，**466×466 RGB565**，QSPI 接口（CO5300）
- **触摸**: I²C，CST92xx（GPIO 20/21 = SCL/SDA，GPIO 42 = RST）
- **音频**: 板载 Codec，喇叭使能 GPIO 28
- **加速度/陀螺**: QMI8658，I²C 地址 `0x6B`（`QMI8658_ADDRESS_HIGH`），与触摸共用 I2C0
- **按键**:
  - **PWR**：GPIO 18，低有效（菜单/电源键）
  - **KEY**：GPIO 12，低有效（仪表切换）
- **电源使能（自锁）**: GPIO 19（高有效）—— PWR 长按 3s 拉低后系统断电
- **电池**: 充电检测 GPIO 30；电压采样 ADC15（GPIO 13），分压系数 `2.51/0.51`

> 引脚事实来源：`~/Project/TuyaOpen/boards/T5AI/WAVESHARE_T5AI_TOUCH_AMOLED_1_75/board_com_api.c`，
> 以及 `~/Project/YuYanDev/01_Factory_Firmware/src/dev_config/dev_config.h`。

### 2.2 外设：ELM327 v1.5 蓝牙 OBD-II 适配器

ELM327 v1.5 在市面上有两种主流形态：

| 形态        | 协议           | 备注                                                                  |
|-------------|---------------|------------------------------------------------------------------------|
| Classic BT  | RFCOMM/SPP    | 经典蓝牙串口，T5 当前栈**不直接支持** SPP，本项目**不予适配**。           |
| **BLE 4.0** | GATT 透传      | 走"BLE Serial"或 HM-10 风格透传服务，**这是本项目的唯一支持目标**。      |

> 本项目锁定 **BLE 版 ELM327**（蓝牙图标常标注 "BLE 4.0" 或 "vLinker MC+ / OBDLink CX-like"）。
> 经典 SPP 版本不支持，菜单中需明确提示用户购买 BLE 版本。

#### 2.2.1 常见 BLE ELM327 服务/特征 UUID（按厂商分类，需运行时探测）

| 厂商/型号                     | Service UUID                                     | Notify (TX→Phone)                              | Write (Phone→TX)                               |
|------------------------------|--------------------------------------------------|-----------------------------------------------|------------------------------------------------|
| HM-10/JDY-08 通用透传        | `0000FFE0-0000-1000-8000-00805F9B34FB`           | `0000FFE1-...` (notify)                        | `0000FFE1-...` (write)                          |
| Microchip/RN4870 风格        | `49535343-FE7D-4AE5-8FA9-9FAFD205E455`           | `49535343-1E4D-4BD9-BA61-23C647249616`         | `49535343-8841-43F4-A8D4-ECBE34729BB3`         |
| Vgate/iCar Pro 类            | `0000FFF0-0000-1000-8000-00805F9B34FB`           | `0000FFF1-...`                                 | `0000FFF2-...`                                 |
| Nordic NUS（少数固件）        | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`           | `6E400003-...`                                 | `6E400002-...`                                 |

实现策略：扫描时按设备名前缀（`OBDII`、`OBD2`、`vLinker`、`Carista`、`OBDLink`）+ 服务 UUID 多匹配；连接后做服务发现，命中即固化到 KV。

#### 2.2.2 ELM327 AT 命令初始化序列（连接后必须依次执行）

每条命令以 `\r` 结尾，等待 `>` 提示符回包；超时 1.5 s 视为失败。

```
ATZ            # 复位
ATE0           # 关闭回显
ATL0           # 关闭换行
ATS0           # 关闭空格
ATH0           # 关闭 Header
ATSP0          # 自动协议
0100           # 探测可用 PID（mode 01 PID 00），返回支持位图
```

#### 2.2.3 PID 解析公式（OBD-II Mode 01）

| PID    | 字段                         | 字节       | 公式                          | 单位 |
|--------|------------------------------|-----------|-------------------------------|------|
| 0x05   | Engine coolant temperature   | A         | A - 40                        | °C   |
| 0x0B   | Intake manifold pressure     | A         | A                             | kPa  |
| 0x0F   | Intake air temperature       | A         | A - 40                        | °C   |
| 0x2F   | Fuel level                   | A         | 100 × A / 255                 | %    |
| 0x33   | Barometric pressure          | A         | A                             | kPa  |
| 0x42   | Control module voltage       | A,B       | ((A×256) + B) / 1000          | V    |
| 0x5C   | Engine oil temperature       | A         | A - 40                        | °C   |
| 0x70   | Boost pressure control       | 复合       | 视车型而定，简化为 `MAP - BARO` | kPa  |

---

## 3. 总体架构

```
┌──────────────────────────────────────────────────────────────────┐
│                          User Tasks                              │
│                                                                  │
│  app_main          app_obd         app_imu         app_ui        │
│  (启动/调度)        (BLE+ELM327)    (QMI8658)       (LVGL 渲染)   │
└─────┬─────────────────┬──────────────┬─────────────────┬─────────┘
      │                 │              │                 │
      ▼                 ▼              ▼                 ▼
  app_state        obd_session    sensor_imu        ui_*     ←── 业务/数据层
   (FSM)           obd_pid        (G 值/横滚)
                                                       ▲
                                                       │ pub
   ┌──────────────────────────────────────────────────┴─────┐
   │              metric_bus  (lock-free single-writer)      │  ←── 数据中心
   │   {water_temp, fuel_level, voltage, boost, g_x/y/z, …}  │
   └──────────────────────────────────────────────────┬─────┘
                                                       │
                              tal_ble (Central) ◄──────┤
                              tdl_button       ◄──────┤
                              tkl_i2c (QMI8658) ◄─────┘
```

### 3.1 任务/线程划分（FreeRTOS 任务）

| Task          | Prio              | Stack | 周期      | 职责                                       |
|---------------|-------------------|-------|-----------|--------------------------------------------|
| `app_main`    | `THREAD_PRIO_2`   | 4096  | one-shot  | 系统初始化、注册按键回调、拉起其他任务         |
| `app_obd`     | `THREAD_PRIO_2`   | 6144  | event/2s  | BLE 扫描/连接、ELM327 AT 流程、PID 轮询       |
| `app_imu`     | `THREAD_PRIO_3`   | 2048  | 50 Hz     | 读 QMI8658，输出 G 值、横滚角到 metric_bus    |
| `lvgl`        | `THREAD_PRIO_0`   | 8192  | 系统调度   | LVGL 自带渲染线程（由 `lv_vendor_start` 拉起）|
| `ui_refresh`  | `THREAD_PRIO_4`   | 2048  | 20 Hz     | 从 metric_bus 拉数，调用 `lv_async_call`     |

> 严禁在非 LVGL 线程直接调 `lv_*`；必须用 `lv_async_call` 或 `lv_vendor_disp_lock/unlock`。

### 3.2 状态机（顶层 FSM）

```
       ┌────────────────┐
   ┌──►│  BOOT_SWEEP    │  开机扫针自检（~2 s）
   │   └────────┬───────┘
   │            ▼
   │   ┌────────────────┐    BLE 未连           ┌─────────────────┐
   │   │ BLE_SCANNING   │──────────────────────►│ BLE_DISCONNECTED│
   │   └────────┬───────┘                       └────────┬────────┘
   │            │ 命中 ELM327 BLE                          │
   │            ▼                                          │
   │   ┌────────────────┐                                  │
   │   │ ELM327_INIT    │  执行 ATZ/ATE0/...                 │
   │   └────────┬───────┘                                  │
   │            ▼                                          │
   │   ┌────────────────┐    PID 轮询                       │
   │   │  METER_RUN     │◄──────────────────────────────────┘
   │   └────────┬───────┘
   │            │ PWR 短按
   │            ▼
   │   ┌────────────────┐
   │   │     MENU       │  可选仪表/单位/亮度/解绑设备
   │   └────────┬───────┘
   │            │ 退出
   └────────────┘
```

### 3.3 数据中心（`metric_bus`）

- 由 `app_obd` / `app_imu` 单写，UI 任务和按键任务读。
- 使用 `tal_mutex_*` 保护写；UI 读时拷贝整结构体，避免局部撕裂。
- 每个指标带 `valid` 位 + `last_update_ms`，UI 据此显示 `--`/灰阶。

```c
typedef struct {
    int16_t  ect_c;        // 水温 °C
    int16_t  iat_c;        // 进气温
    int16_t  oil_c;        // 引擎油温
    uint8_t  fuel_pct;     // 油量
    uint16_t voltage_mv;   // 控制模块电压 mV
    int16_t  boost_kpa;    // 涡轮压力（带正负）
    float    g_x, g_y, g_z;
    float    roll_deg;
    BOOL_T   ect_valid;
    BOOL_T   /* ... 各项 valid */ ;
    uint32_t last_update_ms[METRIC_COUNT];
} APP_METRICS_T;
```

---

## 4. 目录结构（约定）

```
T5-Auto-meter/
├── AGENT.md                  ← 本文（不要把它当成 README）
├── README.md                 ← 给人看的简介（后续补）
├── CMakeLists.txt
├── app_default.config        ← Kconfig 片段（板子 + 字体 + LVGL）
├── .gitignore
├── include/
│   └── app_config.h          ← 全局编译开关、引脚总表
└── src/
    ├── tuya_main.c           ← user_main / tuya_app_main
    │
    ├── board/                ← 板级抽象（按钮、I2C、电源、电池）
    │   ├── board_pwr.c/.h    ← 自锁电源、PWR 按键长按断电
    │   ├── board_btn.c/.h    ← PWR & KEY 双键，事件 dispatch 到 app_state
    │   └── board_io.c/.h     ← I2C/UART/GPIO 复用 01_Factory_Firmware/dev_config
    │
    ├── sensor/
    │   ├── qmi8658.c/.h      ← 复用 01_Factory_Firmware 驱动
    │   └── sensor_imu.c/.h   ← G 值滑动平均、姿态转角
    │
    ├── obd/
    │   ├── elm327_ble.c/.h   ← BLE Central + 透传 RX/TX 队列
    │   ├── elm327_at.c/.h    ← AT 命令封装与状态机
    │   ├── obd_pid.c/.h      ← PID 解析表 + Mode 01 包打/拆
    │   └── obd_session.c/.h  ← 周期轮询，写入 metric_bus
    │
    ├── ui/
    │   ├── ui.c/.h           ← UI 入口、主题、字体
    │   ├── ui_boot.c/.h      ← 开机扫针动画
    │   ├── ui_gauge.c/.h     ← 通用指针仪表组件（参数化）
    │   ├── ui_screen_*.c/.h  ← 各仪表屏（water/oil/fuel/volt/boost/g/iat）
    │   ├── ui_menu.c/.h      ← PWR 菜单
    │   └── assets/           ← 表盘背景图、刻度图、字体
    │
    └── app/
        ├── app_state.c/.h    ← 顶层 FSM + 仪表切换列表
        ├── app_meter.c/.h    ← metric_bus 数据中心
        └── app_kv.c/.h       ← 用户配置持久化（启用列表、亮度、绑定 MAC）
```

> 命名规范遵循仓库根 `TuyaOS C Style` 规则：
> - 文件作用域变量 `s_` 前缀；内部 helper `__` 前缀且 `STATIC`
> - 类型 `_T` 后缀；枚举 `_E` 后缀
> - 块分隔符仅在不同类别之间（include → macros → types → vars → funcs）

---

## 5. UI 设计（466×466 圆屏）

### 5.1 设计原则

1. **圆屏优先**：所有控件必须避开 R<10 的角落（CO5300 边缘有约 10 px 的圆角剪裁）。
2. **可读性 > 信息密度**：高速行车场景，主数字字号必须 ≥ 80 px（Montserrat 80 或 96）。
3. **暖色低光**：默认主题 `#0B0B10` 深背景 + `#FFB347` 琥珀色刻度，避免夜驾刺眼。
4. **指针物理感**：用 `lv_arc` 不够，**必须用自绘 line + 圆心轴 + 阴影**（参考 `lv_meter` v9）。

### 5.2 仪表页面布局（圆心 233,233 半径 233）

```
                ┌─────────────────────────┐
                │       ●  WATER  ●       │   ← 顶部标题（小字 22）
                │      ╭─────────╮        │
                │   °C │░░ 89°  ░│        │   ← 中央主数字（80）
                │      ╰─────────╯        │
                │   ╭───────────────╮     │   ← 弧形刻度（angle 135°~45°）
                │  /                 \    │
                │ /     ⬤ 指针         \  │
                │ \   旋转中心 (233,260) /│
                │  \─────────────────/    │
                │      0   50   130       │   ← 刻度数字
                │ ┌──────────────────────┐│
                │ │  BLE: ●   12.4V  92% ││   ← 底部状态条（28）
                │ └──────────────────────┘│
                └─────────────────────────┘
```

- 状态条三栏：BLE 连接/电瓶电压/油量，**始终常显**（油量页除外）
- 主指针动画：使用 `lv_anim` 缓出曲线（300 ms），避免数字跳变

### 5.3 开机扫针动画（必做仪式感）

时序：
1. **0 ~ 500 ms**：黑屏 → Logo 淡入（项目 logo + "T5-Auto-meter"）
2. **500 ~ 1500 ms**：指针从最小值线性扫到最大值再回到 0（"自检"）
3. **1500 ~ 2000 ms**：状态条从底部滑入；同时 BLE 开始扫描
4. **2000 ms ~**：进入正常仪表页（默认水温）

### 5.4 菜单（PWR 短按）

- 卡片式，纵向 List
- 项：
  - `[✓] 水温` `[ ] 进气温` `[✓] 油量` ……（KEY=切下一项，PWR=切换勾选）
  - `亮度: [-][50%][+]`
  - `单位: [°C / °F]`、`[bar / kPa]`
  - `重新配对 OBD`
  - `关于 / 版本号`
- **PWR 长按 3 s**：直接断电（拉低 GPIO 19）

---

## 6. 按键交互（最终态）

| 按键           | 动作                | 在 METER_RUN          | 在 MENU                     |
|----------------|---------------------|-----------------------|-----------------------------|
| KEY 短按       | 单击                | 切换到下一启用仪表    | 移动焦点 / 切换勾选          |
| KEY 长按 1 s   | 长按                | 重置当前仪表瞬时统计    | （保留）                     |
| **PWR 短按**   | 单击                | 进入菜单              | 退出菜单（保存）             |
| PWR 长按 3 s   | 长按                | **拉低 GPIO19 关机**  | **拉低 GPIO19 关机**         |

> 使用 `tdl_button_manage` API（参考 `01_Factory_Firmware/src/dev_config/dev_config.c` 与 Waveshare 板的 `__board_register_button`）。
> 双键的 `BUTTON_NAME` 与 `BUTTON_NAME_2` 通过 Kconfig 暴露，避免硬编码。

---

## 7. 编译 / 烧录指引

### 7.1 第一次设置工具链

```bash
cd ~/Project/TuyaOpen
. ./export.sh          # 必须用 source（点空格点斜杠），拉起 venv
```

### 7.2 编译

```bash
# 仍在已 source 的 shell
cd <repo>/T5-Auto-meter
rm -rf .build && rm -rf dist
tos.py build
```

成功标志：`dist/<project>_<version>/` 下生成 `*_QIO_*.bin / *_UA_*.bin / *_UG_*.bin`。

### 7.3 烧录（参考 Tuya 文档）

```bash
tos.py flash           # 走 USB 串口；按住 BOOT 键加电
```

> 工具链官方文档：<https://www.tuyaopen.ai/zh/docs/about-tuyaopen>

### 7.4 故障排查

| 现象                                  | 排查方向                                                            |
|--------------------------------------|--------------------------------------------------------------------|
| `lv_vendor_init` panic                | `app_default.config` 是否选了 `CONFIG_BOARD_CHOICE_WAVESHARE_T5AI_TOUCH_AMOLED_1_75=y` 与 `CONFIG_LVGL_ENABLE_TP=y` |
| BLE 扫不到 ELM327                     | 适配器是否 BLE 版（不是 SPP 版）；车熄火 ELM327 也不上电；先用手机端"OBD Auto Doctor" 验证可见 |
| 连上但 `0100` 一直无响应               | AT 命令换行用 `\r`（不是 `\r\n`）；MTU 至少 23，过小要分包          |
| 指针抖动                              | 在 `obd_session` 加滑动平均（窗口 4），UI 端再加 `lv_anim` 平滑     |

---

## 8. 安全与编码规范（强约束）

> 摘自仓库根 `always_applied_workspace_rules`，本项目必须严格遵守。

### 8.1 字符串/缓冲区
- 禁止 `strcpy / sprintf / strcat / gets`，统一用 `snprintf` + `sizeof(buf)`
- ELM327 收发 buffer 必须做长度校验，**外部输入（蓝牙数据）一律视为不可信**

### 8.2 内存
- 使用 `tal_malloc / tal_free`，禁用 `malloc / free`
- 释放后置 NULL；分配后立刻 NULL 检查
- 敏感数据（绑定 MAC、KV key）销毁前 `memset` 清零

### 8.3 整数溢出 / 系统时钟
- `tal_system_get_millisecond()` 是 32 bit，按 `(now - prev)` 无符号差值用，避免直接比较
- PID 报文长度、累加器在 `+=` 前先校验上界

### 8.4 日志
- 不允许 `PR_DEBUG` 打印 BLE MAC 全量；用 `:XX:XX` 末两段脱敏
- 不允许打印 OBD 完整原始字节流到默认日志（仅在 `CONFIG_OBD_VERBOSE` 下输出）
- 日志单行 ≤ 256 字节

### 8.5 并发
- `metric_bus` 写操作必须持锁
- BLE 回调上下文不允许做长操作；用 `tal_workq` 转发

### 8.6 函数注释
- 每个函数（含 static）必须有 Doxygen `@brief / @param / @return`
- 返回 `OPERATE_RET` 的函数必须 `@return OPRT_OK on success, error code on failure`

---

## 9. 配置约定

### 9.1 `app_default.config` 起手式（参考 `01_Factory_Firmware/app_default.config`）

```
CONFIG_BOARD_CHOICE_T5AI=y
CONFIG_BOARD_CHOICE_WAVESHARE_T5AI_TOUCH_AMOLED_1_75=y
CONFIG_ENABLE_LIBLVGL=y
CONFIG_LVGL_ENABLE_TP=y
CONFIG_LV_FONT_MONTSERRAT_22=y
CONFIG_LV_FONT_MONTSERRAT_48=y
CONFIG_LV_FONT_MONTSERRAT_80=y
CONFIG_BUTTON_NAME="key_btn"
CONFIG_BUTTON_NAME_2="pwr_btn"
CONFIG_BUTTION_STACK_SIZE=4096
CONFIG_ENABLE_MBEDTLS_SSL_MAX_CONTENT_LEN=4096
```

### 9.2 引脚总表（`include/app_config.h`，单一事实源）

```c
/* Buttons */
#define APP_PIN_BTN_PWR     TUYA_GPIO_NUM_18
#define APP_PIN_BTN_KEY     TUYA_GPIO_NUM_12
#define APP_PIN_PWR_EN      TUYA_GPIO_NUM_19

/* I2C0: TP + QMI8658 */
#define APP_I2C_PORT        TUYA_I2C_NUM_0
#define APP_PIN_I2C_SCL     TUYA_GPIO_NUM_20
#define APP_PIN_I2C_SDA     TUYA_GPIO_NUM_21

/* QMI8658 */
#define APP_QMI8658_ADDR    0x6B

/* Battery */
#define APP_PIN_BAT_CHRG    TUYA_GPIO_NUM_30
#define APP_BAT_ADC_CH      15
#define APP_BAT_ADC_RATIO   (2.51f / 0.51f)
```

---

## 10. 给 AI 的工作流程指令

每次接到本项目任务，按下面步骤走，不要跳：

1. **读本文（AGENT.md）+ 用户最新请求**，对齐目标。
2. 列 TodoWrite，3 ~ 5 项，含验证项。
3. 改代码前，**先读现有相关文件**（不要凭印象改）。
4. 引用既有实现优先于自己造（`01_Factory_Firmware/src/{dev_config,qmi8658,wifi_ble}` 是首选 reference）。
5. 改完代码后：
   - `ReadLints` 看新引入的报错
   - 提示用户去 `~/Project/TuyaOpen` 跑 `. ./export.sh`，再到 `T5-Auto-meter` 跑 `rm -rf .build && rm -rf dist && tos.py build`
6. **提交规则**：
   - 用户没明说"提交"前不要 `git commit`
   - commit message 用 `feat(t5-auto-meter): xxx` / `fix(t5-auto-meter): xxx` 风格
   - **`.build/`、`dist/`、`CMakeCache.txt` 等产物**已在 `.gitignore`，确认不进版本控制
7. **不允许的事**：
   - 关闭 LVGL 的 LVGL_OS 锁
   - 在 BLE 回调里 `lv_*` 直接画图
   - 在源码或 KV seed 里硬编码 OBD-II 厂商私有 PID 不带文档来源
   - 用 `printf` 代替 `PR_*`
   - 用 `malloc / free`

---

## 11. 已知未决问题（开发中要持续讨论）

- [ ] 油压 PID：标准 OBD-II 不提供，是否要走 Mode 22 厂商扩展？需要为不同 OEM 配置不同 PID 表。**首版策略：UI 上显示 `N/A` 并在菜单中标灰。**
- [ ] G 值表盘是双轴还是单纵轴？纵向加速更直观（刹车/加速），但弯道时会丢横向 G。**首版做"圆点漂移"风格的二维呈现。**
- [ ] 多 ELM327 设备扫描时如何选？**首版策略**：扫描列表 + 用户在菜单里手动选；选定后存 KV，下次自动连。
- [ ] 屏幕烧屏（AMOLED）：常显高亮指针存在风险。**首版策略**：5 分钟无 OBD 数据自动进低亮"屏保"模式（指针缓慢扫动）。

---

## 12. 关键参考资料

- TuyaOpen 编译工具链：<https://www.tuyaopen.ai/zh/docs/about-tuyaopen>
- Waveshare 开发板介绍：<https://developer.tuya.com/cn/docs/iot-device-dev/T5-E1-IPEX-development-board?id=Ke9xehig1cabj>
- OBD-II Mode 01 PID 表：<https://en.wikipedia.org/wiki/OBD-II_PIDs>
- 仓库内参考实现：
  - `~/Project/YuYanDev/01_Factory_Firmware/src/{dev_config,qmi8658,wifi_ble}/`
  - `~/Project/TuyaOpen/boards/T5AI/WAVESHARE_T5AI_TOUCH_AMOLED_1_75/board_com_api.c`
  - `~/Project/TuyaOpen/apps/tuya.ai/your_chat_bot/` (圆屏 LVGL 主题、菜单交互)
  - `~/Project/TuyaOpen/examples/ble/ble_central/` (BLE Central 扫描骨架)

---

**最后更新**：项目骨架完成，编译通过（详见 §13）。后续每个里程碑请追加 `## 13. 变更日志` 段，倒序记录。

---

## 13. 变更日志

### 2026-04-27 · 首版骨架完成（feature/t5-auto-meter）

完整目录树（已编译通过 `tos.py build`）：

```
T5-Auto-meter/
├── AGENT.md                     ← 本文件
├── .gitignore                   ← 排除 .build / dist / CMake / Ninja 产物
├── CMakeLists.txt               ← 自动收集 src/ + 子目录
├── app_default.config           ← T5AI + Waveshare 1.75 + LVGL v9
├── include/
│   └── app_config.h             ← 引脚 / 量程 / 周期 单一事实源
└── src/
    ├── tuya_main.c              ← 应用入口；启动 task 串起所有子系统
    ├── app/
    │   ├── app_kv.{h,c}         ← APP_PREFS_T 持久化（mock_enabled、亮度、绑定 MAC、仪表使能位图）
    │   ├── app_metric.{h,c}     ← metric_bus（带锁）+ 数据源标记（OBD/MOCK/IMU）
    │   └── app_mock.{h,c}       ← Mock 任务，按真实量程生成正弦化随机数据
    ├── board/
    │   ├── board_btn.{h,c}      ← 双按键事件代理（短按 / 双击 / 1.2s 长按 / 3s 长按）
    │   ├── board_io.{h,c}       ← I2C 互斥 + 充电检测 + 电池 ADC
    │   └── board_pwr.{h,c}      ← PWR_EN 自锁 + 关机
    ├── sensor/
    │   ├── qmi8658.{h,c}        ← 直连寄存器读出三轴
    │   └── sensor_imu.{h,c}     ← 取样 + 倾角积分 + 写入 metric_bus
    ├── obd/
    │   ├── elm327_ble.{h,c}     ← BLE Central 扫描 / 连接 / GATT 透传 / ELM AT 行解析
    │   ├── ble_compat.c         ← 平台缺失 TKL 中心角色函数的弱兜底实现（详见下文）
    │   ├── obd_pid.{h,c}        ← Mode 01 PID 解析表（0x05/0x0F/0x2F/0x42/0x5C/0x70）
    │   └── obd_session.{h,c}    ← 会话状态机：SCAN→LINKED→READY→LINK_LOST，AT 初始化 + PID 轮询
    └── ui/
        ├── ui_theme.h           ← 颜色 / 角度常量
        ├── ui_gauge.{h,c}       ← 复用型圆形指针仪表（lv_scale + lv_line 指针 + 扫针动画）
        ├── ui.{h,c}             ← 顶层 UI 状态机 + 菜单 + 蓝牙等待覆盖层
```

**UI 状态机**（`UI_STATE_E`）：

```
BOOT_SWEEP   ──→ WAIT_LINK   (mock 关 & OBD 未就绪)
             ──→ MAIN        (mock 开 / OBD 已就绪)
WAIT_LINK    ──→ MAIN        (OBD READY 或 mock toggle 上)
MAIN         ──→ WAIT_LINK   (LINK_LOST 且 mock 关)
MAIN         ⇄ MENU         (PWR 长按 1.2s)
```

**蓝牙连接过渡动画**：

1. `ui_init()` 创建首个仪表后立即调 `ui_gauge_sweep()` 做 1.1s 全量程扫针（`lv_anim_path_ease_in_out`）。
2. 扫针结束（refresh timer 检测）后，如未连上 OBD 则覆盖 `s_overlay`（半透明遮罩 + `lv_spinner` + 文案 "Connecting OBD / Searching for ELM327 BLE…"）。
3. `obd_session` 状态机进入 `READY` 时通过 `ui_on_obd_state` 把 UI 切到 `MAIN`，覆盖层淡出。
4. `MAIN` 入帧第一次写指针时使用 3× 慢速 `UI_NEEDLE_SLEW_MS`（900ms）让指针**平滑滑到真实值**；之后回到 300ms 的常规滑变。

**菜单项**（`UI_MENU_E`）：

| 项 | 行为（PWR 短按激活） | KEY 短按 | 备注 |
|----|---------------------|----------|------|
| Mock Mode    | 切换 `prefs.mock_enabled`，启停 mock task     | 移动光标 | 切到 ON 立即把 WAIT_LINK 推进 MAIN |
| Brightness   | 步进 +20%（100→20 回卷）                      | 移动光标 | 持久化到 KV |
| Cycle Gauges | 占位（首版未实现逐项勾选）                    | 移动光标 | 后续接 `app_kv_set_gauge_enabled` |
| Forget Adapter | 清除 `bound_addr` 并 `obd_session_rescan()` | 移动光标 | |
| Back         | 回到 MAIN                                     | 移动光标 | |

**按键映射**（与 §5.3 一致）：

| 按键 | 短按 (single) | 长按 1.2s   | 长按 3s   |
|------|---------------|-------------|-----------|
| KEY  | MAIN: 下一仪表 / MENU: 光标下移 | (保留)      | (保留)    |
| PWR  | MENU: 激活当前项               | 打开/关闭菜单 | `board_pwr_shutdown()` |

### 13.1 重要说明：T5AI 平台 BLE Central 受限

构建期发现：T5AI 平台编译进固件的 `libtal_bluetooth.a` **不包含**以下 GATT 客户端符号（NimBLE 中通过 `TY_HS_BLE_ROLE_CENTRAL` 宏屏蔽，Kconfig 默认 `0`）：

- `tkl_ble_gap_connect`
- `tkl_ble_gattc_all_service_discovery`
- `tkl_ble_gattc_all_char_discovery`
- `tkl_ble_gattc_char_desc_discovery`
- `tkl_ble_gattc_write` / `tkl_ble_gattc_write_without_rsp`
- `tkl_ble_gattc_read`

但**扫描相关**（`tkl_ble_gap_scan_start/stop` + GAP/GATT 回调注册 + `tkl_ble_gattc_exchange_mtu_request` + `tkl_ble_gap_disconnect`）是有的。

**应对策略**：

`src/obd/ble_compat.c` 用 `__attribute__((weak))` 给上述缺失符号提供"返回 `OPRT_NOT_SUPPORTED`"的兜底实现。这样：

- 工程能链接通过、能烧录、能跑；
- 真机上扫描会找到 ELM327 但**无法连接**（停留在 `WAIT_LINK`，覆盖层一直转）；
- 用户从菜单打开 `Mock Mode` 即可让指针走起来用于交互验证；
- 一旦底层 SDK 在某个版本里把强符号补回，弱兜底自动失效，不需要改这层。

> **TODO**: 当 TuyaOpen 正式开放 T5AI 的 BLE Central 后，把 `ble_compat.c` 标 `#warning`，确认无引用后删除。

### 13.2 编译验证

环境：

```bash
cd ~/Project/TuyaOpen && . ./export.sh
cd ~/Project/YuYanDev/TuyaOpen-Community-Projects/T5-Auto-meter
rm -rf .build dist && tos.py build
```

产物：

- `dist/T5-Auto-meter_0.1.0/T5-Auto-meter_QIO_0.1.0.bin`（QIO 全量包）
- `.build/bin/T5-Auto-meter_UA_0.1.0.bin` + `_UG_0.1.0.bin`（OTA 包）

固件大小：CP FLASH 84%、AP FLASH 35%、AP RAM 20%（裕量充足）。

### 13.3 指针体验优化（卡顿 / 中心点 / 美化）

> 用户反馈：「指针很卡，而且线的另一头不在中心，另外指针样式需要做得好看点，不要一条线，是一个远端收窄圆润的」

针对三类问题都在 `src/ui/ui_gauge.{h,c}` 和 `src/ui/ui.c` 里做了改造：

#### A. 指针样式：渐窄圆润的水滴形
- 不再用 `lv_line` + `lv_scale_set_line_needle_value`（线宽固定，无法收窄；末端依赖 lv_scale 的几何，容易偏离中心）。
- 改成一个**透明 overlay obj**，自己处理 `LV_EVENT_DRAW_MAIN`，在 layer 上画两个三角形 + 末端圆形 cap：
  - 近端（pivot）宽度 14 px、远端 4 px，构成 trapezoid（共享对角线 p0→p2 防 1 px 缝隙）
  - 远端再叠一个 `LV_RADIUS_CIRCLE` 的小圆（半径 3）做圆润收尾
  - 颜色统一使用 `UI_COLOR_PRIMARY`，hub（半径 18 圆 + 主色边框）盖住 6 px 的小尾巴形成"水滴"剪影

#### B. 中心点对齐：pivot = obj 几何中心
- overlay obj 用 `lv_obj_center` 居中到屏幕，绘制时取 `lv_obj_get_coords` 的几何中心 `(cx, cy)` 作为旋转原点。
- 指针局部坐标系内 base 在 x=0、tip 在 x=LEN，旋转矩阵直接作用，**数学上保证另一端正好落在屏幕中心**，不再依赖 `lv_scale` 内部坐标转换。

#### C. 卡顿来源与修复
| 问题 | 原因 | 修复 |
|------|------|------|
| Refresh tick 100 ms 重启 300 ms ease_in_out | 每次只走完慢启动段就被打断 | 追踪期改 `lv_anim_path_linear` + 时长降到 180 ms |
| 每次 set_value 都重启动画 | 微小抖动也会让指针抖 | 加 0.8° 角度死区（`GAUGE_DEADBAND_X10 = 8`） |
| 动画驱动 value→angle，每帧重新换算 | 多余浮点/除法 | 直接动画 `needle_angle_x10`，绘制只查 `lv_trigo_sin` |
| `lv_obj_invalidate` 触发整屏重绘 | needle 之前是 `lv_line`，宽度受限但 overlay 跨整屏 | 把 needle obj 缩到 ≈380×380 紧贴指针包络 |
| Boot sweep 100 ms 后被 refresh tick 跳出 | 进 MAIN 后立即 set_value 取消 sweep | `s_boot_elapsed_ms` 守 `UI_BOOT_HOLD_MS = APP_BOOT_SWEEP_MS + 100`，扫表跑完才换状态 |
| KEY 切仪表 destroy+create 闪屏 | 整个 lv_arc/lv_scale/labels 全部重建 | 新增 `ui_gauge_set_def`，原地改量程/标题/刻度 |

核心常量（`ui_gauge.c` 顶部，便于后续微调）：

```c
#define GAUGE_NEEDLE_LEN       175   /* pivot→tip */
#define GAUGE_NEEDLE_BACK      6     /* 隐藏在 hub 下的小尾巴 */
#define GAUGE_NEEDLE_BASE_W    14    /* 近端宽 */
#define GAUGE_NEEDLE_TIP_W     4     /* 远端宽（cap 之前） */
#define GAUGE_NEEDLE_CAP_R     3     /* 圆润末端的半径 */
#define GAUGE_DEADBAND_X10     8     /* 0.8° 角度死区 */
#define GAUGE_TRACK_DUR_MS     180   /* 默认追踪时长 */
#define GAUGE_EASE_THRESH_MS   500   /* ≥ 此时长走 ease_in_out（用于扫表/首帧） */
```

`ui.c` 配套常量：

```c
#define UI_NEEDLE_TRACK_MS  180   /* 持续刷新时的滑变 */
#define UI_NEEDLE_INTRO_MS  900   /* 蓝牙连上 / 切仪表 后第一帧的入场动画 */
#define UI_BOOT_HOLD_MS     (UI_BOOT_SWEEP_MS + 100)  /* 守住 boot sweep */
```

> **再编译验证**：`rm -rf .build dist && tos.py build` 通过，固件占用未明显变化（仍 CP 84% / AP 35%）。

### 13.4 WAIT_LINK 状态下的可达性（菜单 / 切表）

> 用户反馈：「开机后会卡在连接状态，但是这时候可以按键切换到菜单界面来连接蓝牙，以及启用 mock 数据，另外卡在连接状态后也不能切换仪表了」

由于 T5AI 平台没有 BLE Central（见 §13.1），开机扫表后会一直停留在 `WAIT_LINK` + spinner 覆盖层。这一轮把 UI 状态机改成"任何 live 状态都能开菜单 / 切表 / 切 mock"：

| 行为 | 修复前 | 修复后 |
|------|--------|--------|
| `PWR` 长按打开菜单（在 WAIT_LINK） | 能打开，但关闭后状态错乱（强制 → MAIN，但覆盖层仍可见） | 关闭时 `__compute_live_state()` 重新选 MAIN/WAIT_LINK，覆盖层联动 |
| `KEY` 切仪表（在 WAIT_LINK） | 直接 return，不响应 | 允许，调用 `ui_gauge_set_def` 原地切量程/标题/刻度，并把当前仪表 idx 持久化 |
| 在菜单中切 Mock Mode | `ui_on_mock_changed` 检查 `s_state==WAIT_LINK/MAIN`，菜单态下两个分支都不走 → mock pref 改了但 UI 不联动 | 菜单态下只更新 mock 任务；关菜单时自动落到正确 live 状态（mock=on→MAIN，mock=off+无链接→WAIT_LINK） |
| 在菜单中收到 OBD 状态变化 | `ui_on_obd_state` 直接改 `s_state` → 用户被踢出菜单 | 菜单态下只更新 `s_obd_state`，关菜单时再决定 |
| Overlay 文案 | 只写"Searching for ELM327 BLE…" | 加提示「`Hold PWR for menu · KEY: switch gauge`」让用户知道有出口 |

新增/修改的关键函数：

- `__compute_live_state()`：单一信源——`mock_on || obd==READY ? MAIN : WAIT_LINK`
- `__enter_live_state(target)`：切 live 状态 + 覆盖层联动 + 进 MAIN 时重置 `s_first_value_after_link` 触发 INTRO 动画
- `ui_toggle_menu()`：开菜单先隐藏覆盖层；关菜单走 `__enter_live_state(__compute_live_state())`，不再硬编码 MAIN
- `ui_show_next_gauge()`：放行 WAIT_LINK；在 WAIT_LINK 下 `animate_in=FALSE`（无数据，纯换 def）
- `ui_on_mock_changed()` / `ui_on_obd_state()`：菜单态下短路返回；live 态下交给 `__enter_live_state(__compute_live_state())`
- `__refresh_timer_cb`：BOOT_SWEEP/WAIT_LINK 都改用 `__enter_live_state` 提升，保证逻辑一致；MENU 态显式跳过 gauge 刷新

> 编译验证：`rm -rf .build dist && tos.py build` 通过，固件占用无明显变化。

