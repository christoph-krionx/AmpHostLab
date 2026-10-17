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
| `--ui-font <name>` | Typeface of the screen, an installed font family such as `Space Grotesk`, `Barlow` or `Barlow Condensed` (default `Barlow Condensed`; the system's typeface when it is not installed). Fonts go to `~/.local/share/fonts` |
| `--ui-accent <colour>` | The screen's one colour, for the cursor and what is being edited: `amber` (default), `blue` or `#rrggbb` |
| `--idle-view <s>` | Seconds without input until the screen shows only the preset, `0` = never (default 10) |
| `--lcd-backlight <gpio>` | Backlight switched by this GPIO (e.g. `13`): on with the first picture, off when the host ends, so the panel stays dark while it is not running |
| `--encoder <a,b,sw>` | Front-panel encoder on these BCM GPIOs, e.g. `5,6,26` |
| `--model-encoder <a,b,sw>` | Second encoder, e.g. `18,19,20` |
| `--model-encoder-role <r>` | What it does: `knob1` (default) .. `knob5` (a value encoder, see Zen mode), `volume` (the preset's volume) or `models` (steps the selected block's models) |
| `--zen-knob-<n> <control>` | Value encoder n's control in the zen mode: `param:<id>`, `block<N>:on` or `block<N>:inputGain` / `outputGain` / `mix`; set by a long press on the panel |
| `--midi-input <text>` | MIDI input whose name contains `<text>`, e.g. `Scarlett` (see MIDI) |
| `--midi-channel <n>` | MIDI channel 1-16, `0` = all (default) |
| `--midi-tuner-cc <n>` | CC number that switches the tuner on and off |
| `--midi-cc <cc>:<target>` | Map a CC to a processor target, repeatable (see MIDI) |
| `--preview <png>` | Draw the main screen into a 320x240 PNG and exit, no audio |
| `--preview-cursor <n>` / `--preview-edit` / `--preview-page <name>` | Cursor position, edit mode and page (`overview`, `gate`, `pitch`, `spread`, `block`, `settings`, `idle`, `splash`) in the preview |

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

The screen has the same frame on every page: the preset name (large, with its place in the preset list in front) with the save, tuner and settings buttons, the effect switches (gate, pitch, spread), the stage between the input and output meters (with the level in dB underneath, its peak held for three seconds and inverted after clipping), a context line, and along the bottom edge five knob slots, one above each of the (planned) value encoders. Everything that shows a state is greyscale; the one accent colour (`ui-accent`) marks only the cursor and what is being edited, and warnings are inverted.

The navigation encoder with push button operates the device. Turning moves the cursor in screen order (wrapping around). On the overview: preset, save, tuner, settings, the effect switches, the blocks of the chain, then the knobs gain, bass, mid, treble and volume.

- A short press switches what the cursor is on (a block, an effect, a switch on a page) or starts editing it (preset, knob, choice); turning then changes the value and another press ends editing. In the preset item turning steps through the presets.
- A long press (600 ms, it fires while the button is still held) on an effect switch or a block opens its page in place of the grid; on a page it goes back. While a knob is being edited, a long press sets it to its default.

The screen starts in the idle view right after the splash, and returns to it after ten seconds without input (`idle-view`): it shows only the preset, its number above its name, nothing else. The first turn or press only brings the normal screen back, so nothing changes by accident in the dark; presets loaded over MIDI show up in the idle view directly. The tuner, an edit in progress or a pending confirmation keep the normal screen.

The gate, pitch and spread pages list their switches on the stage and put their continuous parameters into the knob row; the gate page marks its threshold on the input meter.

A block's page has the block's input, output and mix in the knob row, and on the stage: the block on or off, normalize, the model (for a local tone with several models a press starts picking; the model loads while turning, once the encoder rests), and move and remove. Move goes back to the grid with the block framed in the accent colour: turning moves it one place along its row (empty slots count as places), a press puts it down. Remove asks once: a second press removes the block, turning keeps it. Until the value encoders are fitted, the navigation encoder's cursor also walks through the knob slots. Value encoders work the slot above them directly; a short press toggles fine steps, a long press assigns the encoder's zen control (below).

### Zen mode

The zen mode is the idle view, the screen that shows only the preset. There each value encoder works one control of its own, the same for every preset, and the screen shows that control alone while it turns: its name, the value large and, for a knob, a bar. The idle view stays; presses do nothing there. By default the five encoders are the noise gate's threshold, the preset's bass, mid and treble, and its volume.

To put something else on an encoder, move the cursor onto it on the normal screen and hold that encoder: a knob of any page, an effect switch (gate, pitch, spread), an entry of a page (pitch step, for example) or a block (its on/off). A switch follows the turning direction: clockwise on, the other way off. Blocks are taken by their place in the chain ("block 2"), since every preset has blocks of its own; a preset with fewer blocks shows "No block". The assignment is kept in the settings file (`zen-knob-1` .. `zen-knob-5`). Changes made in the zen mode count like any other change to the preset: save keeps them, discard drops them.

A star next to the preset name marks unsaved changes; the save button writes the current sound back to the loaded preset, and a long press on the preset name discards the changes (the preset loads again as saved). The tuner takes the grid's place while its item is entered. The context line describes whatever the cursor is on (on a block with several models, which one plays, e.g. `3/10  Plexi: Gain 3`; on the settings button the CPU load, the CPU temperature and the dropouts) and shows short messages such as "Saved" or "Presets updated" for a moment.

A press on the settings button opens the settings page: the input mode (left, right, stereo, dual mono) switches while turning and is written to the settings file when editing ends; power off asks once and then shuts the system down, showing when it is safe to unplug; below them the page shows the network address, CPU load, dropouts, sample rate and buffer, and the version. A long press goes back. Power off needs the host's user to run `systemctl poweroff` without a password:

```bash
echo 'pi ALL=(root) NOPASSWD: /usr/bin/systemctl poweroff' | sudo tee /etc/sudoers.d/t3k-host-poweroff
sudo chmod 440 /etc/sudoers.d/t3k-host-poweroff
```

The optional second encoder (`--model-encoder`, e.g. on GPIO 18/19/20 while I2S is unused) is value encoder 1 by default (`model-encoder-role = knob1`, see Zen mode). With `--model-encoder-role volume` it is the preset's volume, like an amp's master knob: it acts at once, even out of the idle view, and the context line shows the value for a moment. With `--model-encoder-role models` it steps through the models of the block under the cursor, or of the open block page's block: quick comparisons of a capture pack's gain stages, for example. The model loads once the encoder rests. Only tones that carry several models can switch: a folder loaded as a local tone in the plugin and saved into a preset. A catalog tone stores only its active model. Note: a preset made on another computer embeds only the models that were actually loaded there; the others refer to that computer's model stash and fail to load on the device.

`--preview <png> --preview-page gate --preview-cursor 1` draws a page into a PNG without audio, to check the layout.

Presets are the plugin's `.t3kpreset` files in `~/.config/TONE3000/Presets/` (factory presets in `Presets/Factory/`). The chain is drawn as the plugin's grid: one row in mono, two in stereo, at least five slots wide.

Presets copied into that folder (for example through a network share) appear in the list within about two seconds, without a restart; the host waits until the folder has stopped changing, so a file still being copied is not read. The list follows the plugin's order: the presets named in `order.json` first (copy it from the plugin's preset folder to keep its order and numbers), then the rest alphabetically by file name. The name shown is the one stored in the file. When the file of the preset that is playing is replaced, the host loads it again, unless the sound has unsaved changes on the device.

