# SysMonitor — XFCE Panel Plugin

<br><img width="941" height="317" alt="demo" src="https://github.com/user-attachments/assets/82996c2c-c084-4a14-862f-c83ab5649666" />


<br>A lightweight, real-time system monitor and thermal controller for the XFCE desktop panel.

This was a **personal project** written for my own Latitude 7490 laptop. It is not intended to be universal — built to keep my laptop's temperature under control while keeping an eye on system resources.


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

| Component           | Path / Mechanism                                          | Notes                              |
|---------------------|-----------------------------------------------------------|------------------------------------|
| CPU frequency       | `/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq`   | Intel pstate                       |
| CPU temp            | First `coretemp` hwmon `temp1_input`                      | Package temperature                |
| GPU frequency       | `/sys/class/drm/card0/gt_act_freq_mhz`                    | Intel iGPU                         |
| Fan                 | First available `fan1_input` under hwmon                  |                                    |
| Battery             | `/sys/class/power_supply/BAT0/`                           |                                    |
| Throttling control  | `intel_pstate/max_perf_pct` , `gt_max_freq_mhz` ,<br> `gt_boost_freq_mhz`| Udev Rules  |

If your hardware uses different paths (AMD, discrete GPU, different thermal zones, a second battery, etc.) the plugin will partially or fully fail to work until the source is adjusted — the throttling logic in particular assumes `intel_pstate` and an Intel iGPU. <br><br>Because sensor names (`PCH`, `WiFi`, `VRM`, etc.) are hardcoded specifically for my machine, unrecognized thermal sensors on other systems may be labeled incorrectly or skipped entirely.

---

## Build Dependencies (Debian)
```bash
sudo apt update
sudo apt install  build-essential pkg-config libxfce4panel-2.0-dev libxfce4ui-2-dev libgtk-3-dev libnotify-dev
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
