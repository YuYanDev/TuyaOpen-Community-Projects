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
> (违反《刑法》第 285 条 / 286 条) or as a violation of vehicle data
> regulations (《汽车数据安全管理若干规定》);
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

- **466 × 466 round AMOLED** with high-DPI analog gauges (LVGL v9, custom-drawn
needle — single 14 px uniform line with a true semicircular tip cap, no internal
seams, C¹-continuous body→cap junction — sitting under a layered red / grey /
dark "metallic cap". Driven by a persistent 100 Hz EMA tracker plus a rotated-AABB
dirty-area redraw path that pushes the boot self-test sweep above 30 fps on the
SW renderer).
- **Needle visibility lifecycle** — the needle stays hidden until the first
real metric arrives, so users never see a stray red silhouette parked at the
dial minimum behind the translucent "waiting for BLE" overlay. First valid
reading snaps the needle onto the live position; subsequent KEY-cycles
between gauges keep the needle visible and glide smoothly to the next value.
- **Power-on needle sweep** (~1.1 s ease-in-out across the full dial range).
- **GoPro-style G-force reticle** — a separate widget (4 concentric rings @
0.25/0.5/0.75/1.0 g + N/S/E/W cardinal labels + crosshair + static red center
dot + live tracking ball) replaces the dial-style G gauge. The ball glides at
60 Hz EMA, never leaves the outer ring (1 g radius), and shows a `0.42 g` bold
heading + `X +0.32   Y −0.27` axis breakdown.
- **G-force zero calibration** — invoke "Calibrate G" from the menu in any
mounting orientation (vertical / tilted / sideways / 90° rotated); the current
static accelerometer vector is captured as the zero offset and persisted in KV,
so subsequent readings show only the actual lateral / longitudinal acceleration.
"Forget Calibration" reverts to the factory zero.
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
- **Buttons + touch** — **PWR short-press toggles the menu** at any time; **KEY
short-press cycles gauges** (works even while WAIT_LINK is on screen); the menu
itself is **touch-only** (no cursor); PWR long-press 3 s shuts the device down
cleanly (PWR_EN latch released).
- **Persistent prefs** stored in TuyaOS KV (gauge enable bitmask, current gauge,
brightness, mock flag, last bound BLE MAC).

---

## Hardware


| Item          | Value                                                   |
| ------------- | ------------------------------------------------------- |
| Board         | Waveshare **T5-E1-Touch-AMOLED-1.75**                   |
| MCU           | Tuya T5 (Wi-Fi + BLE 5.0, dual-core Cortex)             |
| Display       | 1.75" round AMOLED, **466 × 466 RGB565**, QSPI (CO5300) |
| Touch         | I²C, CST92xx (GPIO 20/21 = SCL/SDA, RST = GPIO 42)      |
| IMU           | QMI8658 6-axis @ I²C 0x6B, shares I2C0                  |
| Buttons       | PWR = GPIO 18 (low active), KEY = GPIO 12 (low active)  |
| Power latch   | PWR_EN = GPIO 19, high active                           |
| Battery sense | Charge = GPIO 30, ADC15 (GPIO 13), divider 2.51 / 0.51  |


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


| File                                                   | Use              |
| ------------------------------------------------------ | ---------------- |
| `dist/T5-Auto-meter_0.1.0/T5-Auto-meter_QIO_0.1.0.bin` | Full image flash |
| `.build/bin/T5-Auto-meter_UA_0.1.0.bin`                | Firmware only    |
| `.build/bin/T5-Auto-meter_UG_0.1.0.bin`                | OTA upgrade      |


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

See `[AGENT.md](./AGENT.md)` for the full architecture, UI state machine, and
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
│ WAIT_LINK        │  spinner +     │             │ ◄──── PWR short = MENU
│  "Connecting OBD"│  state text    └──────┬──────┘
└────────┬─────────┘                       │
         │ OBD READY / mock ON             │ LINK_LOST + mock OFF
         └──────────► MAIN ◄───────────────┘   (smooth needle slew)
```

When the link comes up, the very first sample is animated over **900 ms
ease-in-out** so the needle visibly glides from the rest position to the live
value. After that, a persistent 60 Hz tracker drives every refresh, so even
fast-changing readings stay buttery-smooth — no "start / stop" jitter.

---

## Buttons / touch


| Input         | Action                                                                                         |
| ------------- | ---------------------------------------------------------------------------------------------- |
| **PWR short** | **Toggle the menu** open / closed at any time (no long-press required)                         |
| PWR long 3 s  | `board_pwr_shutdown()` — clean power-off                                                       |
| **KEY short** | **Cycle to the next gauge**, also works while WAIT_LINK is on screen                           |
| **Touch tap** | **The menu is touch-only** — every menu item is a clickable card; buttons do not move a cursor |


---

## Menu (touch-only — PWR closes)

- **Mock Mode** (tap to toggle) — switch synthetic telemetry on/off; takes
effect immediately and smoothly transitions the UI between `WAIT_LINK` and
`MAIN`.
- **Brightness** (tap to step) — +20 % per tap, wraps 100 → 20, persisted to KV.
- **Forget Adapter** (tap to execute) — clear bound BLE MAC and trigger an OBD
rescan.
- **Calibrate G** (tap to zero) — capture the **current static accelerometer
vector** as the zero point of the G-force reticle. Place the device in your
preferred mounting orientation, keep it still, then tap; the offset is stored
in KV and applied to every IMU sample on subsequent boots.
- **Close** (tap to dismiss) — back to the live gauge; you can also press PWR.

---

## Known limitations

1. **T5AI BLE Central is not exposed** in the stock TuyaOpen build —
  `tkl_ble_gap_connect`, `tkl_ble_gattc`_* are gated behind
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
- See `[AGENT.md](./AGENT.md)` §8 for the full list.

---

## License

Released under the same license as TuyaOpen (MIT). See the parent repository.

---

## References

- TuyaOpen toolchain: [https://www.tuyaopen.ai/zh/docs/about-tuyaopen](https://www.tuyaopen.ai/zh/docs/about-tuyaopen)
- Waveshare T5-E1 board page:
[https://developer.tuya.com/cn/docs/iot-device-dev/T5-E1-IPEX-development-board?id=Ke9xehig1cabj](https://developer.tuya.com/cn/docs/iot-device-dev/T5-E1-IPEX-development-board?id=Ke9xehig1cabj)
- OBD-II Mode 01 PID list: [https://en.wikipedia.org/wiki/OBD-II_PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)

