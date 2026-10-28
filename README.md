# osd-brightness-bridge

[![build](https://github.com/zhao-ziyi/osd-brightness-bridge/actions/workflows/build.yml/badge.svg)](https://github.com/zhao-ziyi/osd-brightness-bridge/actions/workflows/build.yml)

> Windows: mirror the system brightness OSD value to external monitors via DDC/CI — make Fn brightness keys actually work.

Zero-UI · stateless · ~100 KB exe / ~2 MB RAM / 0% CPU · single native binary.

[中文说明](#中文)

## The problem

On a desktop PC with an external monitor, pressing the keyboard's brightness keys (e.g. Fn+F1/F2) pops the Windows brightness OSD and the bar moves — but the monitor's brightness never changes. Windows only paints the OSD; it never sends the value to the monitor.

## The idea

The OSD bar reflects a **live system value** stored in the power plan (`SUB_VIDEO\VIDEONORMALLEVEL`, 0–100%). This tiny background tool mirrors that value to the monitor via DDC/CI:

```
[Fn brightness key]
   │  Windows: shows OSD + updates its value   (free UI, zero code)
   ▼
Raw Input (Consumer 0x6F/0x70) → read VIDEONORMALLEVEL → SetMonitorBrightness
```

The system OSD *is* the UI. The tool keeps **no state of its own** — whatever the OSD shows is what the monitor gets.

## Features

- **Zero UI** — the system OSD is the feedback; nothing to draw, nothing to intercept, no hooks
- **Stateless mirror** — restart-proof, can never drift out of sync
- **Tiny** — single native exe, ~100 KB on disk, ~2 MB private RAM, idle CPU 0 (pure event-driven)
- **Tray icon** — right-click for *start with Windows* toggle and exit
- Works with any keyboard that sends HID Consumer brightness keys (tested: MCHOSE G75 V2, Fn+F1/F2)

## Requirements

- Windows 10 / 11
- A monitor with DDC/CI (MCCS) brightness support (VCP code `0x10`)
- A keyboard that emits HID Consumer usages `0x6F` (brightness increment) / `0x70` (brightness decrement)

## Build

Visual Studio Build Tools + Windows SDK:

```bat
cl /O2 /W3 /utf-8 bridge.c /link user32.lib powrprof.lib dxva2.lib shell32.lib advapi32.lib /SUBSYSTEM:WINDOWS
```

## Usage

Run `bridge.exe`. It syncs brightness once at startup, then sits in the tray and watches for brightness keys. Right-click the tray icon for:

- **开机自启** (start with Windows) — toggles an `HKCU\...\Run` entry
- **退出** (exit)

## How it works

1. Listen on the HID Consumer page via Raw Input — no hooks, nothing intercepted
2. On a brightness key event (debounced 120 ms), read the value Windows just wrote to the power plan
3. Write it to every DDC/CI-capable monitor via `SetMonitorBrightness`

Verified empirically: the power-plan value tracks the OSD (step of 10% per press), and Windows does **not** forward it to the monitor over DDC/CI — that final hop is exactly what this tool adds.

## License

[MIT](LICENSE)

---

## 中文

把系统亮度弹窗的值通过 DDC/CI 镜像给外接显示器——让 Fn 亮度键在外接屏上真正生效。

**问题**：台式机 + 外接显示器时，按 Fn 亮度键系统会弹 OSD、条也会动，但显示器亮度纹丝不动——系统只画弹窗，不把值发给显示器。

**思路**：OSD 弹窗条对应的是电源方案里的活值（`VIDEONORMALLEVEL`，0~100%）。本工具只做「无状态镜像」：捕获按键 → 读系统值 → 写给显示器。**系统弹窗免费当 UI**，程序自己不记任何状态，弹窗显示什么显示器就是什么。

**特性**：无界面 / 无状态 / 单文件约 100 KB、私有内存约 2 MB、空闲 CPU 0 / 托盘右键可设开机自启与退出 / 不挂钩子不拦截。

**编译与使用**：见上方 Build / Usage；运行后驻留托盘，按 Fn 亮度键即可看到显示器跟随 OSD 变化。
