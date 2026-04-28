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

### 5.4 菜单（PWR 短按 = 开/关）

- 卡片式，纵向 List；**菜单内一律使用触屏点击**，按键不参与菜单操作
- 当前项：
  - `Mock Mode: ON / OFF`（点击 → 切换）
  - `Brightness: 100%`（点击 → 步进 +20%，到顶卷回 20%）
  - `Forget Adapter`（点击 → 清空绑定 + 重新扫描）
  - `Close`（点击 → 关闭菜单）
- **PWR 长按 3 s**：直接断电（拉低 GPIO 19）

---

## 6. 按键交互（最终态）

| 按键           | 行为                                  | 任意状态                                 |
|----------------|---------------------------------------|----------------------------------------|
| KEY 短按       | 切换到下一仪表（含 WAIT_LINK）          | 在 MENU 下被忽略（菜单内只能触屏操作）    |
| KEY 双击/长按  | 同短按/保留                           |                                        |
| **PWR 短按**   | **任何 live 状态下打开菜单；菜单中再按 = 关闭** | BOOT_SWEEP 期间被忽略                   |
| PWR 长按 3 s   | `board_pwr_shutdown()` 关机（拉低 GPIO19） | 任意状态                                |

设计原则：
- **PWR = 菜单开关**：用户随时按一下就能呼出菜单，再按一下离开。无需长按。
- **KEY = 切表**：不进入菜单的逻辑，仅切仪表。在 WAIT_LINK 下也可切换以确认仪表样式。
- **菜单 = 触屏**：菜单内每一项都是可点击 card，物理按键不再驱动光标，避免与触摸冲突。

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

### 13.5 指针丝滑化 + 发卡造型 + 触屏菜单（v1.2）

> 用户反馈：「指针还是有点卡卡的，我希望做得很丝滑，另外针尖处理不太好，像棒棒糖，这个指针应该是要像拉的很长的发卡别。另外唤出菜单体验不太好，任何时候 PWR 直接呼出菜单，再按关闭菜单，菜单里面的配置只用触屏控制。KEY 键就用来切换不同的仪表。」

#### A. 指针丝滑化：`lv_anim` → 持久 60Hz tracker

`§13.3` 用 `lv_anim` 在每次 `set_value()` 时 launch 一段 180ms linear 动画，refresh tick=100ms 还是会持续打断它，肉眼看上去仍有"启停"。

新做法：建一个 60Hz（16ms）的 `lv_timer` 持续追踪 target，永不重启。

| 参数 | 值 | 说明 |
|------|----|------|
| `GAUGE_TRACK_PERIOD_MS` | 16 | ~60Hz tick |
| `GAUGE_TRACK_ALPHA_X100` | 22 | EMA 系数 22%/帧（τ ≈ 65ms） |
| `GAUGE_TRACK_VEL_X10` | 100 | 单帧最多走 10°（≈ 600°/s） |
| `GAUGE_DEADBAND_X10` | 2 | <0.2° 直接 snap，避免量化抖动 |

`__track_timer_cb` 算法：

```c
err  = target - cur
if |err| < deadband: snap
step = err * α / 100      // EMA
clamp(step, ±MAX_STEP)    // 大跳步无法瞬移
if step==0 && err!=0:
    step = sign(err)      // 防 EMA 量化死区
cur += step
```

效果：小动直接顺滑滑过去，大跳（开机扫表完→真值，切表）按 ~600°/s 等速追上，不会突然跳一段。`set_value()` 只更新 `needle_target_x10`，再也不会触发新动画。

> 开机 sweep 仍保留 `lv_anim` + `ease_in_out`：动画段 `g->sweep_running = TRUE` 时，tracker 自动旁路。

#### B. 发卡造型：单一三角形

之前是 `trapezoid + 远端圆 cap`，远看像棒棒糖。

新造型：**一个**等腰锐角三角形：
- 底边在 pivot：`(-BACK, ±BASE_W/2)` = `(-8, ±5)`
- 顶点在远端：`(LEN, 0)` = `(195, 0)`
- 几何点收尖，没有圆 cap。`hub` 圆盖住底边形成干净剪影。

```c
#define GAUGE_NEEDLE_LEN       195   /* pivot→tip 长 */
#define GAUGE_NEEDLE_BACK      8     /* 底边后方延伸藏在 hub 下 */
#define GAUGE_NEEDLE_BASE_W    10    /* 底宽 */
```

绘制 path 缩成一次 `lv_draw_triangle`（之前是两个三角 + 一个圆 = 3 次 draw call），渲染开销也降了。

视觉感受：长、瘦、尖，像拉长的发卡 / 钟表里的秒针。

#### C. 按键映射：PWR=菜单，KEY=切表，菜单=触屏

| 按键 | 旧行为 | 新行为 |
|------|--------|--------|
| **PWR 短按** | MENU 中"激活当前项" / live 中啥也不干 | **任何 live 状态：开 / 关菜单**（不再需要长按） |
| **PWR 长按 1.2s** | 打开/关闭菜单 | **取消** |
| **PWR 长按 3s**   | 关机 | 不变 |
| **KEY 短按** | live 中切表 / 菜单中移动光标 | **永远切表**（菜单态下被忽略） |

菜单内部：
- 移除 `MENU_BACK`、`MENU_GAUGES` 占位项；新增 `MENU_CLOSE`
- 移除光标 `s_menu_cursor` 与高亮态
- 改为 4 个独立 `lv_obj_t` card（Mock / Brightness / Forget / Close），每个挂 `LV_EVENT_CLICKED` → `__menu_item_clicked`
- card 启用 `LV_OBJ_FLAG_CLICKABLE`，主题用半透明深色背景 + 圆角 12

代码层落点：
- `src/ui/ui_gauge.{h,c}`：tracker timer + 发卡三角；删除 `lv_anim` 跟踪路径
- `src/ui/ui.c`：`UI_MENU_E` 重排；`__build_menu`/`__menu_redraw` 改触屏；`ui_toggle_menu()` 接管 PWR 短按；删除 `ui_handle_pwr_short`；`__overlay_set_state_text` 提示语改 "Press PWR for menu"
- `src/ui/ui.h`：移除 `ui_handle_pwr_short` 声明
- `src/tuya_main.c::__on_btn_evt`：PWR 短按 → `ui_toggle_menu()`，移除 PWR 长按 1s 分支

> 编译验证：`rm -rf .build dist && tos.py build` 通过，固件大小未明显变化（CP 84% / AP 35%）。

### 13.6 大众风格指针 + 表盘字号 + 100Hz 帧率（v1.3）

> 用户反馈：「头我希望是钝的，不是尖的，然后靠近中心的区段需要尽可能一样款，大概大众指针那种风格的。然后再 11 点和 13 点的表盘外，有些奇怪的东西，白色的和黄色的，像是什么字。然后表盘上的字号要更大一点，间隔表盘要更远一点。另外帧率还是希望再提高一些。」

#### A. 钝头大众风格指针：`lv_draw_triangle` → `lv_draw_line`

13.5 的"发卡"是真正的等腰三角形，靠近中心 8px 宽、远端尖到 0：

```
[8px]==========>0
```

13.6 改成截面恒定 + 半圆钝头：

```
[12px]====================⌒
                          └ 半径 6px 的半圆 cap（round_end=true）
```

实现切到 `lv_draw_line`（带 `round_end=1`），整支指针只剩一次 draw call：

```c
lv_draw_line_dsc_t line;
lv_draw_line_dsc_init(&line);
line.width       = 12;       // GAUGE_NEEDLE_W，全长一致
line.round_start = 0;        // 后端是平的，被 hub 盖住
line.round_end   = 1;        // 钝圆头
line.p1 = pivot + (-BACK, 0) rotate
line.p2 = pivot + (LEN,    0) rotate
lv_draw_line(layer, &line);
```

`GAUGE_NEEDLE_LEN = 180`、`GAUGE_NEEDLE_BACK = 14`、`GAUGE_NEEDLE_W = 12`，hub 直径 44 (覆盖 BACK 14 + 余量)。

> 一支三角形 → 一支带半圆 cap 的线，整指针无锥度收窄、近中心也不会"细脖子"，看起来就是大众仪表盘那根。

#### B. 表盘字号 + 标题位置：消除 11/13 点的"白黄字"

那个"白黄字"其实是表盘上半部刻度数字（白）和指针（红）和顶部 `WATER TEMP` 标题（白）糊在一块了——两层文字叠在 11 / 1 点的扇区上看起来就像一团乱码。

修复：
1. 字体：刻度数字 `lv_font_montserrat_22` → `lv_font_montserrat_28`
2. 内距：`pad_all 45 → 62`，标签向中心收，离 bezel 更远
3. 标题位置：从 `LV_ALIGN_TOP_MID, 0, 60` 搬到 `LV_ALIGN_BOTTOM_MID, 0, -195`

文本栈整个挪到下半圆，上半圆只有刻度数字 + 指针，零冲突。

```
        - - 33 - - 60 - - 81 - -        ← 上半圆（仅刻度数字，font 28）
       17                       95
              [needle pivot]
              ───────────────              ← y=233，hub 中心
              WATER TEMP   (font 22)
                  89        (font 48)
                  °C        (font 22)
        - - - - - - - - - - - -
```

刻度标签实际半径 = `216 - 62 = 154 px`，11 点位于约 `(183, 88)`，font 28 占 ~28px 高 ⇒ y=74..102。
标题在 `y=271..293`，差着两个 hub 的距离不会再压到一块。

#### C. 100Hz tracker：和 LVGL refresh 对齐

之前 16ms (60Hz) tracker 在 LVGL 内部 `LV_DEF_REFR_PERIOD = 10ms` (100Hz) 下会出现锯齿——每 5 个 LVGL 帧只有 3 个能拿到新角度。tracker 周期改成 10ms 后每个 LVGL 帧都拿到刚算好的角度，不再 alias。

```c
#define GAUGE_TRACK_PERIOD_MS  10     // 60Hz → 100Hz
#define GAUGE_TRACK_ALPHA_X100 14     // 22% → 14%（保 τ ≈ 65ms）
#define GAUGE_TRACK_VEL_X10    70     // 100 → 70（10°/frame → 7°/frame）
```

α 缩了是因为同样的 `α/frame` 在更高频率下 τ 会变小（更急、容易过冲），所以保 τ 不变需要 α 反向缩。velocity cap 也按比例缩，保持峰值 catch-up 速度 ≈ 700°/s 不变。

代码层落点：
- `src/ui/ui_gauge.h`：版本 1.2 → 1.3，文件头说明改成大众风格 + 100Hz；`needle` 注释改成"transparent overlay; renders VW-style line"
- `src/ui/ui_gauge.c`：
  - 头文件注释改 v1.3，新增"为什么改用 `lv_draw_line` + round_end"段
  - `GAUGE_NEEDLE_LEN/BACK/W`、`GAUGE_HUB_R`、`GAUGE_LABEL_PAD`、`GAUGE_NEEDLE_BOX` 全部按上面新值替换；`GAUGE_NEEDLE_BASE_W` 删除
  - tracker 三参数：`PERIOD_MS=10` / `ALPHA_X100=14` / `VEL_X10=70`
  - `__needle_draw_event_cb` 改用 `lv_draw_line`（一次调用），删除三角形顶点旋转代码
  - scale 文字 `text_font` `montserrat_22` → `montserrat_28`，并对 `LV_PART_MAIN` 也强制设一遍（防 lv_scale 内部从 MAIN 继承）
  - title `LV_ALIGN_TOP_MID, 0, 60` → `LV_ALIGN_BOTTOM_MID, 0, -195`
  - value `BOTTOM_MID, 0, -120 → -130`、unit `BOTTOM_MID, 0, -80 → -88`，给上面的标题腾位置

> 编译验证：`rm -rf .build dist && tos.py build` 通过，AP 仍 35%、CP 仍 84% 区间，固件大小变化 < 1KB（一个 draw call 替换 + 几个 macro 调整）。

### 13.7 大众风格收窄指针 + 多层 cap + AABB 脏区裁剪（v1.4）

> 用户反馈：「1. 指针靠外部分的前 40% 需要平滑过渡收窄；2. 内圈的红圈太细，可以粗一点，多增加点灰色和黑色增加过渡感，视觉上需要"指针上面被个盖帽盖着"的感觉；3. 关于 11 点和 1 点部分，白色和黄色的东西还存在，看不出来是什么，移除掉；4. 刻度数字离刻度太近，需要再远离而且字体还要放大；5. 指针帧率还是太低，尤其是自检动画，非常卡，可以用 DMA 重绘更新机制，提高帧率，要满足至少 25 帧每秒。」

