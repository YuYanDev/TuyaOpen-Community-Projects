# T5-Auto-meter

A retro-style **circular OBD-II vehicle meter** for the Waveshare **T5-E1-Touch-AMOLED-1.75** development board. Connects to an ELM327 v1.5 BLE adapter over Bluetooth Low Energy, polls the ECU in real time, and renders the readings as analog-style needle gauges on a 466 × 466 round AMOLED display.

> 中文版本请见 [README_zh.md](./README_zh.md)

---

## ⚠️ Important notice for users in mainland China

> **If your vehicle is a domestically-produced new-energy vehicle (excluding Tesla), this project may not be able to read data from it, and using it may carry legal and even custodial risks. We do not recommend that this group of users adopt this project.**
>
> Always check your local regulations regarding OBD-II diagnostics, on-vehicle aftermarket adapters, and short-range radio devices **before** flashing or installing.

---

## ✨ Features

- **Eight gauges** with smooth needle motion and a "self-test" sweep at boot:
  - Water Temperature (ECT)
  - Intake Air Temperature (IAT)
  - Engine Oil Temperature (EOT)
  - Fuel Level
  - Engine Oil Pressure *(reserved — most ECUs do not expose this via standard PIDs)*
  - Control Module Voltage
  - Turbo / Boost Pressure
  - G-force (from the on-board QMI8658 IMU; works offline)
- **OBD-II over BLE 4.0 GATT** — auto-discovers HM-10 / RN4870 / Vgate / NUS style transparent BLE serial services, then runs the standard ELM327 AT init (`ATZ → ATE0 → ATL0 → ATS0 → ATH0 → ATSP0 → 0100`) with an ATSP6 fallback for stubborn ECUs.
- **Mock mode** for bench testing without a vehicle — generates plausible sinusoidal data per gauge range, persisted across reboots.
- **2-button control + touch menu**:
  - **PWR (left)** — short-press to open / close the menu, long-press 3 s to power off (latched 5 V rail).
  - **KEY (right)** — short-press to cycle through gauges (works in any state, including BLE-waiting).
  - **Touch** — drives all menu interactions.
- **i18n**: English / 简体中文, persisted; Chinese rendered with a custom NotoSansSC subset font (no LVGL CJK tofu).
- **Smooth UX**:
  - 1.32 s eased boot sweep, 167 Hz LVGL refresh during the sweep, 5 ms angle interpolation.
  - 200 Hz EMA needle tracker with velocity capping → no snap, no stutter on data updates.
  - Dual-framebuffer dirty-region tracking (4-deep ring + MIN-pose anchor) to eliminate residual artefacts on AMOLED.
- **G-force algorithm** with high-pass filter: a fast EMA tracks instantaneous acceleration, a slow EMA (τ ≈ 20 s) dynamically tracks gravity across pose changes — the displayed value is dynamic acceleration only, settles back to ~0 g at rest no matter how the device is oriented. Calibration snaps the gravity vector to the current pose.
- **Persistent preferences** (KV-store): mock mode, brightness, language, current gauge, G-offset/seed, bound BLE adapter MAC, BT mode, G-orientation.

---

## 📷 Screens

| State | Description |
|---|---|
| **Boot sweep** | Logo fade-in → needle sweeps min → max → min over ~1.3 s |
| **Waiting for BLE** | Spinner + "Searching for ELM327 BLE…", needle parked at MIN; KEY still cycles gauges |
| **Live** | Selected gauge with title / unit / live numeric value at centre |
| **Menu** | Touch-driven list: Mock, Brightness, Forget Adapter, Calibrate G, Language, BT Mode, Back |

---

## 🔩 Hardware

| Component | Notes |
|---|---|
| Board | Waveshare **T5-E1-Touch-AMOLED-1.75** (Tuya T5 MCU) |
| Display | 1.75" round AMOLED, 466 × 466, RGB565 over QSPI (CO5300) |
| Touch | I²C, CST92xx |
| IMU | QMI8658 (I²C 0x6B), shared with touch on I²C0 |
| Buttons | PWR (GPIO 18, active low), KEY (GPIO 12, active low) |
| Power latch | GPIO 19 (active high) — long-press PWR pulls it low to power off |
| Battery | Charge detect GPIO 30, V-monitor on ADC15 (GPIO 13), divider 2.51 / 0.51 |
| OBD adapter | **Required: ELM327 v1.5 BLE 4.0 (GATT)**. Classic Bluetooth (SPP / RFCOMM) adapters are **not** supported by the current platform stack. |

> Pinout single-source-of-truth: [`include/app_config.h`](./include/app_config.h)

---

## 🛠️ Build & Flash

### 1. Toolchain

```bash
cd ~/Project/TuyaOpen
. ./export.sh        # source it — it brings up the venv
```

### 2. Build

```bash
cd <repo>/T5-Auto-meter
rm -rf .build dist
tos.py build
```

Successful output lands in `dist/T5-Auto-meter_0.1.0/` as `*_QIO_*.bin / *_UA_*.bin / *_UG_*.bin`.

### 3. Flash

```bash
tos.py flash         # USB serial; hold BOOT while powering up
```

