# T5-Auto-meter

基于 Waveshare **T5-E1-Touch-AMOLED-1.75** 开发板的**圆形 OBD-II 车载仪表**。通过低功耗蓝牙连接 ELM327 v1.5 BLE 适配器，实时读取车辆 ECU 数据，并以仿机械指针的方式渲染到 466 × 466 的圆形 AMOLED 屏幕上。

> English version: [README.md](./README.md)

---

## ⚠️ 中国大陆用户特别提示

> **如果你的车为国产新能源汽车（特斯拉除外），可能无法支持数据读取，并且有可能带来法律和牢狱风险，我们不推荐这类用户使用此项目。**
>
> 在烧录或安装本项目前，请务必先了解所在地区关于 **OBD-II 诊断接口、车载后装设备、短距离无线设备**的相关法规。

---

## ✨ 功能特性

- **8 种仪表**，开机有"扫针自检"动画，运行中指针平滑过渡：
  - 水温（ECT）
  - 进气温（IAT）
  - 引擎油温（EOT）
  - 油量
  - 机油压力 *（标准 OBD-II 不提供，预留）*
  - 控制模块电压
  - 涡轮压力（Boost）
  - G 值（来自板载 QMI8658 IMU，离线也能用）
- **OBD-II over BLE 4.0 GATT** —— 自动识别 HM-10 / RN4870 / Vgate / NUS 风格透传服务，连上后跑标准 ELM327 AT 序列（`ATZ → ATE0 → ATL0 → ATS0 → ATH0 → ATSP0 → 0100`），失败自动回退 ATSP6。
- **Mock 模式**用于桌面无车测试 —— 按各仪表量程生成正弦化数据，开关持久化。
- **双键 + 触屏菜单**：
  - **PWR（左）** —— 短按打开 / 关闭菜单；长按 3 秒关机（5 V 自锁断电）
  - **KEY（右）** —— 短按切换下一仪表（任何状态可用，包括蓝牙等待中）
  - **触屏** —— 菜单内所有交互均通过点击
- **中英文切换**，持久化；中文使用自定义 NotoSansSC 子集字体（无 LVGL 方块字问题）
- **流畅的视觉体验**：
  - 1.32 秒 ease-in-out 开机扫针，sweep 期间 LVGL refresh 推到 167 Hz，5 ms 角度插值
  - 200 Hz EMA 指针追踪 + 速度限幅 → 数据更新无跳变、无卡顿
  - 双缓冲脏区追踪（4 帧 ring + MIN 锚点），AMOLED 上无残影
- **G 值算法**采用高通滤波：快速 EMA 跟踪瞬时加速度，慢速 EMA（τ ≈ 20 s）动态跟踪重力方向，显示值仅为动态加速度，**任何姿态下静止都会回到 ~0 g**。"校准 G"将重力向量瞬间锁定到当前姿态。
- **配置持久化**（KV 存储）：mock 模式、亮度、语言、当前仪表、G 偏置/种子、绑定的 BLE 适配器 MAC、蓝牙模式、G 朝向。

---

## 📷 界面状态

| 状态 | 说明 |
|---|---|
| **开机自检** | Logo 淡入 → 指针扫 min → max → min，约 1.3 秒 |
| **等待蓝牙** | 旋转图标 + "正在搜索 ELM327 BLE…"，指针停在 MIN，KEY 仍可切表 |
| **运行中** | 当前仪表，标题/单位/中央实时数值 |
| **菜单** | 触屏列表：Mock、亮度、解绑设备、校准 G、语言、蓝牙模式、返回 |

---

## 🔩 硬件清单

| 部件 | 备注 |
|---|---|
| 开发板 | Waveshare **T5-E1-Touch-AMOLED-1.75**（涂鸦 T5 MCU） |
| 屏幕 | 1.75" 圆形 AMOLED，466 × 466，RGB565 QSPI（CO5300） |
| 触摸 | I²C，CST92xx |
| IMU | QMI8658（I²C 0x6B），与触摸共用 I²C0 |
| 按键 | PWR（GPIO 18，低有效）、KEY（GPIO 12，低有效） |
| 电源自锁 | GPIO 19（高有效）—— PWR 长按拉低后关机 |
| 电池 | 充电检测 GPIO 30、电压采样 ADC15（GPIO 13），分压系数 2.51 / 0.51 |
| OBD 适配器 | **必须为 ELM327 v1.5 BLE 4.0（GATT）版本**。经典蓝牙（SPP / RFCOMM）适配器**不支持**（T5 平台栈未开放 SPP） |

> 引脚单一事实源：[`include/app_config.h`](./include/app_config.h)

---

## 🛠️ 编译与烧录

### 1. 工具链准备

```bash
cd ~/Project/TuyaOpen
. ./export.sh        # 必须用 source（拉起 venv）
```

### 2. 编译

```bash
cd <repo>/T5-Auto-meter
rm -rf .build dist
tos.py build
```

成功后产物在 `dist/T5-Auto-meter_0.1.0/`：`*_QIO_*.bin / *_UA_*.bin / *_UG_*.bin`。

### 3. 烧录

```bash
tos.py flash         # USB 串口；按住 BOOT 上电
```

