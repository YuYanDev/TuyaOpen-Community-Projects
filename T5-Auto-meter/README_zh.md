# T5-Auto-meter

> 一台跑在 1.75" 圆形 AMOLED 上的复古风车载仪表盘，基于涂鸦 T5（Wi-Fi + BLE 5.0）。
> 通过蓝牙连接 ELM327 v1.5 OBD-II 适配器，把水温、油温、油量、机油压力、控制模块电压、
> 涡轮压力、横向 G 值渲染成仿机械指针，并附带开机扫针动画。

**简体中文** · [English](./README.md)

---

## ⚠️ 中国大陆用户特别提示（请务必阅读）

> **对于中国大陆用户：如果你的车为国产新能源汽车（特斯拉除外），可能无法支持数据
> 读取，并且有可能带来法律和牢狱风险，我们不推荐这类用户使用此项目。**
>
> 多数国产新能源车企会对OBD-II 诊断网关做权限隔离或加密。私自读取或干扰其 CAN 流量，可能：
>
> - **触发整车防篡改 / DTC 记录**，导致整车质保失效；
> - 被认定为**非法侵入计算机信息系统**（《刑法》第 285 / 286 条），或违反
>   《汽车数据安全管理若干规定》《道路交通安全法》等相关法规；
> - 招致**行政处罚乃至刑事责任**。
>
> **我们不建议上述用户在自己车辆上运行本项目。** 如果你确属此类用户仍要继续，
> 一切后果由你自行承担，本项目作者及维护者不承担任何法律或财务责任。
>
> 传统燃油车 / 油电混动车 / 进口品牌 / 特斯拉 通常开放标准 OBD-II Mode 01 PID，
> 是本项目的目标使用场景。

---

## 功能特性

- **466 × 466 圆形 AMOLED**，高 DPI 仿机械指针仪表（LVGL v9，`lv_scale` + `lv_line`
  指针 + `lv_anim` 平滑滑变）。
- **开机扫针动画** —— 1.1 秒左右，全量程 `min → max → min` 缓入缓出。
- **BLE 4.0 ELM327 透传**，按服务 UUID + 广播名自动识别 HM-10 / Vgate / Veepeak
  系适配器。**经典蓝牙（SPP）版 ELM327 不支持。**
- **OBD-II Mode 01 PID 轮询** —— 水温（`0x05`）、进气温（`0x0F`）、油量（`0x2F`）、
  控制模块电压（`0x42`）、引擎油温（`0x5C`）、涡轮压力（`0x70`）；G 值由板载
  QMI8658 计算。
- **蓝牙连接过渡动画** —— 开机扫针 → "等待 OBD 连接" 覆盖层（`lv_spinner` + 状态
  文案） → 蓝牙就绪后指针**平滑滑到真实值**。
- **Mock 模式** —— 没有适配器时也能跑：菜单中开启即生成正弦化模拟数据。
- **双按键** —— KEY 切换仪表 / 移动菜单光标；PWR 长按 1.2 s 打开 / 关闭菜单，长按
  3 s 优雅关机（释放 PWR_EN 自锁）。
- **偏好持久化**（TuyaOS KV）——仪表使能位图、当前仪表、亮度、Mock 标志、上次绑定的
  BLE MAC。

---

## 硬件清单

| 项目         | 取值                                                       |
|--------------|------------------------------------------------------------|
| 开发板       | Waveshare **T5-E1-Touch-AMOLED-1.75**                      |
| MCU          | 涂鸦 T5（Wi-Fi + BLE 5.0，双核 Cortex）                     |
| 屏幕         | 1.75 寸圆形 AMOLED，**466 × 466 RGB565**，QSPI（CO5300）    |
| 触摸         | I²C，CST92xx（GPIO 20/21 = SCL/SDA，复位 GPIO 42）          |
| IMU          | QMI8658 六轴 @ I²C `0x6B`，与触摸共用 I2C0                  |
| 按键         | PWR = GPIO 18（低有效），KEY = GPIO 12（低有效）            |
| 电源自锁     | PWR_EN = GPIO 19，高有效                                    |
| 电池采样     | 充电检测 GPIO 30、ADC15（GPIO 13），分压系数 2.51 / 0.51    |

OBD 适配器必须是 **BLE 4.0** 的 ELM327 v1.5（产品页一般标注 "BLE 4.0"、
"vLinker MC+"、"OBDLink CX"、"Veepeak BLE+"）。**经典蓝牙（SPP-only）不能用。**

---

## 编译

```bash
# 1) 拉起 TuyaOpen 工具链虚拟环境
cd ~/Project/TuyaOpen
. ./export.sh

# 2) 进入本工程编译
cd ~/Project/YuYanDev/TuyaOpen-Community-Projects/T5-Auto-meter
rm -rf .build dist
tos.py build
```

产物：

| 文件                                                  | 用途         |
|-------------------------------------------------------|--------------|
| `dist/T5-Auto-meter_0.1.0/T5-Auto-meter_QIO_0.1.0.bin` | 整片烧录     |
| `.build/bin/T5-Auto-meter_UA_0.1.0.bin`               | 应用固件包   |
| `.build/bin/T5-Auto-meter_UG_0.1.0.bin`               | OTA 升级包   |

