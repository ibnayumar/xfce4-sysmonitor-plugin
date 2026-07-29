# SysMonitor — XFCE Panel Plugin

A lightweight, real-time system monitor and thermal controller for the XFCE desktop panel.

This is a **personal project** written for my own hardware (Intel CPU + iGPU on Debian). It was a fun project and not intended to be universal — built to keep my laptop's temperature under control while keeping an eye on system resources.
---

## Features

- **Panel display** (left → right):
  - CPU utilization (%)
  - CPU frequency (GHz)
  - CPU package temperature (°C)
  - GPU frequency (GHz)
  - Fan RPM
  - Memory used (GB)
  - Battery capacity (%)

- Color-coded values that change with load/temperature (Dracula-inspired palette).
- Click the panel item to open a popup with additional temperature sensors (auto-discovered from `hwmon` and ACPI thermal zones).
- **Automatic thermal throttling**:
  - Level 1 / 2 / 3 based on CPU temperature thresholds
  - Dynamically writes to `intel_pstate` max performance and GPU frequency limits
  - Cooldown logic to avoid oscillation
- Desktop notifications (via libnotify) when throttling state changes or when switching between AC / battery / low battery.
- Persistent open file descriptors + `pread` for low overhead.
- Lazy popup construction (only built on first click).

---

## Hardware Assumptions (Important)

This plugin was written for a specific machine and will need adaptation for other systems:

| Component           | Path / Mechanism                                          | Notes                              |
|---------------------|-----------------------------------------------------------|------------------------------------|
| CPU frequency       | `/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq`   | Intel pstate                       |
| CPU temp            | First `coretemp` hwmon `temp1_input`                      | Package temperature                |
| GPU frequency       | `/sys/class/drm/card0/gt_act_freq_mhz`                    | Intel iGPU (card0)                 |
| GPU max / boost     | `gt_max_freq_mhz` / `gt_boost_freq_mhz`                   | Written for throttling             |
| Fan                 | First available `fan1_input` under hwmon                  |                                    |
| Battery             | `/sys/class/power_supply/BAT0/`                           |                                    |
| Throttling control  | `intel_pstate/max_perf_pct` + GPU freq limits             | Requires write permissions (udev)  |

If your hardware uses different paths (AMD, discrete GPU, different thermal zones, a second battery, etc.) the plugin will partially or fully fail to work until the source is adjusted — the throttling logic in particular assumes `intel_pstate` and an Intel iGPU. Sensors it doesn't recognize in the popup are skipped or generically labeled rather than crashing anything, but they won't get the friendly names (`PCH`, `WiFi`, `VRM`, etc.) that are hardcoded for this specific machine.

---

## Dependencies

### Runtime
- XFCE 4 panel (`libxfce4panel-2.0`)
- GTK 3
- libnotify

### Build (Debian)

```bash
sudo apt update
sudo apt install build-essential \
                 libxfce4panel-2.0-dev \
                 libxfce4ui-2-dev \
                 libgtk-3-dev \
                 libnotify-dev \
                 pkg-config
```

---

## Building

```bash
gcc -shared -fPIC -o libsysmonitor.so sysmonitor.c \
    $(pkg-config --cflags --libs gtk+-3.0 libxfce4panel-2.0 libnotify)

# Optional: strip symbols
strip --strip-all libsysmonitor.so
```

---

## Installation

1. **Copy the shared library**
   ```bash
   sudo cp libsysmonitor.so /usr/lib/x86_64-linux-gnu/xfce4/panel/plugins/
   ```

2. **Install the desktop file**
   ```bash
   sudo cp sysmonitor.desktop /usr/share/xfce4/panel/plugins/
   ```

3. **Install udev rules (Required for the thermal-control writes)**
   ```bash
   sudo cp 99-sysfs-permissions.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```
   This udev rule makes `max_perf_pct` and the GPU frequency-cap files world-writable so the plugin can throttle without running as root. That's a reasonable tradeoff on a personal single-user laptop, but it does loosen permissions on those specific files for *any* local process — skip this step if you'd rather run without the throttling feature. Everything else (panel stats, popup, notifications) works fine without it; the throttling writes just silently fail.

   Restart the panel if the item doesn't appear in **Panel** → **Add New Items…**:
   ```bash
   xfce4-panel -r
   ```

4. **Add the plugin to the panel**
   Right-click the XFCE panel → **Panel** → **Add New Items…** → search for "SysMonitor" → **Add**.

---

## Uninstallation

```bash
sudo rm /usr/lib/x86_64-linux-gnu/xfce4/panel/plugins/libsysmonitor.so
sudo rm /usr/share/xfce4/panel/plugins/sysmonitor.desktop
sudo rm /etc/udev/rules.d/99-sysfs-permissions.rules
```
---
