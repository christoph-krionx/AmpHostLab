# Heartbeat LED Installer for Raspberry Pi

This script configures a GPIO-connected LED as a Linux system heartbeat indicator.

## Features

- Configures GPIO 17 as a Linux LED
- Enables the `heartbeat` trigger
- Creates a backup of the existing `config.txt`
- Prevents duplicate configuration entries
- Optionally reboots the system after installation

## Hardware

Connect an LED to GPIO 17 using a current-limiting resistor (typically 220–330 Ω).

```text
GPIO17 ---- Resistor ----|>|---- GND
```

## Installation

Make the script executable:

```bash
chmod +x install-heartbeat-led.sh
```

Run the installer:

```bash
sudo ./install-heartbeat-led.sh
```

Reboot the Raspberry Pi if prompted.

## Result

The following overlay will be added to `/boot/firmware/config.txt`:

```ini
dtoverlay=gpio-led,gpio=17,label=statusled,trigger=heartbeat
```

After reboot, the LED on GPIO 17 will automatically blink using the Linux `heartbeat` trigger, indicating that the system is running.

## Tested On

- Raspberry Pi Compute Module 5 (CM5)
- Raspberry Pi OS Lite (Bookworm)

The configuration should also work on other recent Raspberry Pi models running Raspberry Pi OS.