# SysMonitor — XFCE Panel Plugin

<br><img width="941" height="317" alt="demo" src="https://github.com/user-attachments/assets/82996c2c-c084-4a14-862f-c83ab5649666" />


<br>A lightweight, real-time system monitor and thermal controller for the XFCE desktop panel.

This was a **personal project** written for my own laptop. It was not intended to be universal — built to keep my laptop's temperature under control while keeping an eye on system resources.


## Features

- **Panel display** (left ⇨ right):
  - CPU utilization (%)
  - CPU frequency (GHz)
  - CPU package temperature (°C)
  - GPU frequency (GHz)
  - Fan RPM
  - Memory used (GB)
  - Battery capacity (%)

- Color-coded values that change with load / temperature (Dracula-inspired palette).
- Popup with additional temperature sensors (auto-discovered from `hwmon` and ACPI thermal zones).
- **Automatic thermal throttling**:
  - Three-level throttling linked directly to hardware thermal states.
  - Dynamic scaling of CPU `intel_pstate` and GPU frequency limits.
  - Smart cooldown buffers to prevent rapid state changes.
- Desktop notifications (via libnotify) when throttling state changes or when switching between AC / battery.
- Persistent open file descriptors + `pread` for low overhead.
- Lazy popup construction (only built on first click).

---

## Hardware Assumptions (Important)

This plugin was written for a specific machine and will need adaptation for other systems:

| Component | Path / Mechanism |
| :--- | :--- |
| **CPU Frequency** | `/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq` |
| **CPU Temp** | `/sys/class/hwmon/` (`temp1_input` of `coretemp`) |
| **GPU Frequency** | `/sys/class/drm/card0/gt_act_freq_mhz` |
| **Fan Speed** | `/sys/class/hwmon/` (First available `fan1_input`) |
| **Battery** | `/sys/class/power_supply/BAT0/` |
| **CPU Throttling** | `/sys/devices/system/cpu/intel_pstate/max_perf_pct` |
| **GPU Throttling** | `/sys/class/drm/card0/gt_max_freq_mhz` and `/sys/class/drm/card0/gt_boost_freq_mhz` |

If your hardware uses different paths (AMD, discrete GPU, different thermal zones, a second battery, etc.) the plugin will partially or fully fail to work until the source is adjusted — the throttling logic in particular assumes `intel_pstate` and an Intel iGPU. <br><br>Because sensor names (`PCH`, `WiFi`, `VRM`, etc.) are hardcoded specifically for my machine, unrecognized thermal sensors on other systems may be labeled incorrectly or skipped entirely.

---

## Configuration

To adjust the color thresholds and performance limits for your specific hardware, modify these values in `sysmonitor.c`

```c
// COLOR THRESHOLDS

// RAM colors (20 = 2.0 GB, 35 = 3.5 GB)
set_label(&s->l_mem, buf, LVL_CLASSES[get_level(mem_u, 20, 35, 50, 65)]);

// CPU Frequency colors (kHz)
set_label(&s->l_cpu_f, buf, LVL_CLASSES[get_level(cpu_f, 1200000, 2000000, 2800000, 3400000)]);

// CPU Temperature colors (°C)
set_label(&s->l_temp, buf, LVL_CLASSES[get_level(temp, 45, 55, 75, 85)]);

// THROTTLING THRESHOLDS

// Set the temperature thresholds (°C) that trigger each throttling level
new_throttle = (temp >= 85) ? 3 : (temp >= 80) ? 2 : (temp >= 75) ? 1 : 0; 

// Set CPU/GPU limits for each level (e.g., Level 3 limits CPU to 50% and GPU to 300 MHz)
if (new_throttle == 3) { write_sys(CPU_MAX_PCT,"50"); write_sys(GPU_MAX,"300"); write_sys(GPU_BOOST,"300"); }
```
---

## Build Dependencies (Debian)
```bash
sudo apt update
sudo apt install build-essential pkg-config libxfce4panel-2.0-dev libxfce4ui-2-dev libgtk-3-dev libnotify-dev
```
---
  
## Compilation

```bash
gcc -shared -fPIC -o libsysmonitor.so sysmonitor.c \
    $(pkg-config --cflags --libs gtk+-3.0 libxfce4panel-2.0 libnotify)

# Optional: strip symbols
strip --strip-all libsysmonitor.so
```

---

## Installation

Copy the library and desktop configuration to your XFCE plugin directories:

```bash
sudo cp libsysmonitor.so /usr/lib/x86_64-linux-gnu/xfce4/panel/plugins/
sudo cp sysmonitor.desktop /usr/share/xfce4/panel/plugins/
```
*If the plugin does not immediately appear in your panel's "Add New Items" menu after copying, restart the panel `xfce4-panel -r`*

 ---
## Udev Rules (Required for Thermal Throttling)

  Warning: This udev rule grants write permissions to CPU and GPU limit files, allowing thermal throttling without root access. This is a practical tradeoff for a personal, single-user laptop, but allows any local process to modify these limits. <br><br>Skip this step if you'd rather run without the throttling feature. Everything else (panel stats, popup, notifications) works fine without it; the throttling writes just silently fail.
   ```bash
   sudo cp 99-sysfs-permissions.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```
 ---
  

## Uninstallation

```bash
# Remove plugin and configuration files
sudo rm /usr/lib/x86_64-linux-gnu/xfce4/panel/plugins/libsysmonitor.so
sudo rm /usr/share/xfce4/panel/plugins/sysmonitor.desktop
sudo rm /etc/udev/rules.d/99-sysfs-permissions.rules

# Reload udev to clean up rules and restore default permissions
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---