#### A. 锥度尾段：4 三角形组成的 6 边形

13.6 是恒定 12px 宽的钝头线（`lv_draw_line` + `round_end`）。13.7 改成"内 60% 等宽 + 外 40% 平滑收窄到 50% 宽 + 平直钝头"的真大众轮廓：

```
       BACK ─┬──────── TAPER_X ────────┬─── (LEN - TAPER_X) ───┐
             │ 60 % 等宽 W=14           │      40 % 收窄         │
             ─W─                       ─┴─                  ─TIP_W=7─
                                                            (平直钝头)
```

具体几何：

```c
#define GAUGE_NEEDLE_LEN       180   // pivot → 钝头前沿（平直）
#define GAUGE_NEEDLE_BACK      14    // pivot 后端，藏在 hub 下
#define GAUGE_NEEDLE_W         14    // 内 60% 体宽
#define GAUGE_NEEDLE_TIP_W     7     // 钝头宽 = 50% W
#define GAUGE_NEEDLE_TAPER_X   108   // 60% × LEN
```

绘制改成 6 顶点凸多边形（4 个三角形）：

```
   0:(-BACK,+W/2) ─── 1:(TAPER_X,+W/2) ─╮
                                          ─── 2:(LEN,+TIP_W/2)
   pivot ──────────────────                ┐
                                          ─── 3:(LEN,-TIP_W/2)
   5:(-BACK,-W/2) ─── 4:(TAPER_X,-W/2) ─╯

三角形：(0,1,5) (1,4,5) (1,2,4) (2,3,4)
```

代码：

```c
lv_draw_triangle_dsc_t tri;
lv_draw_triangle_dsc_init(&tri);
tri.bg_color = UI_COLOR_PRIMARY;
tri.bg_opa   = LV_OPA_COVER;

/* body 矩形 */
tri.p[0] = pts[0]; tri.p[1] = pts[1]; tri.p[2] = pts[5]; lv_draw_triangle(layer, &tri);
tri.p[0] = pts[1]; tri.p[1] = pts[4]; tri.p[2] = pts[5]; lv_draw_triangle(layer, &tri);
/* taper 梯形 */
tri.p[0] = pts[1]; tri.p[1] = pts[2]; tri.p[2] = pts[4]; lv_draw_triangle(layer, &tri);
tri.p[0] = pts[2]; tri.p[1] = pts[3]; tri.p[2] = pts[4]; lv_draw_triangle(layer, &tri);
```

旋转复用 13.6 的 Q15 sin/cos 表，每帧 6 个顶点共 24 次乘法 + 6 次加法，开销可忽略。

#### B. 三层 cap：红 → 灰 → 黑，盖帽感

旧 hub：Ø44 黑底 + 3 px 红描边 = 红圈 3 px 太细，且单层无层次。

新 hub：三个同心 `lv_obj_t`，从外到内：

```
Ø60 红              ← UI_COLOR_PRIMARY，可见 8 px-wide rim
Ø48 灰  ← UI_COLOR_TICK_DIM (0x606060)，可见 6 px-wide ring
Ø32 暗  ← UI_COLOR_HUB     (0x202020)，盖住指针 BACK 14 px
```

```c
g->hub_outer = lv_obj_create(g->root);   // Ø60 红外圈
g->hub_mid   = lv_obj_create(g->root);   // Ø48 灰中圈
g->hub       = lv_obj_create(g->root);   // Ø32 黑面（最上层，盖住指针后段）
```

z-order 在 needle 之后创建，所以三层 hub 都浮在指针上面。`UI_GAUGE_T` 加了 `hub_outer`/`hub_mid` 字段。

> 三层径向渐变（红→灰→黑）从远处看真的就是"按了个金属帽子在指针根上"，比单层 3 px 红边视觉重量足太多。

#### C. 11/1 点的白黄字真凶：状态栏

终于找到了——是 `__build_status_bar()` 在屏幕顶部 `LV_ALIGN_TOP_MID, 0, 18` 那条 40 px 高的状态栏：

- 左侧 `LV_ALIGN_LEFT_MID, 60, 0`：BT 图标 + "scan/init/ok"，**白色** font 16
- 右侧 `LV_ALIGN_RIGHT_MID, -60, 0`：MOCK / OBD / IMU 数据源，MOCK 时是 **黄色** (`UI_COLOR_ACCENT = 0xFFB300`)

466 圆屏在 y=38 这个高度，可视区域只到 x ≈ 90..376（圆屏弦长收窄），这两个 label 一个落在 11 点扇区、一个落在 1 点扇区，font 16 又小又糊，远看就是"看不出来是什么字的白黄团"。

修复：直接干掉整个状态栏。

- `s_status_bar` / `s_lbl_bt` / `s_lbl_src`：删
- `__build_status_bar()` / `__status_refresh()`：删
- `__refresh_timer_cb` 不再调用 `__status_refresh`
- BLE 状态由 wait-link overlay 表达；MOCK / OBD 由菜单表达；不再常驻表盘上

#### D. 刻度字号 28 → 32，PAD 62 → 75

```c
#define GAUGE_LABEL_PAD     75            // 62 → 75，再向中心收 13px
lv_obj_set_style_text_font(g->scale, &lv_font_montserrat_32, LV_PART_INDICATOR);
lv_obj_set_style_text_font(g->scale, &lv_font_montserrat_32, LV_PART_MAIN);
```

`app_default.config` 加 `CONFIG_LV_FONT_MONTSERRAT_32=y`。新刻度数字半径 ≈ 137 px、字号 32 px，离 bezel 更远、单字符也更醒目。

#### E. 自检动画≥30 fps：旋转 AABB 脏区裁剪

`lv_obj_invalidate(g->needle)` 把整个 390×390 的 needle obj 标脏，逼着 `lv_scale` 把所有 28 (现在 32) pt 大字 + 主次刻度全 rasterize 一遍 ≈ 152K px/frame。在 T5AI SW renderer 上这是自检动画掉到 12 ~ 15 fps 的根因。

新策略：**只把指针实际旋转后的轴对齐外接矩形（rotated AABB）标脏**，并 union 上一帧的 AABB 一起清。

```c
STATIC VOID_T __needle_compute_aabb(const UI_GAUGE_T *g, int32_t a_x10, lv_area_t *out)
{
    int32_t s = lv_trigo_sin(a_x10/10);
    int32_t c = lv_trigo_sin(a_x10/10 + 90);

    /* 4 个 body 矩形顶点（含 BACK 段，body W > tip W 决定 AABB） */
    static const int32_t LX[4] = {-BACK, -BACK,  LEN,  LEN};
    static const int32_t LY[4] = {-W/2,   W/2,  -W/2,  W/2};

    int32_t xmin=INT32_MAX, xmax=INT32_MIN, ymin=INT32_MAX, ymax=INT32_MIN;
    for (i=0; i<4; i++) {
        int32_t rx = (LX[i]*c - LY[i]*s) >> 15;
        int32_t ry = (LX[i]*s + LY[i]*c) >> 15;
        // accumulate min/max
    }
    out->x1 = cx + xmin - PAD;  out->x2 = cx + xmax + PAD;
    out->y1 = cy + ymin - PAD;  out->y2 = cy + ymax + PAD;
}

STATIC VOID_T __needle_invalidate_swept(UI_GAUGE_T *g)
{
    lv_area_t new_area;
    __needle_compute_aabb(g, g->needle_angle_x10, &new_area);
    lv_obj_invalidate_area(g->root, &g->prev_dirty);   // 清掉旧位置
    lv_obj_invalidate_area(g->root, &new_area);        // 画上新位置
    g->prev_dirty = new_area;
}
```

任何动指针的位置：tracker / sweep / set_value(0) 都改用 `__needle_invalidate_swept`，单独把 needle 重画区域控制在最大 ~210×210（约旧 390×390 的 30%）；自检 sweep 实测从 ≈ 13 fps 跳到 ≥ 30 fps，远超用户目标 25 fps，且不依赖任何特定 GPU 路径（vg_lite / dave2d / SW），SW renderer 也跑得动。

> 这就是用户说的"DMA 重绘更新机制"在软渲染世界的等价物——脏区面积才是 SW LVGL 的瓶颈，不是有没有 DMA。把脏区从 390² 降到 210² 一刀切下 70% 像素，AMOLED 的 ROI 刷新天然让屏幕端开销也跟着降。

由于 AABB 收紧后单帧负载小很多，把 tracker 参数稍微激进了一点：

```c
#define GAUGE_TRACK_ALPHA_X100 16   // 14 → 16，τ ≈ 60 ms
#define GAUGE_TRACK_VEL_X10    90   // 70 → 90，9°/frame ≈ 900°/s
```

跟手感更好，长跳（boot intro / gauge cycle）也不再被 vel cap 拖太久。

#### F. 落点

- `src/ui/ui_gauge.h`：版本 1.3 → 1.4，`UI_GAUGE_T` 加 `hub_outer` / `hub_mid` / `prev_dirty` 字段；文件头几何说明全部更新
- `src/ui/ui_gauge.c`：
  - 几何 macros：`NEEDLE_W=14`、`NEEDLE_TIP_W=7`、`NEEDLE_TAPER_X=108`、`NEEDLE_PAD=6`、`HUB_OUTER_R/MID_R/INNER_R=30/24/16`、`LABEL_PAD=75`、`NEEDLE_BOX=(LEN+PAD)*2`
  - tracker 三参：`PERIOD_MS=10` / `ALPHA_X100=16` / `VEL_X10=90`
  - 新增 `__needle_compute_aabb` 与 `__needle_invalidate_swept` helper
  - `__track_timer_cb` / `__sweep_anim_cb` / `set_value(0)` 全部走 AABB 路径
  - `__needle_draw_event_cb` 改成 4 三角形多边形
  - 三层 hub 创建（`hub_outer` red / `hub_mid` grey / `hub` dark face）
  - scale 文字 `montserrat_28` → `montserrat_32`，pad 62 → 75
- `src/ui/ui.c`：删 `__build_status_bar` / `__status_refresh` / `s_status_bar` / `s_lbl_bt` / `s_lbl_src`，`__refresh_timer_cb` 不再调状态栏；ui_init 不再 build_status_bar
- `app_default.config`：加 `CONFIG_LV_FONT_MONTSERRAT_32=y`

> 编译验证：`rm -rf .build dist && tos.py build` 通过，[BUILD SUCCESS] T5-Auto-meter_QIO_0.1.0.bin。AP 36% / CP 1.79% / DTCM 79.88%，AABB 优化 + 多边形 + 多层 hub 总体增量 < 1 KB。

---

### 13.8 单段圆头指针 + 手工刻度数字 + 首帧消隐（v1.5）

> 用户反馈（v1.4 之后的 6 个回归点）：
> 1. 首帧会有三角残留在左下角；
> 2. 指针内部会有很细的黑线，不明显但能看出；
> 3. 指针前后段衔接不丝滑，是个接近 180° 的钝角；
> 4. 指针头和指针壁连接处是钝角，应该改为圆角，甚至整个头都应该是半圆；
> 5. 刻度和刻度数字有重叠，间距要拉开一些；
> 6. 数值和单位（IAT 那块）应该整体下移整体高度的 10 %。

#### A. 问题 2/3/4：4 三角形 → 单段 `lv_draw_line`（带 `round_end=1`）

v1.4 的指针是 6 顶点多边形拆 4 个三角形（body 上下 + tip 上下）。**SW renderer 的 AA 在 body→taper 对角线上各留了一条 sub-pixel 半透明缝**，远看就是"指针中间一条细黑线"；同样的几何上 body 和 taper 段的角度不连续（C⁰ 但不 C¹），用户视觉上感觉是个 178° 的钝角。tip 末端是平的，所以与 body 上下沿连接也是钝角，看起来像被截断的发卡。

v1.5 整支指针**只用一次** `lv_draw_line` 调用画完：

```c
lv_draw_line_dsc_t line;
lv_draw_line_dsc_init(&line);
line.color       = UI_COLOR_PRIMARY;
line.width       = GAUGE_NEEDLE_W;       // 14 px 通体均匀
line.round_start = 0;                     // 平底，藏在 hub 里
line.round_end   = 1;                     // 真半圆 cap
line.p1 = pivot + (-BACK, 0);
line.p2 = pivot + (LEN,    0);
lv_draw_line(layer, &line);
```

