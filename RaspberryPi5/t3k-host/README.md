# t3k-host

Headless host for the [TONE3000 plugin](https://github.com/tone-3000/tone3000-plugin) processor on the Raspberry Pi CM5.

It compiles the plugin's processor sources directly (the same recipe as the plugin's own `test/CMakeLists.txt`, `HEADLESS=1`) into a console program and runs it on an ALSA device. There is no editor, no plugin format and no window.

Status: builds and runs on a CM5 (Debian 13) with a USB audio interface and an ILI9341 SPI display. The ST7789 path of the display driver is untested.

## Build

On the Pi (Raspberry Pi OS 64-bit):

```bash
chmod +x build.sh
./build.sh
```

The script installs the build packages, clones the plugin into `tone3000-plugin/` at the commit the host was written against (`T3K_REF`, default `ab5e5ae`) and builds into `build/`.

## Run

```bash
t3k-host --list-devices
t3k-host --device Scarlett --block 64 --tone ~/tones/my-amp.nam
```

| Option | Meaning |
| --- | --- |
| `--device <text>` | ALSA device whose name contains `<text>` |
| `--rate <hz>` | Sample rate, default 48000 (the plugin's chain runs at 48 kHz) |
| `--block <frames>` | Buffer size, default 64 |
| `--tone <path>` | Local `.nam` (A2 only), IR `.wav`, or a folder of them |
| `--preset <name>` | Load a preset by name |
| `--input-mode <mode>` | `stereo`, `left`, `right`, `dual`; default `left` (guitar on input 1) |
| `--status <seconds>` | CPU load and xrun report interval, `0` = off |
| `--set <id>=<value>` | Set a parameter in its own units, repeatable (e.g. `--set pitchEnabled=1 --set pitchSemitones=-2`) |
| `--list-params` | List parameter ids with range and current value, then exit |
| `--lcd <panel>` | SPI display: `none` (default), `ili9341`, `st7789` |
| `--lcd-rotation <0..3>` | Quarter turns, default 1 (landscape) |
| `--lcd-speed <hz>` | SPI clock, default 16000000 |

## Display

A 240x320 SPI panel (ILI9341/ILI9340 or ST7789) driven from userspace through `spidev` and three GPIO lines. Enable SPI first (`sudo raspi-config nonint do_spi 0`).

| Display pin | CM5 header |
| --- | --- |
| VCC | 3.3 V (pin 1) |
| GND | GND (pin 6) |
| CS | GPIO 8 (pin 24) |
| RESET | GPIO 27 (pin 13) |
| DC | GPIO 25 (pin 22) |
| SDI / MOSI | GPIO 10 (pin 19) |
| SCK | GPIO 11 (pin 23) |
| LED | 3.3 V (pin 17) |

`build/lcd-test --panel ili9341` draws a test picture (frame, corner marker, colour bars) to check wiring and orientation without starting audio.

The screen text needs a font that fontconfig can find, for example DejaVu Sans in `~/.local/share/fonts`.

## Realtime permissions

The audio thread needs realtime priority and locked memory. For the user that runs the host, in `/etc/security/limits.d/audio.conf`:

```text
@audio   -  rtprio     95
@audio   -  memlock    unlimited
```

and add the user to the `audio` group.

## Where files live

The processor stores presets and its local-model stash under the user's application data directory (`~/.config/TONE3000/` on Linux).