Refer to the official [TuyaOpen documentation](https://www.tuyaopen.ai/zh/docs/about-tuyaopen) for board-specific flashing details.

---

## 🎮 Usage

### Pairing an ELM327 BLE adapter

1. Power up the device. After the boot sweep completes, the screen shows a "Searching for ELM327 BLE…" spinner.
2. Power up your **BLE-version** ELM327 (most adapters wake when the OBD-II port has 12 V, e.g. ignition or vehicle running).
3. The first BLE adapter that matches a known transparent-serial service UUID will be bound and saved to flash. Reboot to skip the scan next time.
4. To re-pair, open the menu (**PWR short-press**), then tap **Forget Adapter**.

### Cycling gauges

Press **KEY (right)** at any time to advance to the next gauge in the active list. Selection is persisted.

### Menu

Press **PWR (left)** short to open. Tap any item to act on it; tap **Back** or press **PWR** again to close.

| Item | Effect |
|---|---|
| Mock Mode | Toggle bench-test data generator on/off |
| Brightness | +20 % step (100 % → 20 % wrap) |
| Forget Adapter | Clear bound BLE address, restart scan |
| Calibrate G | Snap the gravity vector to the current pose; displayed G then settles to ~0 |
| Language | English ↔ 简体中文 |
| BT Mode | BLE 4.0 GATT (default) — Classic SPP entry is a stub for future platform support |
| Back | Close menu |

### Power off

Long-press **PWR** for ~3 s — the GPIO 19 latch is released and the device powers down.

---

## 📁 Project layout

```
T5-Auto-meter/
├── include/
│   └── app_config.h            # Pinout & global constants (single source of truth)
├── src/
│   ├── tuya_main.c             # tuya_app_main entry, brings up all tasks
│   ├── app/
│   │   ├── app_config.h
│   │   ├── app_i18n.{c,h}      # English / 简体中文 string tables
│   │   ├── app_kv.{c,h}        # Preferences persistence (tal_kv)
│   │   ├── app_metric.{c,h}    # metric_bus — thread-safe data centre
│   │   └── app_mock.{c,h}      # Sinusoidal mock data generator
│   ├── board/
│   │   ├── board_btn.{c,h}     # Dual-button event broker
│   │   ├── board_io.{c,h}      # I²C / ADC / charge detect
│   │   └── board_pwr.{c,h}     # PWR_EN latch / soft power-off
│   ├── sensor/
│   │   ├── qmi8658.{c,h}       # IMU register driver
│   │   └── sensor_imu.{c,h}    # Fast/slow EMA gravity tracker, dynamic-G output
│   ├── obd/
│   │   ├── obd_io.h            # Backend vtable (BLE / SPP)
│   │   ├── elm327_ble.{c,h}    # BLE 4.0 GATT transport
│   │   ├── elm327_spp.{c,h}    # SPP stub (platform support pending)
│   │   ├── ble_compat.c        # Weak fallbacks for missing platform BLE-central symbols
│   │   ├── obd_pid.{c,h}       # Mode 01 PID parsers
│   │   └── obd_session.{c,h}   # AT init flow, multi-frame aggregation, polling
│   └── ui/
│       ├── ui_theme.h          # Colour & angle constants
│       ├── ui_gauge.{c,h}      # Custom circular needle gauge (LV_EVENT_DRAW_MAIN)
│       ├── ui_gforce.{c,h}     # 2-axis G-circle widget
│       ├── ui.{c,h}            # Top-level FSM, menu, BLE-wait overlay
│       └── fonts/
│           ├── lv_font_zh_16.{c,h}  # NotoSansSC subset (menu / overlay)
│           └── OFL.txt
├── app_default.config          # Kconfig snippet (board + LVGL fonts)
├── CMakeLists.txt
├── AGENT.md                    # Full design & engineering notes
└── README*.md
```

---

## 📐 Supported PIDs

| PID | Field | Formula | Unit |
|---|---|---|---|
| 0x05 | Engine coolant temperature | A − 40 | °C |
| 0x0B | Intake manifold pressure | A | kPa |
| 0x0F | Intake air temperature | A − 40 | °C |
| 0x2F | Fuel level | 100 × A / 255 | % |
| 0x33 | Barometric pressure | A | kPa |
| 0x42 | Control module voltage | ((A × 256) + B) / 1000 | V |
| 0x5C | Engine oil temperature | A − 40 | °C |
| Boost | Turbo (computed) | MAP − BARO | kPa |

The ECU determines actual support — gauges with no live data display `--`.

---

## 🚧 Known limitations

- **Engine oil pressure** is not in the standard OBD-II PID set; we display `--` until a future Mode 22 vendor-specific PID table lands.
- **Bluetooth Classic (SPP)** ELM327 adapters are not supported (T5 platform doesn't expose SPP in current SDK). `obd/elm327_spp.c` is a vtable-compatible stub.
- **Domestic Chinese new-energy vehicles** often re-implement or hide the diagnostic layer — see warning above.

---

## 📜 License & credits

- Source code under this directory: see project root LICENSE.
- LVGL — see `src/liblvgl/LICENSE` in TuyaOpen.
- NotoSansSC subset — Open Font License 1.1 (`src/ui/fonts/OFL.txt`).
- TuyaOpen / T5AI platform: <https://www.tuyaopen.ai>
- ELM327 protocol reference: <https://en.wikipedia.org/wiki/OBD-II_PIDs>

---

## 🤖 For contributors / AI agents

Engineering notes, file conventions, FSM diagrams, change log, and project rules are in [AGENT.md](./AGENT.md). Read it **before** sending PRs or asking an AI to refactor code.