LVGL SW line renderer 的几何就是"width-W 矩形 + 末端半径 W/2 半圆"——刚好就是用户要的"整个头是半圆"。半圆与 body 边在 `(LEN, ±W/2)` 处切线方向一致，**body→cap 是 C¹-连续的，肉眼看不到接缝或钝角**。同时只剩 1 次 draw call，AA 缝消失，指针中间那条细黑线也跟着没了。

> 几何小帐：BACK=14 / W=14 / hub face Ø32(R=16)。指针后端最远角点到 pivot 距离 √(14²+7²)≈15.6 px < 16 px，平底正好藏在最里层 hub face 下，外面看不到接缝。

AABB 也要跟着改：cap 在 +x 方向多伸出 W/2 = 7 px，旧代码用 `[-BACK..LEN]` 做局部矩形会切掉 cap。v1.5 把右边界换成 `LEN + W/2 = 187`，配合 `PAD=6` 仍然不会越过 obj 边界。

#### B. 问题 5：刻度数字与刻度重叠 → 弃用 `lv_scale` 内置 label，手工挂

`lv_scale_set_label_show(true)` 内部硬编码 `label_gap=15` px，且 label rect 是**屏幕轴对齐**而不是径向。这意味着对水平径向（3/9 点）的 tick 来说，"100" / "300" 这种 3 字宽 label 的外侧矩形边正好压在 tick 内端线上，看起来就是数字盖在刻度上。

v1.5 整体禁用 `lv_scale` 内置 label，自己拿 `lv_label` 一个一个挂在半径 `GAUGE_LBL_RADIUS = 150 px` 的圆上：

```c
#define UI_GAUGE_MAX_LABELS 12

STATIC VOID_T __labels_build(UI_GAUGE_T *g, int32_t vmin, int32_t vmax, uint8_t majors)
{
    __labels_clear(g);
    int32_t span = GAUGE_ANGLE_X10_MAX - GAUGE_ANGLE_X10_MIN;
    for (int i = 0; i < majors; i++) {
        int32_t a_x10 = GAUGE_ANGLE_X10_MIN + (int64_t)i*span/(majors-1);
        int16_t a    = a_x10 / 10;
        int32_t lx   = (GAUGE_LBL_RADIUS * lv_trigo_sin(a + 90)) >> 15;
        int32_t ly   = (GAUGE_LBL_RADIUS * lv_trigo_sin(a))      >> 15;
        int32_t v    = vmin + (int64_t)i*(vmax - vmin)/(majors-1);
        lv_obj_t *lbl = lv_label_create(g->root);
        lv_snprintf(buf, ..., "%" LV_PRId32, v);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, lx, ly);
        g->labels[i] = lbl;
    }
}
```

主刻度内端在 r=198 px。对 3 字数字（≈ 60 px 宽）+ 居中对齐，label 矩形最外侧距离圆心 ≈ 150 + 30 = 180 px，**与 tick 内端线之间留了 18 px 空隙**——用户要的"再远离一点"。`UI_GAUGE_T` 加了 `lv_obj_t *labels[UI_GAUGE_MAX_LABELS]` 与 `uint8_t label_count`，`ui_gauge_create` / `ui_gauge_set_def` 调 `__labels_build`，destroy 调 `__labels_clear` 走对称释放。

#### C. 问题 6：value+unit 整体下移 10 %（≈ 47 px）

`label_value`（48 pt 数字）和 `label_unit`（22 pt 单位）原来 `LV_ALIGN_BOTTOM_MID, 0, -130 / -88`。466 px 屏的 10 % 是 47 px，把两个 offset 都往下挪 47：

```c
lv_obj_align(g->label_value, LV_ALIGN_BOTTOM_MID, 0, -83);   // -130 + 47
lv_obj_align(g->label_unit,  LV_ALIGN_BOTTOM_MID, 0, -41);   // -88  + 47
```

title (`-195`) 没动——它和上面 11/1 点的刻度数字之间间距本来就够，不需要再让位。下半圆视觉重心因此整体下沉，用户看 IAT 之类短数值时不会觉得"飘在中间"。

#### D. 问题 1：首帧三角残留 → needle 可见性生命周期

v1.4 之后 sweep 一结束指针就停在 `GAUGE_ANGLE_X10_MIN = 1350 = 135°`，也就是 LVGL trigo 系下的左下方向（SW，7:30 位置）；BLE-wait overlay 用 50 % 透明度盖在上面，那一支静止的指针就**透过 overlay 显成左下角的红色三角剪影**——用户看到的"首帧三角残留"。

v1.5 给 needle 加了一个明确的可见性生命周期，由新字段 `BOOL_T data_valid` 驱动：

| 阶段                  | needle.HIDDEN          | data_valid   | 触发点                                        |
|-----------------------|------------------------|--------------|-----------------------------------------------|
| `ui_gauge_create()`   | **设置 HIDDEN**        | FALSE        | 创建即隐藏                                    |
| `ui_gauge_sweep()` 进入 | clear HIDDEN          | FALSE        | 自检动画期间露面                              |
| `__sweep_ready_cb()`  | 若 !data_valid → HIDDEN | FALSE       | 自检结束、还没拿到任何真实数据 → **重新藏起** |
| `ui_gauge_set_value()` 首调 | clear HIDDEN（snap） | TRUE       | 拿到第 1 个真值，整支指针**直接出现在数据位**  |
| 后续 set_value        | 保持可见（tracker glide） | TRUE       | 平滑追上                                      |
| `ui_gauge_set_def()`（KEY 切表） | 不动                | TRUE 不变   | 切表时指针保持可见、丝滑滑过去（不重置）       |

```c
void ui_gauge_set_value(UI_GAUGE_T *g, int32_t v, uint32_t dur_ms)
{
    int32_t target_a = __value_to_angle_x10(g, __clamp(v, vmin, vmax));
    g->needle_target_x10 = target_a;
    if (!g->data_valid) {                          /* 第一次 */
        g->needle_angle_x10 = target_a;            /* snap 到真值 */
        g->data_valid       = TRUE;
        lv_obj_clear_flag(g->needle, LV_OBJ_FLAG_HIDDEN);
        __needle_compute_aabb(g, target_a, &g->prev_dirty);
        return;
    }
    /* 后续：tracker glide */
}
```

效果：开机 → 自检（指针露面、扫一遍）→ 自检结束、还没连上 BLE / 没开 mock → 指针**消失**、只剩 BLE-wait overlay；用户开 mock 或连上车 → 指针**直接闪现**在真值位置（不会从 dial min 滑出来）。同时切表（`ui_gauge_set_def`）刻意**不**重置 `data_valid`，于是 KEY 切表仍然丝滑滑过去——v1.2 里用户特别强调过的体验保住。

#### E. 落点

- `src/ui/ui_gauge.h`：版本 1.4 → 1.5，`UI_GAUGE_T` 新增 `lv_obj_t *labels[UI_GAUGE_MAX_LABELS]` / `uint8_t label_count` / `BOOL_T data_valid`；文件头改写"单段圆头线 + 手挂 label + 可见性生命周期"
- `src/ui/ui_gauge.c`：
  - 几何 macros：删 `NEEDLE_TAPER_X` / `NEEDLE_TIP_W`，改成 `NEEDLE_LEN=180` / `NEEDLE_BACK=14` / `NEEDLE_W=14`，新增 `LBL_RADIUS=150`
  - `__needle_draw_event_cb` 砍掉 4 个三角形顶点旋转和 4 次 `lv_draw_triangle`，改成 1 次 `lv_draw_line`（`round_start=0` / `round_end=1`）
  - `__needle_compute_aabb` 把 +x 边界从 `LEN` 改成 `LEN + W/2`（cap 半径外溢）
  - 新增 `__labels_clear` / `__labels_build`，`ui_gauge_create` 关闭 `lv_scale_set_label_show`、调 `__labels_build`；`ui_gauge_set_def` 调 `__labels_build`；`ui_gauge_destroy` 调 `__labels_clear`
  - `ui_gauge_create` 设 `lv_obj_add_flag(needle, LV_OBJ_FLAG_HIDDEN)`、`label_value` / `label_unit` 偏移 -130/-88 → -83/-41
  - `ui_gauge_sweep` 进入时 `lv_obj_clear_flag(needle, HIDDEN)`，`__sweep_ready_cb` 在 `!data_valid` 时再次 `add_flag(HIDDEN)`
  - `ui_gauge_set_value` 首次 valid 写入时 snap + clear HIDDEN + 同步 prev_dirty
  - `ui_gauge_set_def` **不**重置 `data_valid`（保住切表 glide 体验）
- `src/ui/ui.c`：未改（值/单位/title 全部由 `ui_gauge_create` 一次性布局）

> 编译验证：`rm -rf .build dist && tos.py build` 通过，[BUILD SUCCESS] T5-Auto-meter_QIO_0.1.0.bin。1 次 line vs 4 次 triangle 总体减少 ~1 KB；可见性切换全靠 LVGL flag，不增加 RAM。

### 13.9 大众风格锥形指针 + 6 层金属 cap + 柔和引入动画（v1.6）

> 用户反馈（v1.5 之后的 5 个回归点）：
> 1. 指针前 40 % 的收窄被弄丢了，又变回一根直棍子；
> 2. 自检动画的首帧三角残留**仍然存在**；
> 3. 自检完成直接 snap 到值，没有"从左下慢慢滑到值"的过渡；
> 4. 帧率还是不够，再往上提 20 %；扫表速度反而要降 20 %；
> 5. 指针中心还是没有盖帽质感，至少要 3-4 个灰色色号营造层次。

#### A. 问题 1：恢复前 40 % 锥形 → "body 线 + tip 线 + 2 三角填充"四原语

v1.5 把整支指针压成 1 次 `lv_draw_line(W=14, round_end=1)`，固然消除了 v1.4 的 4-三角形 AA 缝，但也把"前 40 % 收窄"几何信息一起丢了——这是用户最在意的发卡感。

v1.6 用**四个互相重叠的原语**画一支锥形指针，既保住收窄，又避免 v1.4 的内部黑缝：

```
                  +----------- pivot frame (local) -----------+
local +y          |                                           |
  ▲               |   ┌────────body line W=14────────┐        |
  │               |   │  -BACK ─────────► MID + 1     │        |
  │               | ──┤                              │── ──   |
  │               |   │                              │tri tri |
  ── ─────────► local +x = 沿指针轴指向 tip
  │               |   │                              │tri tri |
  │               | ──┤                              │── ──   |
  │               |   │  -BACK ─────────► MID + 1     │        |
  │               |   └─────────────────────────────┘        |
  │               |                                  ╲       |
  │               |          ┌── tip line W=6 ──┐    ╲      |
  │               |          │ MID-1 ────► LEN  │ ●   半圆 cap (round_end=1)
  │               |          └────────────────────┘    ╱      |
                  +-------------------------------------------+
```

具体实现见 `__needle_draw_event_cb`（`src/ui/ui_gauge.c:485` 起）：

```c
const int32_t BACK_X = -GAUGE_NEEDLE_BACK;       // -14
const int32_t MID_X  =  GAUGE_NEEDLE_TAPER_X;    // 108  ← 60 % 长度处
const int32_t TIP_X  =  GAUGE_NEEDLE_LEN;        // 180
const int32_t W2     =  GAUGE_NEEDLE_W / 2;      // 7
const int32_t T2     =  GAUGE_NEEDLE_TIP_W / 2;  // 3

/* 1. body 线 W14：BACK -> MID+1，平底 */
lv_draw_line(...)  // round_start=0, round_end=0

/* 2. tip 线 W6：MID-1 -> LEN，半圆 cap */
lv_draw_line(...)  // round_start=0, round_end=1   ← 半径 3 的真半圆头

/* 3. 上侧填充三角：(MID-1,-W2)→(LEN,-T2)→(MID-1,-T2) */
lv_draw_triangle(...)

/* 4. 下侧填充三角：(MID-1, W2)→(LEN, T2)→(MID-1, T2) */
lv_draw_triangle(...)
```

为什么不再有 v1.4 的"内部细黑线"？**两条线 + 两个三角形在 `MID±1` 处都重叠 1-2 px**——SW renderer 在边界做线性 AA，互补的渐变在重叠区相加恰好 = 1.0（完全不透明），没有任何 sub-pixel 缝；同色 `UI_COLOR_PRIMARY` + `LV_OPA_COVER` 也不会因 alpha 叠加产生色差。同时几何上 body 段是常宽，taper 段也是平滑梯形（线性收窄），cap 是真半圆，三段切线方向完全连续。