具体烧录步骤详见 [TuyaOpen 官方文档](https://www.tuyaopen.ai/zh/docs/about-tuyaopen)。

---

## 🎮 使用方法

### 配对 ELM327 BLE 适配器

1. 上电，开机自检结束后屏幕显示"正在搜索 ELM327 BLE…"。
2. 给 **BLE 版** ELM327 上电（多数适配器需要 OBD-II 接口有 12 V 才会唤醒，例如点火 / 启动车辆）。
3. 第一个匹配已知透传服务 UUID 的 BLE 适配器会被绑定并写入 flash，下次开机自动连接，跳过扫描。
4. 需要重新配对：短按 **PWR** 打开菜单 → 点 **解绑设备**。

### 切换仪表

任何时候按 **KEY（右）** 切到列表中的下一个仪表，选择会被持久化。

### 菜单

短按 **PWR（左）** 打开。点击任意条目执行；点 **返回** 或再按 **PWR** 关闭。

| 项 | 行为 |
|---|---|
| Mock 模式 | 切换桌面测试数据生成器 |
| 亮度 | 步进 +20 %（100 % → 20 % 卷回） |
| 解绑设备 | 清除已绑定的 BLE 地址，重新扫描 |
| 校准 G | 把重力向量瞬间锁定到当前姿态，显示 G 立刻回到 ~0 |
| 语言 | English ↔ 简体中文 |
| 蓝牙模式 | BLE 4.0 GATT（默认）—— Classic SPP 入口为预留 stub |
| 返回 | 关闭菜单 |

### 关机

长按 **PWR** 约 3 秒 —— GPIO 19 自锁释放，设备断电。

---

## 📁 项目结构

```
T5-Auto-meter/
├── include/
│   └── app_config.h            # 引脚 / 全局常量（单一事实源）
├── src/
│   ├── tuya_main.c             # tuya_app_main 入口，串起所有任务
│   ├── app/
│   │   ├── app_i18n.{c,h}      # 英文 / 简体中文 字符串表
│   │   ├── app_kv.{c,h}        # 偏好持久化（tal_kv）
│   │   ├── app_metric.{c,h}    # metric_bus —— 带锁的数据中心
│   │   └── app_mock.{c,h}      # 正弦化 Mock 数据生成器
│   ├── board/
│   │   ├── board_btn.{c,h}     # 双按键事件代理
│   │   ├── board_io.{c,h}      # I²C / ADC / 充电检测
│   │   └── board_pwr.{c,h}     # PWR_EN 自锁 / 软关机
│   ├── sensor/
│   │   ├── qmi8658.{c,h}       # IMU 寄存器驱动
│   │   └── sensor_imu.{c,h}    # 快慢双 EMA 重力跟踪 + 动态 G 输出
│   ├── obd/
│   │   ├── obd_io.h            # 后端 vtable（BLE / SPP）
│   │   ├── elm327_ble.{c,h}    # BLE 4.0 GATT 传输
│   │   ├── elm327_spp.{c,h}    # SPP stub（待平台支持）
│   │   ├── ble_compat.c        # 平台缺失的 BLE central 符号弱兜底
│   │   ├── obd_pid.{c,h}       # Mode 01 PID 解析
│   │   └── obd_session.{c,h}   # AT 初始化 / 多帧聚合 / 周期轮询
│   └── ui/
│       ├── ui_theme.h          # 颜色、角度常量
│       ├── ui_gauge.{c,h}      # 自绘圆形指针仪表（LV_EVENT_DRAW_MAIN）
│       ├── ui_gforce.{c,h}     # 二维 G 圈控件
│       ├── ui.{c,h}            # 顶层 FSM、菜单、蓝牙等待覆盖层
│       └── fonts/
│           ├── lv_font_zh_16.{c,h}  # NotoSansSC 子集（菜单 / 覆盖文案）
│           └── OFL.txt
├── app_default.config          # Kconfig 片段（板子 + LVGL 字体）
├── CMakeLists.txt
├── AGENT.md                    # 完整设计与工程笔记
└── README*.md
```

---

## 📐 已支持的 PID

| PID | 字段 | 公式 | 单位 |
|---|---|---|---|
| 0x05 | 发动机水温（ECT） | A − 40 | °C |
| 0x0B | 进气歧管压力（MAP） | A | kPa |
| 0x0F | 进气温度（IAT） | A − 40 | °C |
| 0x2F | 油量 | 100 × A / 255 | % |
| 0x33 | 大气压力（BARO） | A | kPa |
| 0x42 | 控制模块电压 | ((A × 256) + B) / 1000 | V |
| 0x5C | 引擎油温（EOT） | A − 40 | °C |
| Boost | 涡轮压力（计算值） | MAP − BARO | kPa |

实际是否能读取由车辆 ECU 决定 —— 无数据的仪表显示 `--`。

---

## 🚧 已知限制

- **机油压力**不在标准 OBD-II PID 集合中，统一显示 `--`，等后续做 Mode 22 厂商扩展 PID 表
- **经典蓝牙（SPP）** 版 ELM327 不支持（T5 平台 SDK 未开放 SPP）；`obd/elm327_spp.c` 仅为 vtable 兼容 stub
- 国产**新能源车**多在诊断层做了重写或屏蔽 —— 详见上方警告

---

## 📜 许可与致谢

- 本目录代码：见仓库根 LICENSE
- LVGL：见 TuyaOpen 中 `src/liblvgl/LICENSE`
- NotoSansSC 子集：Open Font License 1.1（`src/ui/fonts/OFL.txt`）
- TuyaOpen / T5AI 平台：<https://www.tuyaopen.ai>
- ELM327 协议参考：<https://en.wikipedia.org/wiki/OBD-II_PIDs>

---

## 🤖 给 Contributor / AI Agent

工程笔记、文件命名约定、FSM 流程图、变更日志、项目规则全部在 [AGENT.md](./AGENT.md)。在提 PR 或让 AI 重构代码**之前**，请先读完 AGENT.md。