A Samba share of the preset folder (and of a tone folder) for Windows, macOS and Linux, in `/etc/samba/smb.conf`:

```text
[Presets]
   path = /home/pi/.config/TONE3000/Presets
   valid users = pi
   force user = pi
   read only = no
```

with `sudo smbpasswd -a pi` for the password; on Windows connect as `NAMHOST02\pi` to `\\namhost02.local\Presets`.

The encoder is read through GPIO edge events with the Pi's internal pull-ups, so it only needs wires to the pins and GND:

| Encoder | CM5 header |
| --- | --- |
| A (CLK) | GPIO 5 (pin 29) |
| B (DT) | GPIO 6 (pin 31) |
| Button (SW) | GPIO 26 (pin 37) |
| Common / GND | GND (pin 39) |

Encoder modules with on-board pull-up resistors need those removed (or their `+` pin on 3.3 V); with `+` left open the resistors couple the three signals together.

`build/enc-test` prints every detent and button event to check the wiring.

## MIDI

A MIDI controller on the audio interface's MIDI input (`midi-input = Scarlett`) drives the processor's own MIDI mapper:

- Program change n loads the (n+1)th preset of the list, the number shown in front of the preset name minus one.
- `midi-cc = <cc>:<target>` maps a CC, one line per mapping. Targets: `presetNext`, `presetPrevious`, `block1Power`, `block2Power`, ... (tone blocks by position), `stereoEnabled`, or any parameter id (`gateEnabled`, `pitchEnabled`, `spreadEnabled`, `toneBass`, `outputLevel`, ... see `--list-params`). Switch targets toggle once per press; continuous parameters follow the CC value.
- `midi-tuner-cc` switches the tuner, which is no processor parameter; the screen goes to the overview with the tuner showing.

The screen follows presets loaded over MIDI (number, name, and the preset remembered for the next start).

```text
midi-input = Scarlett
midi-channel = 0
midi-tuner-cc = 64
midi-cc = 65:presetPrevious
midi-cc = 66:presetNext
midi-cc = 67:block1Power
```

## Logo light

The logo glows like a warm tube: a steady low glow while nothing is played; while the input passes the gate's threshold (-55 dB with the gate off) it brightens and breathes, and it glows up briefly when another preset comes in. The logo in the idle view shows it.

With `logo-light = 12` a GPIO lights a physical logo while the guitar is played: on as soon as the input passes the threshold, held 0.4 s across the gaps between notes, then off; the light's own RC filter makes the fade. It shares its LED with the heartbeat, each on its own GPIO: GPIO 17 (the system's heartbeat, always running, the sign of life) and GPIO 12 each feed the LED's RC filter through a Schottky diode (diode-OR), so neither GPIO drives the other. No overlay is needed: the host drives the GPIO directly.

## Realtime permissions

The audio thread needs realtime priority and locked memory. For the user that runs the host, in `/etc/security/limits.d/audio.conf`:

```text
@audio   -  rtprio     95
@audio   -  memlock    unlimited
```

and add the user to the `audio` group. The systemd unit sets both limits itself.

The host raises the audio device's thread to `SCHED_FIFO` priority 80 on its first callback (JUCE's ALSA thread starts with normal scheduling) and logs `Audio thread: realtime (SCHED_FIFO 80)`, or a warning when the limit refuses it. The systemd unit also switches the CPU governor to `performance` before the host starts, so clock changes never slow the audio thread down.

Keep the CM5 cool: `vcgencmd get_throttled` reports past throttling in its upper bits (`0xe0000` = the clock was capped and throttled after reaching the soft temperature limit). A throttled CPU can miss audio deadlines.

## Where files live

The processor stores presets and its local-model stash under the user's application data directory (`~/.config/TONE3000/` on Linux).