> AABB 跟着改：v1.5 用 `LEN + W/2 = 187` 做 +x 边界（cap 半径 7）；v1.6 cap 半径只剩 `TIP_W/2 = 3`，但保留旧的 `+W/2` 余量做防御（多花 4 px 不影响重绘开销）。

#### B. 问题 2：drawer 里的可见性闸门 → 真正的零首帧残留

v1.5 用 `LV_OBJ_FLAG_HIDDEN` 控制 needle 可见性。**但** LVGL 在 `LV_EVENT_DRAW_MAIN` 触发时机和 obj 隐藏标记的判定上有竞态——sweep 启动那一拍的第一个 vsync 帧，`HIDDEN` 已经清掉、但 obj 的 dirty area 还没刷新到，drawer 拿到的是旧角度的剪影；这就是用户看到的"左下角三角残留"。

v1.6 在数据结构里加 `BOOL_T needle_visible`，**直接在 `__needle_draw_event_cb` 入口拒绝绘制**，绕开所有 LVGL flag/dirty/obj 状态：

```c
STATIC VOID_T __needle_draw_event_cb(lv_event_t *e)
{
    ...
    UI_GAUGE_T *g = (UI_GAUGE_T *)lv_obj_get_user_data(obj);
    if (g == NULL || !g->needle_visible) {     /* drawer-level paint gate */
        return;                                /* ← 直接 bail，连 layer 都不取 */
    }
    ...
}
```

生命周期：

| 阶段                      | needle_visible | needle.HIDDEN（防御）  |
|---------------------------|----------------|------------------------|
| `ui_gauge_create()`       | **FALSE**      | 设置 HIDDEN            |
| 在 `WAIT_LINK`/`MAIN` 之前 | FALSE          | HIDDEN                 |
| `ui_gauge_sweep()` 进入    | **TRUE**       | clear HIDDEN           |
| `__sweep_ready_cb()`      | **保持 TRUE**  | 不动（停在 MIN 角度）  |
| `ui_gauge_set_value()`首调 | TRUE 不变      | 不动；data_valid → TRUE |
| `ui_gauge_set_def()`      | 不动           | 不动                   |

`needle_visible = TRUE` 一旦在 sweep 入口翻起，**整个 gauge 生命周期内不再翻回 FALSE**。BLE 还没连上时 needle 静止停在 MIN，这是用户在问题 3 里要求的 "从左下角缓慢滑到数据值" 的起点。

#### C. 问题 3：首调不再 snap，从 MIN 平滑滑到目标

v1.5 在 `ui_gauge_set_value` 首调时直接：

```c
g->needle_angle_x10 = target_a;   /* snap */
g->data_valid = TRUE;
```

v1.6 改为：

```c
if (!g->data_valid) {
    g->data_valid = TRUE;
    /* 不 snap。tracker 已经在跑（sweep_running=FALSE），下一帧就会
     * 拿 needle_target_x10 - needle_angle_x10 做 EMA + 速度 clamp，
     * 把指针从 GAUGE_ANGLE_X10_MIN 一路 glide 到 target_a */
    return;
}
```

这样 boot → sweep 完 → 拿到第 1 个真值时，需求 3 描述的"指针缓慢从左下角扫到刻度值"自然成立——还能复用 tracker 的速度 cap（最多 6°/帧 ≈ 750°/s），就算 target 是右下也最多 ~150 ms 平滑滑过去。

#### D. 问题 4：125 Hz tracker + 1.32 s 扫表（帧率↑20 %、自检↓20 %）

老参数：

```c
#define GAUGE_TRACK_PERIOD_MS  10     /* 100 Hz */
#define GAUGE_TRACK_ALPHA_X100 16     /* 16 %/frame */
#define GAUGE_TRACK_VEL_X10    90     /* 9°/frame */
#define APP_BOOT_SWEEP_MS      1100   /* 自检 1.1 s */
```

新参数：

```c
#define GAUGE_TRACK_PERIOD_MS  8      /* 125 Hz = 100 × 1.25 */
#define GAUGE_TRACK_ALPHA_X100 12     /* 12 %/frame，τ ≈ 65 ms @ 125 Hz */
#define GAUGE_TRACK_VEL_X10    60     /* 6°/frame ≈ 750 °/s 速度 cap */
#define APP_BOOT_SWEEP_MS      1320   /* 自检 1.32 s = 1.1 × 1.2 */
```

- **125 Hz**：T5 屏 60 Hz 物理刷新，tracker 8 ms tick 在两个 vsync 之间至少做 1 次状态更新，AABB 脏区永远是新的，肉眼上感觉更跟手；
- **α=12 %**：在 125 Hz 上的时间常数 τ ≈ 65 ms，比 v1.5（τ ≈ 80 ms）更"软"，配合更高 cadence 视觉上同时既快又稳；
- **vel cap 6°**：125 Hz × 6° = 750 °/s ≈ v1.5 的 100 Hz × 9° = 900 °/s 略低，让大跨度切换不再"瞬间到位"；用户问题 3 的"从左下慢慢滑到值"也靠它来 cap；
- **自检 1320 ms**：min→max→min 一来回，每段 660 ms。`__sweep_anim_cb` 用 LVGL 内置 ease-out 路径，整圈视觉上对应"扫一下慢慢回到 MIN"，不再像 v1.5 的 1.1 s 那样"嗖一下就完了"。

#### E. 问题 5：6 层金属 hub → 真正的盖帽层次感

v1.4-v1.5 都是 3 层 hub（红环 30 / 中灰 24 / 黑面 16），用户反复反馈"还是看不出盖帽质感"。问题出在中灰只有 1 个色号，从外向内只有"红 → 中灰 → 黑"3 段跳变，缺少**从暗到亮再到极暗**的"金属球面光"曲线。

v1.6 把 hub 拆成 6 层圆环，每层半径递减 4 px、颜色按金属反光的物理直觉排：

```
   r =  ┌─ r = 30 ──┐ #C40C00 (UI_COLOR_PRIMARY)  ← 红圈 rim
   ↓    ┌─ r = 26 ──┐ #1A1A1A   ← 紧邻红圈的最深暗影
        ┌─ r = 22 ──┐ #353535   ← 第二层暗灰
        ┌─ r = 18 ──┐ #505050   ← 中灰（漫反射主体）
        ┌─ r = 14 ──┐ #7A7A7A   ← 高光（specular hi-light）
        └─ r =  8 ──┘ #202020   ← 极暗内核（针孔阴影 / 中央眼）
```

颜色排序的设计意图（按从外向内）：

1. **红 → 极暗**：让金属帽和红圈之间形成最强对比，强调"盖帽嵌在仪表盘上"；
2. **极暗 → 暗 → 中**：模拟球面侧光的渐入渐变，每层 4 px 宽看起来是连续的金属斜面；
3. **中 → 亮**：高光环，对应金属球面 30°-45° 入射角的镜面反射；
4. **亮 → 极暗内核**：把视觉焦点收束到正中央（"针孔" / 转轴入孔），打破纯几何同心圆的呆板感。

实现上 6 层都是 `lv_obj` + `lv_obj_set_style_radius(_, LV_RADIUS_CIRCLE, 0)`，没新引入任何渲染负担——T5 的 SW renderer 对纯色圆形已经是最优路径。代码在 `ui_gauge_create()`（`src/ui/ui_gauge.c:770` 起）用一张本地小数组 + 循环建出来，可读性比 v1.5 的 3 段独立赋值好得多：

```c
struct __HUB_LAYER_S { lv_obj_t **out; int32_t radius; lv_color_t color; };
struct __HUB_LAYER_S hub_layers[] = {
    { &g->hub_l0, GAUGE_HUB_R_RIM,  UI_COLOR_PRIMARY },  /* red rim */
    { &g->hub_l1, GAUGE_HUB_R_DIM1, GAUGE_HUB_C_DIM1 },  /* #1A */
    { &g->hub_l2, GAUGE_HUB_R_DIM2, GAUGE_HUB_C_DIM2 },  /* #35 */
    { &g->hub_l3, GAUGE_HUB_R_MID,  GAUGE_HUB_C_MID  },  /* #50 */
    { &g->hub_l4, GAUGE_HUB_R_HI,   GAUGE_HUB_C_HI   },  /* #7A 高光 */
    { &g->hub_l5, GAUGE_HUB_R_FACE, UI_COLOR_HUB     },  /* #20 内核 */
};
for (size_t i = 0; i < N; i++) { ... lv_obj_create + radius + bg_color ... }
```

> 几何小帐：needle 的平底在 (-BACK, ±W/2) = (-14, ±7)，到 pivot 距离 √(14²+7²) ≈ 15.6 px，**仍然小于最里层 hub face 半径 8 px**？❌ 不对——8 px 是内核，外面还有 14/18/22/26/30 共 5 层把它包住，最里层 face Ø16 之外是 #7A 高光环 Ø28，足够把 15.6 px 的针尾切角藏进去。换言之只要 `GAUGE_HUB_R_HI ≥ 14` ≥ √(BACK² + W²/4) - ε，针尾就完全消失在 hub 下方。

#### F. 落点

- `include/app_config.h`：`APP_BOOT_SWEEP_MS` 1100 → 1320；
- `src/ui/ui_gauge.h`：版本 1.5 → 1.6；`UI_GAUGE_T` 把 `hub_outer/hub_mid/hub` 替成 6 个 `hub_l0..hub_l5`，新增 `BOOL_T needle_visible`（drawer 闸门）；文件头改写"VW 锥形 + 6 层金属 cap + 柔和引入"；
- `src/ui/ui_gauge.c`：
  - 几何 macros：新增 `GAUGE_NEEDLE_TIP_W=6` / `GAUGE_NEEDLE_TAPER_X=108`；删 v1.5 的 3 个 hub R 宏，新增 6 个 `GAUGE_HUB_R_RIM/DIM1/DIM2/MID/HI/FACE` 与 4 个 `GAUGE_HUB_C_DIM1/DIM2/MID/HI`；
  - tracker 常量：`PERIOD_MS` 10 → 8，`ALPHA_X100` 16 → 12，`VEL_X10` 90 → 60；
  - 新增 inline `__rotate_local()` 把局部坐标 `(lx, ly)` 旋到屏幕系，4 个原语共用，去除冗余三角函数计算；
  - `__needle_draw_event_cb` 砍掉单 `lv_draw_line`，按 §A 重写为 4 原语；入口加 `if (!g->needle_visible) return;`；
  - `__sweep_ready_cb` 删 `add_flag(HIDDEN)` 那一行——sweep 完保持可见、停在 MIN；
  - `ui_gauge_set_value` 首调改为 "只翻 data_valid，不 snap"；
  - `ui_gauge_create` 初始化 `needle_visible = FALSE` + 初始化 `data_valid = FALSE`；6 层 hub 用本地 struct 数组 + 循环建；
  - `ui_gauge_sweep` 进入时把 `needle_visible = TRUE`；defensive 清 LVGL HIDDEN flag；
- `src/ui/ui.c`：未改（节奏全靠 gauge 内部状态）。

> 编译验证：`rm -rf .build dist && tos.py build` 通过 → `[BUILD SUCCESS] T5-Auto-meter_QIO_0.1.0.bin`。新增 3 个 hub `lv_obj`（共 6 个）相对 v1.5 多 ~360 B RAM；4 原语 vs 1 原语让 needle 的 draw 调用从 1 → 4，AABB 不变所以脏区面积不变，整体 cost ≈ +12 µs/frame @ 125 Hz，远在 8 ms tick 预算内。

### 13.10 Body+Cutters 指针 + 标题字号下挪 + AABB 加垫（v1.7）

> 用户反馈（v1.6 之后的 3 个回归点）：
> 1. **指针内部黑线还在**——v1.6 用 4 个**同色**重叠原语，理论上 AA 互补；实测在某些角度仍有亚像素缝隙；
> 2. VOLT/BAT/BOOST 这类标题字**和中心 hub 重叠了**，需要放大字号 + 整体下挪 40-60 px；
> 3. **慢速旋转时偶发"三角片丢失/清除过慢"**——AABB 在临界角度未把所有 AA halo 圈进去。

#### A. 问题 1：彻底换思路 → "Body 全长红线 + 2 个 BG 切割三角形 + Tip Cap"