USB 串口烧录（开机时按住 BOOT）：

```bash
tos.py flash
```

---

## 工程结构

```
T5-Auto-meter/
├── AGENT.md                     ← AI Agent 项目说明书（先读这个）
├── CMakeLists.txt
├── app_default.config           ← T5AI + Waveshare 1.75 + LVGL v9
├── include/app_config.h         ← 引脚 / 量程 / 周期单一事实源
└── src/
    ├── tuya_main.c              ← 入口；按顺序拉起各子系统
    ├── app/                     ← KV 偏好、metric 总线、Mock 生成器
    ├── board/                   ← 按键、I/O 互斥、电源自锁
    ├── sensor/                  ← QMI8658 驱动 + IMU 取样
    ├── obd/                     ← BLE 中心透传、ELM AT、PID 解析、会话状态机
    └── ui/                      ← LVGL 主题、仪表组件、顶层 UI
```

完整架构、UI 状态机、平台限制详见 [`AGENT.md`](./AGENT.md)。

---

## UI 流程

```
┌──────────────────┐  扫针 ~1.1s
│ BOOT_SWEEP       │
└────────┬─────────┘
         │ mock 已开 / OBD READY ────────────┐
         │                                   ▼
         │ 否则                     ┌─────────────┐
         ▼                          │ MAIN        │ ◄──── KEY = 下一仪表
┌──────────────────┐                │             │
│ WAIT_LINK        │  Spinner +     │             │ ◄──── PWR 长按 = 菜单
│ "Connecting OBD" │  状态文案       └──────┬──────┘
└────────┬─────────┘                       │
         │ OBD READY / mock 开启           │ LINK_LOST 且 mock 关
         └──────────► MAIN ◄───────────────┘   （指针平滑滑到真实值）
```

蓝牙就绪后的**第一个数据帧**会用 **900ms ease-in-out** 动画，让指针明显地从扫针停留
位置滑到真实值；之后回到常规 300ms 滑变。

---

## 按键

| 按键 | 短按              | 长按 1.2s     | 长按 3s        |
|------|-------------------|---------------|----------------|
| KEY  | 下一仪表 / 菜单光标下移 | （保留）       | （保留）        |
| PWR  | 激活当前菜单项     | 打开 / 关闭菜单 | 优雅关机       |

---

## 菜单

- **Mock Mode** —— 切换模拟数据源；切换后立即生效，UI 会从 `WAIT_LINK` 平滑过渡
  到 `MAIN`。
- **Brightness** —— 步进 +20%（100 → 20 回卷），写入 KV。
- **Cycle Gauges** —— 单仪表逐项启用占位（首版未实现，后续接 KV）。
- **Forget Adapter** —— 清除已绑定 BLE MAC 并触发重新扫描。
- **Back** —— 返回主显示。

---

## 已知限制

1. **T5AI 平台默认未开放 BLE Central** —— 涂鸦 NimBLE 移植中
   `TY_HS_BLE_ROLE_CENTRAL=0`，导致 `tkl_ble_gap_connect / tkl_ble_gattc_*`
   等中心角色 GATT 客户端符号未链接。我们在 `src/obd/ble_compat.c` 用
   `__attribute__((weak))` 提供 "返回 `OPRT_NOT_SUPPORTED`" 的兜底实现，让
   工程能链接、能跑、能扫到适配器，但**无法真正打开 GATT 通道**。当前请用
   菜单的 **Mock Mode** 离线驱动 UI；底层 SDK 一旦补齐强符号，弱实现自动失效。
2. **机油压力** 不在标准 Mode 01 PID 表内；多数车返回 `N/A`。OEM 私有
   Mode 22 PID 暂不在 v1 范围内。
3. **AMOLED 烧屏** —— 长时间高亮指针有老化风险。后续版本会加 5 分钟无数据
   自动降亮 + 指针缓慢漂移的"屏保"。

---

## 编码规范

本项目遵循仓库根 `always_applied_workspace_rules` 中的 TuyaOS C 风格 / 安全规范：

- 全部使用涂鸦类型（`UINT32_T`、`OPERATE_RET`、`STATIC`…）。
- 所有函数（含 static）必须有 Doxygen `@brief / @param / @return`。
- 内存：仅用 `tal_malloc / tal_free`，**禁止** `malloc / free`。
- 字符串：`snprintf` + `sizeof(buf)`，**禁止** `strcpy / sprintf`。
- 非 LVGL 线程访问 LVGL 必须通过 `lv_vendor_disp_lock/unlock`。
- 完整规则见 [`AGENT.md`](./AGENT.md) §8。

---

## License

跟随上游 TuyaOpen，采用 MIT 协议。

---

## 参考

- TuyaOpen 工具链文档：<https://www.tuyaopen.ai/zh/docs/about-tuyaopen>
- Waveshare 开发板页面：
  <https://developer.tuya.com/cn/docs/iot-device-dev/T5-E1-IPEX-development-board?id=Ke9xehig1cabj>
- OBD-II Mode 01 PID 表：<https://zh.wikipedia.org/wiki/OBD-II_PIDs>
- 国家相关法规：
  - 《中华人民共和国刑法》第 285 / 286 条
  - 《汽车数据安全管理若干规定（试行）》（国家网信办等五部门）
