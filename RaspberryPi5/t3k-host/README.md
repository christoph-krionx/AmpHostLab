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
| `--config <file>` | Start settings, default `~/.config/t3k-host/t3k-host.conf` (see below) |
| `--no-config` | Ignore the settings file |
| `--version` | Print the host version and the plugin commit, then exit |
| `--device <text>` | ALSA device whose name contains `<text>` |
| `--device-wait <s>` | Wait this long for the device to appear, default 30 |
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
| `--encoder <a,b,sw>` | Front-panel encoder on these BCM GPIOs, e.g. `5,6,26` |
| `--preview <png>` | Draw the main screen into a 320x240 PNG and exit, no audio |
| `--preview-cursor <n>` / `--preview-edit` / `--preview-page <name>` | Cursor position, edit mode and page (`overview`, `gate`, `pitch`, `spread`) in the preview |

### Settings file

Without a command line the host takes its start settings from `~/.config/t3k-host/t3k-host.conf`: one `key = value` per line, the keys are the option names without `--`, `#` starts a comment and a leading `~/` means the home directory. Options on the command line win; a `--tone` or `--preset` there replaces the file's sound. [`t3k-host.conf.example`](t3k-host.conf.example) is a starting point:

```bash
mkdir -p ~/.config/t3k-host && cp t3k-host.conf.example ~/.config/t3k-host/t3k-host.conf
```

A preset in the file wins over the tone, which is the fallback when the preset is missing. Whenever another preset is loaded on the device, the host rewrites the `preset` line, so the next start comes up with the same sound.

### Start on boot

[`systemd/t3k-host.service`](systemd/t3k-host.service) starts the host as user `pi` with the settings file, restarts it if it fails, and sets the realtime and memory-lock limits itself:

```bash
sudo cp systemd/t3k-host.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now t3k-host
```

`journalctl -u t3k-host -f` shows its output. While the service runs, the audio interface and the display are taken: `sudo systemctl stop t3k-host` before starting the host by hand.

During start the splash screen shows the version and what the host is waiting for (for example a USB interface that comes up late) for at least three seconds.

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

## Encoder

The screen has the same frame on every page: the preset name with the save, tuner and settings buttons, the effect switches (gate, pitch, spread), the stage between the input and output meters, a context line, and along the bottom edge five knob slots, one above each of the (planned) value encoders.

The navigation encoder with push button operates the device. Turning moves the cursor in screen order (wrapping around). On the overview: preset, save, tuner, settings, the effect switches, the blocks of the chain, then the knobs gain, bass, mid, treble and volume.

- A short press switches what the cursor is on (a block, an effect, a switch on a page) or starts editing it (preset, knob, choice); turning then changes the value and another press ends editing. In the preset item turning steps through the presets.
- A long press (600 ms, it fires while the button is still held) on an effect switch opens that effect's page in place of the grid; on a page it goes back. While a knob is being edited, a long press sets it to its default.

The gate, pitch and spread pages list their switches on the stage and put their continuous parameters into the knob row; the gate page marks its threshold on the input meter. Until the value encoders are fitted, the navigation encoder's cursor also walks through the knob slots. Value encoders will work the slot above them directly: a short press toggles fine steps, a long press resets the knob.

A star next to the preset name marks unsaved changes; the save button writes the current sound back to the loaded preset. The tuner takes the grid's place while its item is entered. The settings button has no function yet. The context line describes whatever the cursor is on.

`--preview <png> --preview-page gate --preview-cursor 1` draws a page into a PNG without audio, to check the layout.

Presets are the plugin's `.t3kpreset` files in `~/.config/TONE3000/Presets/` (factory presets in `Presets/Factory/`). The chain is drawn as the plugin's grid: one row in mono, two in stereo, at least five slots wide.

The encoder is read through GPIO edge events with the Pi's internal pull-ups, so it only needs wires to the pins and GND:

| Encoder | CM5 header |
| --- | --- |
| A (CLK) | GPIO 5 (pin 29) |
| B (DT) | GPIO 6 (pin 31) |
| Button (SW) | GPIO 26 (pin 37) |
| Common / GND | GND (pin 39) |

Encoder modules with on-board pull-up resistors need those removed (or their `+` pin on 3.3 V); with `+` left open the resistors couple the three signals together.

`build/enc-test` prints every detent and button event to check the wiring.

## Realtime permissions

The audio thread needs realtime priority and locked memory. For the user that runs the host, in `/etc/security/limits.d/audio.conf`:

```text
@audio   -  rtprio     95
@audio   -  memlock    unlimited
```

and add the user to the `audio` group.

## Where files live

The processor stores presets and its local-model stash under the user's application data directory (`~/.config/TONE3000/` on Linux).