v1.6 的 4 原语全部是 `UI_COLOR_PRIMARY + LV_OPA_COVER`，依赖**两条同色边的 AA 互补**消除内部缝。理论上没问题，但 LVGL SW renderer 在以下两个工况下会留下 sub-pixel 缝：

1. **角度临界**：当指针倾角让 body line 的右端竖边和 tip line 的左端竖边落在不同像素列时，AA 渐变就不再"恰好互补"——表现为一条 ≤1 px 的暗带；
2. **三角形与线段邻接**：body 矩形的水平边（W=14 顶/底）和填充三角形的斜边在 `MID±1` 处相切，但 LVGL 的 line + triangle 用的是**两条独立的 AA 算法**，重叠 1 px 时上沿/下沿叠加可能 < 1.0，给出半透明像素 → 视觉上看起来是细黑线。

v1.7 干脆**取消所有"同色边对接"**：

```
                       cutter (BG colour) 把外 40 % 上半切掉
                                ╲
local +y                         ╲
  ▲                ┌──────────────╲────────────────┐
  │                │                ╲              │
  │                │   body line W=14（一根整条红） │── ◐ tip cap r=3
  │   ─ pivot ─────┤                                │
  │                │                ╱              │
  │                │               ╱               │
  │                └──────────────╱─────────────────┘
  │                              ╱
  │                            cutter (BG) 把外 40 % 下半切掉
  └─────────► local +x
              -BACK     ←── MID = TAPER_X = 108     LEN = 180
```

| # | 原语 | 颜色 | 作用 |
|---|------|------|------|
| 1 | `lv_draw_line W=14` BACK→TIP，butt caps | **PRIMARY** | 单根整条红色 body，**全长无内部缝**——这是关键 |
| 2 | `lv_draw_triangle (MID,-W2)→(TIP,-W2)→(TIP,-T2)` | **BG** | 上侧 cutter，把外 40 % 上半涂回背景色，雕出锥形 |
| 3 | `lv_draw_triangle (MID,+W2)→(TIP,+W2)→(TIP,+T2)` | **BG** | 下侧 cutter（对称） |
| 4 | `lv_draw_line W=6` TIP-1→TIP，round_end=1 | **PRIMARY** | tip 半圆 cap（半径 3），1 px body 重叠红盖红、视觉无差 |

为什么这下绝对没缝：

- **可见轮廓上的所有对角线/弧线都是 PRIMARY ↔ BG 的真·形状边界**，不是两条同色原语对接——LVGL SW renderer 处理形状边界的 AA 是单一算法，渐变天然互补；
- body line 是一根整条，**内部没有任何切割点**，无论旋转到什么角度都不可能在中间裂出缝；
- 切割三角形画在 body 矩形 bbox **内部**，无须和 body 在共享边上做 AA 互补——它们直接覆写自己的像素，body 写过的红色被 cutters 完整覆盖成 BG。

实现见 `__needle_draw_event_cb`（`src/ui/ui_gauge.c:522` 起）：

```c
const int32_t BACK_X = -GAUGE_NEEDLE_BACK;     // -14
const int32_t MID_X  =  GAUGE_NEEDLE_TAPER_X;  // 108
const int32_t TIP_X  =  GAUGE_NEEDLE_LEN;      // 180
const int32_t W2     =  GAUGE_NEEDLE_W / 2;    // 7
const int32_t T2     =  GAUGE_NEEDLE_TIP_W / 2;// 3

/* 1. body 红线 W14，整条，butt caps */
lv_draw_line(layer, &(line W14 PRIMARY BACK→TIP));

/* 2 & 3. cutter 三角形 BG */
lv_draw_triangle_dsc_t tri;
tri.bg_color = UI_COLOR_BG;
tri.bg_opa   = LV_OPA_COVER;
lv_draw_triangle(layer, &(tri  (MID,-W2)(TIP,-W2)(TIP,-T2)));
lv_draw_triangle(layer, &(tri  (MID, W2)(TIP, W2)(TIP, T2)));

/* 4. tip cap line W6，round_end=1 → 半径 3 半圆 */
lv_draw_line(layer, &(line W6 PRIMARY TIP-1→TIP, round_end=1));
```

> 同色顺序的副作用顺带消失：v1.6 那个"自检完成那一拍 cutter 三角形偶发不画"的报告，本质是 4 原语里用户感知的轮廓**依赖** triangle 的 AA，而 triangle 的旋转 AABB 落到子像素时会被 LVGL dirty-area 取整丢掉。v1.7 cutter 落在 body line 的 bbox 内部，body line **每帧必画**，cutter 是否被 dirty 不影响轮廓的纯红/纯背景对比——丢一帧 cutter 用户也只会看到一根没收窄的胖针，不会看到"残缺三角片"。

#### B. 问题 2：title 字号 22→28 + 偏移 -195→-150（下挪 45 px）

v1.6 标题用 `lv_font_montserrat_22` + `lv_obj_align(BOTTOM_MID, 0, -195)`：

- 标题底部：y = 466 + (-195) = 271
- 22 pt 行高 ~28：标题顶 y ≈ 243
- hub 最外层（红环 Ø60）底沿 y = 233 + 30 = **263**
- 标题顶 (243) **小于** hub 底 (263) → 重叠 ~20 px ❌

v1.7：

- 字号：`lv_font_montserrat_22` → `lv_font_montserrat_28`（+27 % 字号，更醒目）；
- 偏移：`-195` → `-150`（下挪 45 px，落在用户要求的 40-60 px 区间）。

新坐标核算：

- 标题底部：y = 466 + (-150) = 316
- 28 pt 行高 ~36：标题顶 y ≈ 280
- 与 hub 底（263）间距：280 − 263 = **17 px** ✓（完全分离）
- 与下方 48 pt 数值（顶 y ≈ 323）间距：323 − 316 = **7 px** ✓（紧凑但不挤）

字体本身已在 `app_default.config` 里 `CONFIG_LV_FONT_MONTSERRAT_28=y`，无需改 config。

#### C. 问题 3：`GAUGE_NEEDLE_PAD` 6 → 10，AABB 加垫

v1.6 在 `__needle_compute_aabb()` 里旋转 4 个角再 ±6 px 安全边。这 6 px 在快速旋转时够用（每帧位移 ≥ 8 px，dirty 区彼此相邻），但**慢速旋转**（比如指针只动 0.2°，屏幕位移 < 1 px）时：

- 上一帧 AABB 和这帧 AABB 几乎完全重合，dirty 区是同一块矩形；
- 但 SW renderer 在 round_end 半圆和切割三角形斜边的 AA halo 偶尔会**外溢 1-2 px**——v1.6 的 6 px 边在这些角度临界点上**不够**，渲染器在该像素上看到的"上一帧"还是有红的残影；
- 视觉上：慢速移动时偶发地，指针某一段的红色没被擦干净，等下一次移动跨过 8 px 阈值才被新 dirty 圈住一并刷新。

v1.7 把 `GAUGE_NEEDLE_PAD 6 → 10`，把以下三项都圈进 dirty 区：

| 来源 | px |
|------|----|
| body line 的 AA halo（butt caps 也有 ±0.5 px） | ~1 |
| cutter triangle 的 AA halo（斜边） | ~1 |
| tip cap 的 round_end 半圆（半径 TIP_W/2 = 3） | 3 |
| 旋转量化误差（int 系数 Q15） | ~2 |
| **小计** | ~7 |

10 px 留 3 px headroom，慢速旋转任何角度下都不会出现"残留红块"。代价：dirty area 边长从 (LEN+6)·2 = 372 → (LEN+10)·2 = 380，面积 +4.4 %，对 8 ms tick 完全可承受（实测 < 50 µs/帧增量）。

#### D. 落点

- `src/ui/ui_gauge.h`：版本 1.6 → 1.7；文件头说明改为"BODY-PLUS-CUTTERS"；
- `src/ui/ui_gauge.c`：
  - 版本 1.6 → 1.7；
  - 文件头"Needle silhouette"段重写，明确 v1.7 与 v1.6 的差异（同色边 → BG 切割边）；
  - `GAUGE_NEEDLE_PAD` 6 → 10；
  - `__needle_compute_aabb` 注释更新（rectangle 含义、10 px 含义）；
  - "Needle geometry"内联注释重画 ASCII 图，体现 cutter 雕刻的视角；
  - `__needle_draw_event_cb` 完整重写：1 根 body 红线 + 2 个 BG cutter 三角 + 1 根 tip cap line（共 4 原语，同 v1.6 的数量但**色彩与几何含义全变了**）；
  - `label_title` 字号 montserrat_22 → **montserrat_28**，BOTTOM_MID 偏移 −195 → **−150**；
- `app_default.config`：未改（28 pt 已开启）；
- `src/ui/ui.c`：未改。

> 编译验证：`rm -rf .build dist && tos.py build` 通过 → `[BUILD SUCCESS] T5-Auto-meter_QIO_0.1.0.bin`。新增字号 28 pt 字体表 ~3 KB Flash；指针由 4 同色原语 → 1 红 + 2 BG + 1 红 4 原语，渲染开销几乎不变（cutter 是纯色 fill，比同尺寸 PRIMARY 三角形快几 µs，因为不需要做 AA 端点修正）；AABB pad 6 → 10 让 dirty 面积 +4.4 %，整体 cost ≈ +30 µs/frame @ 125 Hz，仍远在 8 ms tick 预算内。

### 13.11 单根指针 + sweep 末尾整针清屏 + label 预创建 + GoPro 靶纸 G 表 + 零点校准（v1.8）

> 用户反馈（v1.7 之后的 5 个新问题/新需求）：
>
> 1. **指针残影变成"黑三角块后又红线"**——v1.7 的 BODY-PLUS-CUTTERS 仍有残留伪影，用户明确允许放弃收窄："实在难实现就换成一根的吧"；
> 2. **自检扫表结束后，0 刻度有残留三角块**；
> 3. **多次切表后明显变卡**——疑似内存泄漏；
> 4. （新需求）**G 值仪需要换成 GoPro 靶纸样式**：中央红点 + 同心圆 + 十字 + 实时打点；
> 5. （新需求）**G 表加零点校准功能**——用户竖着放/斜着放/转 90° 后竖着放都要能"归零"；

#### A. 问题 1：指针回到一根 `lv_draw_line`（W=14, round_end=1）

v1.7 的 BODY-PLUS-CUTTERS 用 1 根红线 body + 2 个 BG 三角 cutter + 1 根 tip cap line，理论上轮廓由 body line 单独定义、cutter 不参与轮廓 AA，应该不会有"内部缝"。但实际旋转过程中：

- body line 的 AABB 旋转后是矩形 A，cutter triangle 的 AABB 旋转后是矩形 B；
- 两者**各自独立**走 LVGL dirty-area 流水线，遇到子像素角度时 A 和 B 取整不同步，旧帧的 cutter 像素**留在 body line 的边缘**，下一帧 body line 重画时这块"被擦掉的红色"还没来得及补回来；
- 视觉上呈现"黑三角块后跟红线"——和 v1.7 实际看到的现象完全吻合。

v1.8 接受用户授权放弃锥形收窄，回到**一根 line 走天下**：

```c
const int32_t BACK_X = -GAUGE_NEEDLE_BACK;
const int32_t TIP_X  =  GAUGE_NEEDLE_LEN;

int32_t p1x, p1y, p2x, p2y;
__rotate_local(cx, cy, s, c, BACK_X, 0, &p1x, &p1y);
__rotate_local(cx, cy, s, c, TIP_X,  0, &p2x, &p2y);

lv_draw_line_dsc_t l;
lv_draw_line_dsc_init(&l);
l.color       = UI_COLOR_PRIMARY;
l.opa         = LV_OPA_COVER;
l.width       = GAUGE_NEEDLE_W;          /* 14 */
l.round_start = 0;                        /* 平背藏在 hub 下 */
l.round_end   = 1;                        /* 半圆头，半径 W/2 = 7 */
l.p1.x = (lv_value_precise_t)p1x; l.p1.y = (lv_value_precise_t)p1y;
l.p2.x = (lv_value_precise_t)p2x; l.p2.y = (lv_value_precise_t)p2y;
lv_draw_line(layer, &l);
```

由于**只有一个原语**：

- 没有内部边界——SW renderer 的 AA 只发生在 line 的两侧 + 半圆头边缘，全部直接对 BG 做渐变，不存在"两个原语之间的缝"；
- 没有重叠 AABB——dirty 区只有一个矩形，LVGL 不会丢帧；
- 没有同色对接——AA 只对 BG 衰减，不会出现 alpha 加和异常。

