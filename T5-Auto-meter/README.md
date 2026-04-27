# T5-Auto-meter

> A retro-style automotive gauge cluster running on a 1.75" round AMOLED, built on
> Tuya T5 (Wi-Fi + BLE 5.0). Connects to a BLE ELM327 v1.5 OBD-II adapter and renders
> coolant temp, oil temp, fuel level, oil pressure, control-module voltage, boost
> pressure and lateral G-force as analog needle gauges with a power-on "needle sweep"
> animation.

[简体中文](./README_zh.md) · **English**

---

## ⚠️ Important Notice for Users in Mainland China

> **For users in mainland China:** if your car is a **Chinese-brand new-energy
> vehicle (NEV/EV/PHEV/REEV) — Tesla excluded — this project may not be able to read OBD data at all, and may put you at legal and even criminal risk.**
>
> Many Chinese OEMs restrict or
> encrypt the OBD-II diagnostic gateway. Reading or interfering with their CAN
> traffic can:
>
> - Trigger an **anti-tamper / DTC log** that voids your warranty;
> - Be classified as **illegal intrusion into a computer information system**
>   (违反《刑法》第 285 条 / 286 条) or as a violation of vehicle data
>   regulations (《汽车数据安全管理若干规定》);
> - Lead to **administrative penalties or imprisonment**.
>
> **We strongly DO NOT recommend this group of users run this project on
> their own vehicle.** If you fall into this category and still proceed, you
> do so at your own risk; the authors and maintainers accept no liability.
>
> Conventional ICE / hybrid vehicles, imported brands, and Tesla typically expose
> the standard OBD-II Mode 01 PIDs and are the intended target of this project.

---

## Features

- **466 × 466 round AMOLED** with high-DPI analog gauges (LVGL v9, `lv_scale` +
  `lv_line` needle, smooth `lv_anim` slew).
- **Power-on needle sweep** (~1.1 s ease-in-out across the full dial range).
- **BLE 4.0 ELM327 transport** — auto-discovers HM-10 / Vgate / Veepeak class
  adapters by service UUID and adv name. Classic-Bluetooth (SPP) ELM327 is **not**
  supported.
- **OBD-II Mode 01 PID poller** — round-robin fetches: ECT (`0x05`), IAT (`0x0F`),
  Fuel Level (`0x2F`), Module Voltage (`0x42`), Engine Oil Temp (`0x5C`), Boost
  Pressure (`0x70`); G-force from on-board QMI8658 IMU.
- **Bluetooth-link transition animation** — boot sweep → "waiting for OBD"
  spinner overlay → smooth needle slew to the live reading once linked.
- **Mock mode** — generate plausible synthetic telemetry from the menu when no
  adapter is available.
- **Dual buttons** — KEY cycles gauges or moves the menu cursor; PWR
  long-press 1.2 s opens / closes the menu, long-press 3 s shuts down (PWR_EN
  latch released).
- **Persistent prefs** stored in TuyaOS KV (gauge enable bitmask, current gauge,
  brightness, mock flag, last bound BLE MAC).

---

## Hardware

| Item            | Value                                                      |
|-----------------|------------------------------------------------------------|
| Board           | Waveshare **T5-E1-Touch-AMOLED-1.75**                      |
| MCU             | Tuya T5 (Wi-Fi + BLE 5.0, dual-core Cortex)                |
| Display         | 1.75" round AMOLED, **466 × 466 RGB565**, QSPI (CO5300)    |
| Touch           | I²C, CST92xx (GPIO 20/21 = SCL/SDA, RST = GPIO 42)         |
| IMU             | QMI8658 6-axis @ I²C 0x6B, shares I2C0                     |
| Buttons         | PWR = GPIO 18 (low active), KEY = GPIO 12 (low active)     |
| Power latch     | PWR_EN = GPIO 19, high active                              |
| Battery sense   | Charge = GPIO 30, ADC15 (GPIO 13), divider 2.51 / 0.51     |

The OBD-II adapter must be a **BLE 4.0** ELM327 v1.5 dongle (look for
"BLE 4.0", "vLinker MC+", "OBDLink CX", "Veepeak BLE+"). Classic-Bluetooth /
SPP-only adapters will not work.

---

## Build

```bash
# 1) Activate the TuyaOpen environment (creates / refreshes a venv)
cd ~/Project/TuyaOpen
. ./export.sh

# 2) Build this project
cd ~/Project/YuYanDev/TuyaOpen-Community-Projects/T5-Auto-meter
rm -rf .build dist
tos.py build
```

Outputs:

| File                                                 | Use            |
|------------------------------------------------------|----------------|
| `dist/T5-Auto-meter_0.1.0/T5-Auto-meter_QIO_0.1.0.bin` | Full image flash |
| `.build/bin/T5-Auto-meter_UA_0.1.0.bin`              | Firmware only  |
| `.build/bin/T5-Auto-meter_UG_0.1.0.bin`              | OTA upgrade    |