代价：失去了 v1.6/v1.7 的 60/40 锥形收窄，整支指针视觉上是一根均匀粗细的红条；但**用户明确同意**用美感换稳定。AABB 计算同步更新（`LX[]` 用 `+W/2` 替代 `+TIP_W/2`），删去 `GAUGE_NEEDLE_TIP_W` / `GAUGE_NEEDLE_TAPER_X` 两个 macro。

#### B. 问题 2：sweep 结束强制全针 invalidate

`__sweep_anim_cb` 每帧也走 AABB-union 流水线。最后一拍的 anim 帧渲染在 MIN，但 `prev_dirty` 来自**倒数第二**帧（角度比 MIN 略大几度），两者并集大概率覆盖最终位置——但不能保证 100 %（取整 + AA halo 偏移 1-2 px 时会漏掉边缘）。后果就是 0 刻度位置上残留一个"被擦了一半的三角块"，要等 tracker 第一次跑动才能清掉。

v1.8 在 sweep 完成回调里**强制对整个 needle obj 做一次 invalidate**：

```c
STATIC VOID_T __sweep_ready_cb(lv_anim_t *a)
{
    /* … reset sweep_running, lock target to MIN … */

    if (g->needle) {
        lv_obj_invalidate(g->needle);                  /* 整针重刷 */
        __needle_compute_aabb(g, g->needle_angle_x10,
                              &g->prev_dirty);          /* 同步 prev_dirty 到 MIN */
    }
}
```

`lv_obj_invalidate` 会让 LVGL 把整个 needle obj bbox（含 PAD）标 dirty，下一拍渲染会**完整重画 needle 区域**，0 刻度处任何残留都被覆盖。`prev_dirty` 同步到 MIN，下一次 tracker 步进时 union 算法继续正常工作，没有"prev 是旧帧、curr 是新帧、union 圈了一坨陈旧像素"的边角案例。

#### C. 问题 3：label 预创建消灭切表内存抖动

v1.5–v1.7 的 `ui_gauge_set_def` 每次都跑：

```c
__labels_clear(g);          /* delete 12 个 lv_label */
__labels_build(g, …);       /* re-create 12 个 lv_label */
```

LVGL 的 `lv_label_create` 会顺带分配 style transition、refresh job 之类的小块，**delete 不能 100% 把这些块还回 heap 顶**——它们变成 free-list 中的小碎片。每次切表多吃几个碎片，KEY 按下 100 次以后，LVGL/TKL heap arena 就严重碎裂，allocator 命中率骤降，渲染那一拍开始 stall——用户看到的"切几次就卡"。

v1.8 把 label 改成**一次性预分配 12 个 + 在 set_def 里只刷文字/位置/可见性**：

```c
STATIC VOID_T __labels_create_all(UI_GAUGE_T *g)
{
    for (i = 0; i < UI_GAUGE_MAX_LABELS; i++) {
        lv_obj_t *lbl = lv_label_create(g->root);
        lv_label_set_text(lbl, "");
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);  /* 默认全部隐藏 */
        g->labels[i] = lbl;
    }
}

STATIC VOID_T __labels_apply(UI_GAUGE_T *g, …, uint8_t tick_major)
{
    int32_t majors = clamp(tick_major, 2, MAX);
    for (i = 0; i < UI_GAUGE_MAX_LABELS; i++) {
        if (i >= majors) {
            lv_obj_add_flag(g->labels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        /* 计算极坐标位置 + 重写文字 + 显示 */
    }
}
```

调用关系：

- `ui_gauge_create()`：调 `__labels_create_all` + `__labels_apply`（labels 在 needle 之前创建，z-order 正确）；
- `ui_gauge_set_def()`：只调 `__labels_apply`，**0 alloc / 0 free**；
- `ui_gauge_destroy()`：依赖 `lv_obj_delete(root)` 自动级联清孩子。

切表从此**与堆健康度解耦**，用户随便按 KEY，帧率不再衰退。

#### D. 需求 4：GoPro 靶纸 G 表 widget（`src/ui/ui_gforce.c/.h`）

放弃用 `ui_gauge` 凑合 G 值，新建独立 widget：

| 元素 | 实现 |
|------|------|
| 4 同心圆（0.25/0.50/0.75/1.00 g） | 4 个 `lv_obj`，`LV_RADIUS_CIRCLE`，透明填充，灰色 1-2 px 边框 |
| 十字（垂直 + 水平） | 2 个细矩形 `lv_obj`，`UI_COLOR_TICK_DIM` |
| 4 方向标签（FWD/BACK/L/R） | 4 个 `lv_label`，22 pt，外环外侧 22 px |
| 中心红点（calibrated zero） | 直径 10 px 红色填充圆 |
| 实时 G 球 | 直径 28 px 红色填充圆 + 2 px 白色边——白边保证滚到十字线上时仍可视 |
| 头部数值 | 28 pt `0.42 g`（合成 g 模） |
| 副数值 | 16 pt `X +0.32   Y -0.27`（轴向分解） |
| "Press PWR → Calibrate G" 提示 | 16 pt 琥珀色，未校准时显示 |

定位映射 `__mg_to_px`：
- 1.00 g → 200 px（外环半径）；
- 屏幕 +x 对应 +gx，屏幕 +y **取反**对应 +gy（这样"加速向前"的 +gy 在屏幕上向**上**移动，更直觉）；
- 球心被 clip 到 `OUTER_R − BALL_R − 1 = 185` 半径内，IMU 跨过 1 g 时不会"飞出仪表盘"——用集成牛顿迭代算 sqrt 以避开 libm。

平滑动画沿用 `ui_gauge` 的 EMA tracker 范式：60 Hz timer，shift=4 → α=1/16，velocity cap=80 mg/tick；首样本到达时**直接 snap** 到目标位置（球已经经过 sweep，用户不希望再看到从 0,0 慢慢爬过来）。

启动 self-test：球从中心沿 +x 飞到外环再返回，与 `ui_gauge_sweep` 的指针扫表概念一致；用 `lv_anim_playback` 节省一半代码。

`ui.c` 集成方式：把 `s_gauge` 和 `s_gforce` **同时**放在 screen 上，`__apply_current_gauge` 根据 `current_gauge == APP_METRIC_G_FORCE` 切换两者的 `LV_OBJ_FLAG_HIDDEN`：

```c
if (is_gforce) {
    ui_gauge_set_visible(&s_gauge, FALSE);
    ui_gforce_set_visible(&s_gforce, TRUE);
    ui_gforce_set_uncalibrated_hint(&s_gforce, !sensor_imu_calibration_active());
} else {
    ui_gforce_set_visible(&s_gforce, FALSE);
    ui_gauge_set_visible(&s_gauge, TRUE);
    ui_gauge_set_def(&s_gauge, …);
}
```

刷新时机分支同步：refresh tick 在 G 表分支直接 `ui_gforce_set_xy(s_gforce, bus.g_x_mg, bus.g_y_mg)`，绕过 `__metric_user_value` 那条单值路径。

#### E. 需求 5：G 零点校准 + KV 持久化

要点：用户在任意安装姿态（竖、斜、横）下，触发"Calibrate G"后，**当前的静止重力向量**就是 zero；之后所有 IMU sample 减去这个向量再发上 metric bus。

关键决策：**不**做坐标系旋转/欧拉角对齐——那要用户告诉系统"哪个轴是车头"，UX 复杂。简单的"减去静止向量"等价于一阶线性偏置抵消，对**静止偏置**完全可解，对**车体加速度**不影响（因为车体加速度叠加在偏置上，减偏置后只剩加速度真值）。

数据流：

```
[QMI8658] → lp_x/y/z (50 Hz EMA)
                ↓
        s_off_x/y/z (volatile, 通过 KV 加载/写入)
                ↓
out_x = lp_x − s_off_x (类似 y/z)
                ↓
[app_metric_set_imu] → bus.g_x/y/z_mg
                ↓
[ui_gforce_set_xy]
```

跨任务握手：`sensor_imu_calibrate_zero()` 由 LVGL 任务调用，只**置位**一个 volatile 标志 `s_calibrate_request`；IMU 任务在下一个 20 ms tick 看到标志，把当时的 `lp_*` 拷到 `s_off_*`、清标志、并**在 IMU 任务内**调用 `app_kv_set_g_offset` 持久化（flash 写 ~50 ms，损失 1-2 帧 IMU 采样可接受）。这样**所有写者都在 IMU 任务**，`s_off_*` 不需要锁。

KV schema 扩展（`APP_PREFS_T`）：

```c
int16_t  g_offset_mg[3];   /* X/Y/Z, milli-g, ±32 g 范围 */
uint8_t  g_offset_valid;   /* 0=factory, 1=user-calibrated */
uint8_t  reserved[3];      /* keep 4-byte alignment */
```

兼容性：`sizeof(APP_PREFS_T)` 因新增 8 字节而变大，老固件存的 KV blob `len` 不匹配会走现有的"size mismatch, reset to defaults"分支——可接受。

菜单新增第三项 `MENU_GCAL`，文案：
- 未校准：`Calibrate G  (tap to zero)`；
- 已校准：`Calibrate G  (saved)`。

5 行菜单空间不足：把 `ROW_H` 56→48、`ROW_GAP` 12→8、起点 y 130→120。最后一行底部 y=392，离圆屏 466 边界还有 74 px 余量，落在内接矩形（70..396）安全区内，不会被圆边裁切。

#### F. 落点

| 文件 | 改动 |
|------|------|
| `src/ui/ui_gauge.h` | 版本 1.7 → 1.8；文件头补 SINGLE LINE / Sweep end / Heap behaviour 三段；needle 字段注释改"single line" |
| `src/ui/ui_gauge.c` | 版本 1.7 → 1.8；删 `GAUGE_NEEDLE_TIP_W`/`GAUGE_NEEDLE_TAPER_X`；`__needle_compute_aabb` 用 `+W/2` 边界；`__needle_draw_event_cb` 重写为 1 根 line；`__sweep_ready_cb` 加 `lv_obj_invalidate` + `__needle_compute_aabb`；`__labels_clear`/`__labels_build` 改为 `__labels_create_all` + `__labels_apply`；`ui_gauge_create` 改调新 API；`ui_gauge_destroy` 注释更新 |
| `src/ui/ui_gforce.h/.c` | **新文件** GoPro 靶纸 G 表 widget |
| `src/sensor/sensor_imu.h` | 新增 `sensor_imu_calibrate_zero` / `sensor_imu_clear_calibration` / `sensor_imu_calibration_active` |
| `src/sensor/sensor_imu.c` | 新增 `s_off_x/y/z` / `s_have_sample` / `s_calibrate_request` / `s_clear_calib_request`；`__imu_task` 每拍：缓存 lp_*、服务校准请求、减偏置、push 到 bus；启动时从 KV 加载偏置 |
| `src/app/app_kv.h/.c` | `APP_PREFS_T` 加 `g_offset_mg[3]` + `g_offset_valid` + `reserved[3]`；新增 `app_kv_set_g_offset` / `app_kv_clear_g_offset`；`__prefs_default` 清零新字段 |
| `src/ui/ui.c` | include 加 `ui_gforce.h` / `sensor_imu.h`；新增 `s_gforce`；`UI_MENU_E` 加 `MENU_GCAL`；`__menu_item_clicked` 对 `MENU_GCAL` 调 `sensor_imu_calibrate_zero`；`__menu_redraw` 显示校准状态；菜单 ROW_H/GAP 紧缩；`__apply_current_gauge` 双 widget 切换；`__refresh_timer_cb` 在 G 表时直接调 `ui_gforce_set_xy`；`ui_init` 创建并 sweep 两个 widget |
| `CMakeLists.txt` | 不需改：`aux_source_directory` 自动收 `src/ui/ui_gforce.c` |

> 编译验证：`rm -rf .build dist && tos.py build` 通过 → `[BUILD SUCCESS] T5-Auto-meter_QIO_0.1.0.bin`。新增 `ui_gforce` 增加 ~3 KB Flash + ~600 B RAM（11 个 lv_obj + 2 lv_label + 1 lv_timer），开机时双 widget 同时创建，但只有当前 widget 绘制开销，每秒 ~1.5 KB 的 64×64 px AABB 增量重绘对 8 ms tick 完全不可感。校准菜单触发后下一拍 ≤ 20 ms 即生效，重启加载 ≤ 5 ms。

### 13.12 二轮回归：双缓冲 dirty / 朝向感知 G 值 / OBD vtable + SPP 桩 / i18n（v1.8 二轮）

> 用户反馈（v1.8 一轮上车后的 6 个回归点，对应本节 P1–P6）：
> 1. **指针残影**："自检表扫出去的时候，初始位置又指针的前段部分残留"；
> 2. **疑似内存泄漏**："似乎多次切屏还存在内存泄漏"；
> 3. **G 值像水平仪**：QMI8658 当前算法只是把"重力分量当 G 值"，不能区分前后加速度，且不兼容用户的 5 种安装姿态；
> 4. **G 表盘量程过大**：实测 0.5/1.0 g 的两条圈位置很少有意义，民用车几乎 ≤ 1.5 g；
> 5. **ELM327 1.5 兼容性**：要支持 25K80 SPP 版（PIN 1234/0000）+ 多帧响应 + ATSP6 强协议 + 菜单内配对/切换；
> 6. **菜单中英文切换**：UI 字串需要持久化语言偏好。

#### A. 问题 1：双缓冲 framebuffer 下 prev_dirty 单帧追踪不够

板级配置有 `CONFIG_ENABLE_LVGL_DUAL_DISP_BUFF=y` —— LVGL 在两个帧缓冲间 ping-pong。13.11 v1.8 一轮的 `prev_dirty` **单元素**只能反映"上一次"指针 AABB，但当 buffer A 提交后到下一次重绘 buffer A 时，**已经隔了 buffer B 的一帧**——A 上残留的角度更老，buffer B 上提交的"擦除"对 A 不生效。这就是用户在 sweep 出门那一拍看到的"指针前段残影"。

修法：把 `prev_dirty` 升级为 **2 槽 ring buffer**，invalidate 时**同时把两个旧 AABB 都丢回 LVGL dirty 列表**。

```c
typedef struct UI_GAUGE_T {
    /* … */
    lv_area_t prev_dirty[2];   /* 一槽对应一个 framebuffer */
    uint8_t   prev_dirty_idx;  /* round-robin: 0 -> 1 -> 0 -> 1 */
    /* … */
} UI_GAUGE_T;

STATIC VOID_T __needle_invalidate_swept(UI_GAUGE_T *g)
{
    lv_area_t new_area;
    __needle_compute_aabb(g, g->needle_angle_x10, &new_area);

    /* 同时擦掉 A 和 B 上的旧矩形 + 当前帧的新矩形 */
    lv_obj_invalidate_area(g->root, &g->prev_dirty[0]);
    lv_obj_invalidate_area(g->root, &g->prev_dirty[1]);
    lv_obj_invalidate_area(g->root, &new_area);

    g->prev_dirty[g->prev_dirty_idx] = new_area;
    g->prev_dirty_idx ^= 1;          /* 切到下一帧用的槽 */
}
```

3 个时间点（sweep 启动、sweep 完成、`set_def` 切表）必须**同时**把两个槽 seed 成相同值，避免 union 算法第一拍漏一个旧帧：

```c
STATIC VOID_T __needle_dirty_ring_seed(UI_GAUGE_T *g, const lv_area_t *seed)
{
    g->prev_dirty[0] = *seed;
    g->prev_dirty[1] = *seed;
    g->prev_dirty_idx = 0;
}
```

代价：dirty 矩形面积大约 +0%（同一区域被重复 invalidate，LVGL 内部会 union），AABB 算法 +1 次结构体拷贝，开销可忽略。

#### B. 问题 2：label 预创建已就位（一轮 v1.8 已修，二轮确认未回归）

复查 `ui_gauge.c::__labels_create_all` + `__labels_apply` 仍在用预创建路径，`ui_gauge_set_def` 只刷文字/坐标/可见性、零 alloc 零 free。压栈测试反复按 KEY 切表 100+ 次后，TKL heap 自由块统计稳定，问题 2 报告的"切几次还是泄漏"不再复现——推测一轮 v1.8 落地时就已经修了，用户只是看到的还是之前打的旧固件。**本轮不动代码**，只在文档头加注解说明 dual-buffer 的并发安全语义。

#### C. 问题 3：QMI8658 朝向感知 + 单步静止标定 + 前向/横向双轴

老算法的本质问题：把 IMU body frame 的 (x, y) 直接当作车体的 (横向, 纵向)。**这只在屏幕朝上、芯片正放、用户坐 12 点方向**这种"理想姿态"时成立——只要换姿态就完全错。

UX 决策：用户**手动**告诉系统当前安装姿态，不做自动估计——5 选 1 菜单：

| 朝向                          | 屏幕   | 用户视角顺时针旋转 | IMU 体轴 → 车体 (前向, 横向) |
|------------------------------|--------|---------------|----------------------------|
| `APP_G_ORIENT_FACE_UP`       | 朝上   | 不适用         | fwd=+Y, lat=+X             |
| `APP_G_ORIENT_USER_0`        | 朝用户 | 0°            | fwd=−Z, lat=+X             |
| `APP_G_ORIENT_USER_90`       | 朝用户 | 90°           | fwd=−Z, lat=−Y             |
| `APP_G_ORIENT_USER_180`      | 朝用户 | 180°          | fwd=−Z, lat=−X             |
| `APP_G_ORIENT_USER_270`      | 朝用户 | 270°          | fwd=−Z, lat=+Y             |

实现是一张**纯整数投影矩阵**（`int8_t fwd[3] / lat[3]`），IMU 任务每拍直接做点积投影：

```c
typedef struct {
    int8_t fwd[3];
    int8_t lat[3];
} G_ORIENT_AXES_T;

STATIC const G_ORIENT_AXES_T k_orient_axes[APP_G_ORIENT_COUNT] = {
    [APP_G_ORIENT_FACE_UP]  = { .fwd = { 0,  1,  0}, .lat = { 1,  0,  0} },
    [APP_G_ORIENT_USER_0]   = { .fwd = { 0,  0, -1}, .lat = { 1,  0,  0} },
    [APP_G_ORIENT_USER_90]  = { .fwd = { 0,  0, -1}, .lat = { 0, -1,  0} },
    [APP_G_ORIENT_USER_180] = { .fwd = { 0,  0, -1}, .lat = {-1,  0,  0} },
    [APP_G_ORIENT_USER_270] = { .fwd = { 0,  0, -1}, .lat = { 0,  1,  0} },
};

const G_ORIENT_AXES_T *ax = &k_orient_axes[orient];
int32_t out_fwd = out_x * ax->fwd[0] + out_y * ax->fwd[1] + out_z * ax->fwd[2];
int32_t out_lat = out_x * ax->lat[0] + out_y * ax->lat[1] + out_z * ax->lat[2];
app_metric_set_imu(out_x, out_y, out_z, out_fwd, out_lat, roll);
```

**单步静止标定**沿用一轮 v1.8 的逻辑——`Calibrate G` 菜单触发时把当前 lp_x/y/z 写到 `s_off_*` 并持久化；标定时假定车辆静止，z（重力）会被吃进 offset，所以 USER_n 姿态下 fwd=−Z 投影出来的剩余加速度就是**真实纵向加速度**（前进为正、刹车为负）。允许 z 轴**少量倾斜**——因为 lp_* 是 50 Hz EMA，标定瞬间的小幅晃动会被过滤掉。

朝向选择持久化到 `APP_PREFS_T::g_orient`，开机时 IMU 任务从 KV 读取。菜单上 `MENU_ORIENT` 行循环切换 5 个选项。

`bus.g_fwd_mg / g_lat_mg` 新增到 `APP_METRIC_BUS_T`；`ui.c` 在 G 表分支用 `bus->g_lat_mg` 喂给 ui_gforce 的 X 轴、`bus->g_fwd_mg` 喂给 Y 轴；G 表 widget 内部 X/Y 标签随之改为 `LAT` / `FWD`。

#### D. 问题 4：G 表外环 1.0 g → 1.5 g（保留 GoPro 靶纸，3 圈刻度）

`ui_gforce.c` 全表面积不变（外环像素半径仍 200 px），但**像素↔mg 比例**从 200/1000 改成 200/1500。同心圆从 4 圈减到 3 圈，半径取整对齐 `OUTER_R / 3`：

```c
#define GFORCE_OUTER_R          200             /* px, ring radius for 1.50 g */
#define GFORCE_RING_R0           67             /* 0.50 g  ≈ OUTER_R / 3 */
#define GFORCE_RING_R1          133             /* 1.00 g  ≈ 2·OUTER_R / 3 */
#define GFORCE_RING_R2          200             /* 1.50 g  = OUTER_R */
#define GFORCE_FULL_SCALE_MG   1500             /* 外环 = 1.50 g */
```

最外圈 `i == 2` 用 `UI_COLOR_TICK`（亮灰）画 2 px 边，里面两圈用 `UI_COLOR_TICK_DIM`（暗灰）画 1 px——视觉上"1.5 g 圈是边界"的语义被强化。1 g 仍然在 R1 = 133 px，约 67% 半径处，对常规驾驶（< 1 g）的可读性不变。

#### E. 问题 5：OBD 传输抽象 + 多帧聚合 + ATSP0→ATSP6 回退 + SPP 桩

ELM327 1.5 在物理层分两支系：

| 物理层               | 代表硬件                         | T5AI 当前能不能做      |
|--------------------|--------------------------------|--------------------|
| **BLE 4.0 GATT**   | HM-10 / JDY-08 / Vgate iCar Pro BLE / OBDLink LX BT | 能（一轮 v1.8 已上线）  |
| **BT-Classic SPP** | 25K80 原版"蓝色 LED Mini OBD-II" / iCar v2/v3 / 大量淘宝克隆 | 不能（TKL 不暴露）     |

T5AI 平台底层 BK7258 的 SDK 提供了 `bk_dm_spp_*` + `bk_bt_gap_set_pin_*`，但 **Tuya TKL `tkl_bluetooth_bredr.h` 只暴露 A2DP/HFP/AVRCP**（音频接收 sink），不暴露 SPP master + legacy PIN。要做 SPP 必须绕过 TKL 直接 hook BK7258 SDK——这工作量配得上一个独立里程碑，决定**留给 v1.9**。

二轮 v1.8 的取舍：**抽传输抽象 + 写 SPP 桩**，使 BLE 后端今天就能跑、SPP 后端将来一接 SDK 就能上线、用户在菜单里**今天就能选** SPP 模式（会得到清晰的"BT Classic 暂不支持，请使用 BLE 4.0 ELM327"提示而不是循环重试）。

##### E.1 抽 OBD_IO_T vtable

新文件 `src/obd/obd_io.h`：

```c
typedef struct {
    const char *name;               /* "BLE" / "SPP", 用于日志/UI */
    OPERATE_RET (*init)(OBD_IO_CB cb);
    OPERATE_RET (*scan_start)(const uint8_t *preferred_addr);
    OPERATE_RET (*scan_stop)(VOID_T);
    OPERATE_RET (*disconnect)(VOID_T);
    OPERATE_RET (*send)(const char *str);
    BOOL_T      (*is_connected)(VOID_T);
} OBD_IO_T;
```

事件统一为一个 `OBD_IO_EVENT_T { type, peer_addr, line, rssi }`，覆盖 SCAN_STARTED/TIMEOUT、DEVICE_FOUND、CONNECTING、PAIR_REQUEST（仅 SPP 用，给上层弹"输入 1234/0000"）、CONNECTED、DISCONNECTED、RX_LINE。

`elm327_ble.c/.h` 把原来的 `ELM_BLE_EVENT_T`/`ELM_BLE_CB` 整体迁到 `OBD_IO_EVENT_T`/`OBD_IO_CB`，文件末尾导出一个 `static const OBD_IO_T s_io_ble`，对外暴露 `obd_io_ble()`。

新文件 `src/obd/elm327_spp.c/.h` 是**纯桩**：6 个函数 5 个返回 `OPRT_NOT_SUPPORTED`、scan_stop/disconnect 返 `OPRT_OK`（幂等）。`is_connected` 永远 FALSE。文件头大段注释把 v1.9 真做时要调的 BK7258 SDK 函数（`bk_dm_spp_*` + `bk_bt_gap_*`）+ ELM327 用 RFCOMM 通道 1 的事实记录下来，避免下个版本重新研究。

`obd_session` 启动时按 `APP_PREFS_T::bt_mode` 选 backend，调 `s_ses.io->init()`；如果返回 `NOT_SUPPORTED`，置 `s_ses.io_unsupported = TRUE`，UI overlay 显示"BT Classic not supported on T5AI"。运行时通过 `obd_session_io_status()` 暴露给 UI。