To flash via USB serial (hold BOOT while powering on):

```bash
tos.py flash
```

---

## Project layout

```
T5-Auto-meter/
├── AGENT.md                     ← AI agent contract (read this first)
├── CMakeLists.txt
├── app_default.config           ← T5AI + Waveshare 1.75 + LVGL v9
├── include/app_config.h         ← pinout / ranges / timings
└── src/
    ├── tuya_main.c              ← entry, brings up the stack in order
    ├── app/                     ← KV prefs, metric bus, mock generator
    ├── board/                   ← buttons, I/O mutex, power latch
    ├── sensor/                  ← QMI8658 driver + IMU sampler
    ├── obd/                     ← BLE central transport, ELM AT, PID parser, session FSM
    └── ui/                      ← LVGL theme, gauge widget, top-level UI
```

See [`AGENT.md`](./AGENT.md) for the full architecture, UI state machine, and
known platform limitations.

---

## UI flow

```
┌──────────────────┐  needle sweep ~1.1 s
│ BOOT_SWEEP       │
└────────┬─────────┘
         │ mock=ON or OBD READY ─────────────┐
         │                                   ▼
         │ otherwise                ┌─────────────┐
         ▼                          │ MAIN        │ ◄──── KEY = next gauge
┌──────────────────┐                │             │
│ WAIT_LINK        │  spinner +     │             │ ◄──── PWR long = MENU
│  "Connecting OBD"│  state text    └──────┬──────┘
└────────┬─────────┘                       │
         │ OBD READY / mock ON             │ LINK_LOST + mock OFF
         └──────────► MAIN ◄───────────────┘   (smooth needle slew)
```

When the link comes up, the very first sample is animated over **900 ms
ease-in-out** so the needle visibly glides from the rest position to the live
value.

---

## Buttons

| Button | Short        | Long 1.2 s    | Long 3 s             |
|--------|--------------|---------------|----------------------|
| KEY    | Next gauge / menu cursor down | (reserved)    | (reserved)           |
| PWR    | Activate menu item            | Open / close menu | Graceful shutdown |

---

## Menu

- **Mock Mode** — toggle synthetic telemetry; takes effect immediately and
  smoothly transitions the UI from `WAIT_LINK` to `MAIN`.
- **Brightness** — step 20 % (wraps 100 → 20), persisted to KV.
- **Cycle Gauges** — placeholder for per-gauge enable toggles.
- **Forget Adapter** — clear bound BLE MAC and trigger an OBD rescan.
- **Back** — return to `MAIN`.

---

## Known limitations

1. **T5AI BLE Central is not exposed** in the stock TuyaOpen build —
   `tkl_ble_gap_connect`, `tkl_ble_gattc_*` are gated behind
   `TY_HS_BLE_ROLE_CENTRAL=0`. We provide weak fall-back stubs in
   `src/obd/ble_compat.c` so the firmware links and runs; on real hardware
   the adapter will be discovered but the GATT pipe will not open. Use
   **Mock Mode** to drive the UI for now. When the platform port enables
   central role, the strong symbols win automatically and OBD will work
   without code changes.
2. **Engine oil pressure** (`APP_METRIC_OIL_PRESSURE`) is not part of the
   standard Mode 01 PID set. Most vehicles return `N/A`; OEM-specific Mode
   22 PIDs are out of scope for v1.
3. **AMOLED burn-in** — bright stationary needles can age the panel. A
   future revision will dim and slowly wander the needle after 5 min idle.

---

## Coding rules

This repository follows the TuyaOS C style and security rules in
`always_applied_workspace_rules` at the repo root:

- All identifiers use Tuya types (`UINT32_T`, `OPERATE_RET`, `STATIC`, ...).
- All functions, including statics, have Doxygen `@brief / @param / @return`.
- Memory: `tal_malloc / tal_free` only; no raw `malloc / free`.
- Strings: `snprintf` with `sizeof(buf)`; no `strcpy / sprintf`.
- LVGL access from non-LVGL threads goes through `lv_vendor_disp_lock/unlock`.
- See [`AGENT.md`](./AGENT.md) §8 for the full list.

---

## License

Released under the same license as TuyaOpen (MIT). See the parent repository.

---

## References

- TuyaOpen toolchain: <https://www.tuyaopen.ai/zh/docs/about-tuyaopen>
- Waveshare T5-E1 board page:
  <https://developer.tuya.com/cn/docs/iot-device-dev/T5-E1-IPEX-development-board?id=Ke9xehig1cabj>
- OBD-II Mode 01 PID list: <https://en.wikipedia.org/wiki/OBD-II_PIDs>