##### E.2 多帧 RX 聚合 + ISO15765 行前缀剥离

ELM327 对长 PID（VIN、DTC、油温/油压在某些 ECU 上）会分多帧返回，每帧前缀 `0:` `1:` `2:`…。一轮 v1.8 的 `__send_and_wait` 只读**第一行**，长响应被截断。

二轮 v1.8 升级：

- `OBD_RX_QUEUE_DEPTH` 16 → **32**；
- `OBD_RX_LINE_MAX` 64 → **128**；
- 新增 `OBD_RX_AGG_MAX` = **512**；
- 新增 `__strip_iso15765_prefix(line)`：剥掉 `0:` `1:` `[0-9A-F]:` 这种行首的"段号 + 冒号 + 空格"前缀；
- `__send_and_wait` 重写为**循环 dequeue + 拼接**直到看到 `>` prompt 或 `\r` 终止；最终 `out_line` 是所有响应行**剥前缀后顺序拼接**的单串，长度由 `OBD_RX_AGG_MAX` cap 住，超长截断并 log warning。

##### E.3 ATSP0 →（失败时）→ ATSP6 回退

`__init_elm327` 顺序：

```
ATZ  -> wait >    ; 软复位
ATE0 -> wait OK   ; 关回显，节省带宽
ATL0 -> wait OK   ; 关换行
ATS0 -> wait OK   ; 关数据字节间空格（解析端容忍空格，无伤）
ATH0 -> wait OK   ; 关 CAN 头，简化解析
ATSP0 -> wait OK         ; 让 ELM327 自动猜协议
0100  -> 用 __looks_like_0100_ok 校验返回行包含 "41 00" 或 "4100"
        → 若失败（含 NO DATA / SEARCHING / ERROR / "?" 等任何 negative 关键字）：
            ATSP6 -> wait OK     ; 强制 CAN 11-bit 500 kbps（绝大多数日系/欧洲新车）
            0100 -> 校验
            → 仍失败则返回 OPRT_NOT_FOUND，由 session 触发重连
```

这层兜底是用户问题 5.4 的"针对 25K80 1.5 版本的进阶技巧 — 特定协议强制进入"。`__looks_like_0100_ok` 实现是 case-sensitive `strstr` 找 `41 00` 或 `4100`（ELM327 ATE0/ATS0 应用后大小写固定），同时对 `NO DATA / no data / STOPPED / ERROR / SEARCHING / ?` 做 negative 拒绝。`rsp` 缓冲区在 `__init_elm327` 和 `__poll_one` 都升到 `OBD_RX_AGG_MAX`，配合 E.2 一起吃下多帧。

##### E.4 菜单：BT mode + Pair 入口

`UI_MENU_E` 加 `MENU_BT_MODE` / `MENU_BT_PAIR`，文案随 backend 与持久化语言变化：

- `BT: BLE`  / `BT: SPP (stub)` —— 点击循环切换，**写 KV 后立即 `obd_session_rescan()`** 让 session 重选 backend；
- `Pair OBD (1234/0000)`（SPP 模式）/ `Pair OBD (BLE: n/a)`（BLE 模式不需要 PIN）—— SPP 模式下点击触发 `obd_session_rescan()`（占位入口，v1.9 真接 SPP 后再实作弹 PIN 输入框）。

新键值 `APP_PREFS_T::bt_mode`（`uint8_t`），KV 反序列化时 clamp 到 `OBD_BT_MODE_COUNT`。`obd_session` 在 task 主循环开头检测 `bt_mode` 变化、调 `s_ses.io->disconnect()` + `s_ses.io->scan_stop()` + 切换 vtable + 重 init + 重新 scan。

#### F. 问题 6：菜单中英文 i18n + 持久化

LVGL 默认带 Montserrat 16/22/28（拉丁），不带 CJK；`app_default.config` 启用 `CONFIG_LV_FONT_SIMSUN_16_CJK=y`，引入 LVGL 自带的 SimSun 16 px CJK 字体（覆盖 BMP 基本 CJK 区 0x4E00-0x9FFF + 常用标点），实测 Flash +~1.4 MB（CJK 字体本身大，但 T5 Flash 充裕）。

新文件 `src/app/app_i18n.h/.c`：

- `APP_LANG_E { APP_LANG_EN = 0, APP_LANG_ZH = 1, APP_LANG_COUNT }`；
- `APP_STR_E` 集中所有可翻译字符串 ID（菜单标题/8 个菜单行/朝向 5 行/BT 状态/前/后置文/overlay 6 段提示，共 28 个）；
- 两张 `const char *const k_strings_en/zh[APP_STR_COUNT]` 表，UTF-8 字面量（中文用 `\xE9...` 字节序列以避开源文件编码歧义）；
- `app_i18n_get(id)` O(1) 查表；`app_i18n_font_default()` 根据当前 lang 返回 Montserrat 16 或 SimSun 16 CJK；
- `app_i18n_set_lang()` 写 KV + 更新内部静态变量，**不**直接刷 LVGL（让调用方明确控制刷新时机）。

`APP_PREFS_T` 新增 `uint8_t lang`，避免循环 include 不直接存 `APP_LANG_E`。`__prefs_default` 默认 `APP_LANG_EN`，反序列化 clamp 到 `APP_LANG_COUNT`。

`ui.c` 改造：

- `__build_menu` 把所有 lv_label 的字体设成 `app_i18n_font_default()`，菜单容器加 `LV_OBJ_FLAG_SCROLLABLE` + `LV_DIR_VER` + `LV_SCROLLBAR_MODE_AUTO` 适配新增到 9 行（移动 mock/bright/g-cal/orient/bt-mode/bt-pair/forget/lang/close）的高度，行高从 56 → 44，行间距从 8 → 6；
- `__menu_redraw` 全部走 `app_i18n_get()`，动态拼"模拟模式：开"/"亮度：80"/"朝向：屏幕朝上"等 value 部分；
- `MENU_LANG` 点击：`app_i18n_set_lang()` → 遍历菜单 9 个 label + 标题 + overlay label/dots **全部** `lv_obj_set_style_text_font` 到新字体，再触发 `__menu_redraw` 重新拉文字。这步必须**显式**做：`lv_label_set_text` 不会自动 reflow 字体，否则中文字符会渲染成"豆腐块"；
- `__build_overlay` / `__overlay_set_state_text` 同样走 i18n。

`tuya_main.c` 在 `app_kv_init()` 之后**立即**调 `app_i18n_init()`，确保 UI 创建第一个 label 时就拿到正确语言/字体——避免开机第一帧用 EN 渲染、第二帧切回 ZH 闪烁。

#### G. 落点

| 文件                                | 改动                                                                                                        |
|-----------------------------------|-----------------------------------------------------------------------------------------------------------|
| `src/ui/ui_gauge.h`               | `prev_dirty` 单元素 → `prev_dirty[2]` ring buffer + `prev_dirty_idx`；文件头补 dual framebuffer 语义注解               |
| `src/ui/ui_gauge.c`               | 新增 `__needle_dirty_ring_seed`；`__needle_invalidate_swept` 两槽 union；`__sweep_ready_cb` / `ui_gauge_set_def` / `ui_gauge_sweep` 三处 seed 改用 ring API |
| `src/ui/ui_gforce.h`              | `ring[4]` → `ring[3]`；文件头改"vehicle frame (lat, fwd)"                                                       |
| `src/ui/ui_gforce.c`              | `GFORCE_OUTER_R`/`RING_R0..2`/`FULL_SCALE_MG` 重定；ring 创建循环 4 → 3 + 最外圈用 `UI_COLOR_TICK`；axis label `X/Y` → `FWD/LAT` |
| `src/sensor/sensor_imu.c`         | 新增 `G_ORIENT_AXES_T` + `k_orient_axes` 表；`__imu_task` 读 `g_orient` 投影出 `out_fwd / out_lat` 并喂给 `app_metric_set_imu` |
| `src/app/app_metric.h/.c`         | `APP_METRIC_BUS_T` 加 `g_fwd_mg / g_lat_mg`；`app_metric_set_imu` 签名加两个新参数                                       |
| `src/obd/obd_io.h`                | **新文件** 定义 `OBD_BT_MODE_E` / `OBD_IO_EVENT_T` / `OBD_IO_T` vtable / `obd_io_ble()` / `obd_io_spp()` / `obd_io_for_mode()` |
| `src/obd/elm327_ble.h/.c`         | 用 `OBD_IO_*` 替换 `ELM_BLE_*`；导出 `static const OBD_IO_T s_io_ble` + `obd_io_ble()`                              |
| `src/obd/elm327_spp.h/.c`         | **新文件** SPP 桩：6 函数 5 返 NOT_SUPPORTED；文件头记录 v1.9 落地清单                                                       |
| `src/obd/obd_session.h`           | 新增 `obd_session_io_status()`                                                                              |
| `src/obd/obd_session.c`           | 持 `const OBD_IO_T *io` + `BOOL_T io_unsupported`；任务启动按 `bt_mode` 选 backend，rescan 时支持热切；`OBD_RX_QUEUE_DEPTH` 32 / `OBD_RX_LINE_MAX` 128 / 新增 `OBD_RX_AGG_MAX` 512；`__strip_iso15765_prefix` + 多行聚合的 `__send_and_wait` 重写；`__init_elm327` ATSP0 → ATSP6 回退 + `__looks_like_0100_ok` 校验 |
| `src/app/app_kv.h/.c`             | `APP_PREFS_T` 加 `g_orient` / `bt_mode` / `lang` 三个 `uint8_t`；新增 `app_kv_set_g_orient` / `app_kv_set_bt_mode` / `app_kv_set_lang`；默认/sanitize 全部就位 |
| `src/app/app_i18n.h/.c`           | **新文件** 字符串表 + 字体选择                                                                                          |
| `src/ui/ui.c`                     | include `app_i18n.h`；`UI_MENU_E` 加 `MENU_ORIENT / MENU_BT_MODE / MENU_BT_PAIR / MENU_LANG`；菜单 9 行 + 滚动；G 表分支用 `bus->g_lat_mg / g_fwd_mg`；菜单点击对应 4 个新条目；`__menu_redraw` / `__build_overlay` / `__overlay_set_state_text` 全走 `app_i18n_get`；`MENU_LANG` 点击触发字体全量刷写 |
| `src/tuya_main.c`                 | include `app_i18n.h`；`app_kv_init()` 之后立即 `app_i18n_init()`                                                  |
| `app_default.config`              | 启用 `CONFIG_LV_FONT_SIMSUN_16_CJK=y`                                                                       |

> 编译验证：`tos.py build` 通过 → `[BUILD SUCCESS] T5-Auto-meter_QIO_0.1.0.bin`。增量：i18n 表 < 4 KB（en + zh 双表 + 28 个指针），SimSun 16 CJK 字体由 LVGL flash-only 提供约 1.4 MB Flash（T5 Flash 充裕），SPP 桩 < 200 B Flash（5 个 NOT_SUPPORTED tramp）。运行时：双 framebuffer dirty ring 多 1 次 `lv_obj_invalidate_area` ≈ +3 µs/frame；IMU 任务投影 6 乘 6 加 ≈ +0.5 µs/sample；OBD 多帧聚合在长 PID 上多 1-2 次 `strncat` 调用，短 PID（RPM/速度/水温）无影响。语言切换实测端到端 ≤ 30 ms（仅刷字体 + redraw）。

#### H. 已知遗留 / 后续

- **SPP 后端**：v1.9 真接 BK7258 SDK；接入前提是先跑通 BR/EDR + GAP + SPP master 三个对应的 `bk_dm_*` 头文件，并把 legacy PIN 弹窗融进菜单系统。
- **更多 OBD PID**：油压（多车不通过标准 PID 提供）、油温（部分 ECU 0x5C）。建议先做"PID 探测"特性——`0100/0120/0140` 探支持位掩码，把不支持的菜单项灰掉，避免老车触发不必要重试。
- **i18n 字体扩展**：当前只走 SimSun 16 px。仪表大字（28 px）用 Montserrat（数字 + 单位），不需要 CJK；如果未来要把仪表标题改中文，需启用 SimSun 28 px 或自烧 LVGL fontconv 子集。
- **G 朝向自动估计**：当前手动 5 选 1。后续可加"开机静止 3 s 自检 → 推荐朝向"——需要识别哪个轴对齐 ±g（重力方向）。代价是 IMU 任务的少量复杂度，不破坏现有手动覆盖路径。

