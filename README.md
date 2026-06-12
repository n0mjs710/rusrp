# rusrp — Remote USRP

> **Beta — believed working, still in testing.** Basic operation in both directions appears functional, but this has not yet been tested against a live repeater. Behaviour may change without notice. We are not accepting bug reports at this stage. If you're experimenting, we'd love to hear what you find (just not as issues in github); and please don't deploy this on a live repeater you depend on.

A lightweight audio/control terminal for amateur radio repeater linking. Runs on Linux SBCs and connects an analog repeater controller to an [AllStarLink (ASL3)](https://www.allstarlink.org/) server using the USRP protocol over UDP.

One instance of `rusrp` handles one radio link: a single full-duplex CM119A USB audio/HID device, one ALSA audio stream, and one USRP session.

## Terminology

Direction in rusrp is always described from the **analog side** (the CM119A). The same line means different things depending on what is connected:

| rusrp term | CM119A pin | Connected to a **radio** | Connected to a **controller port** |
|---|---|---|---|
| **input_active** | VOLDN (pin 48) | COR/COS from the radio | PTT out from the controller |
| **output_active** | GPIO3 (pin 13) | PTT to the radio | COR/COS in to the controller |

A repeater controller sees rusrp as a radio. A radio sees rusrp as a controller. The line is the same; only the label on the far end changes. The `input_active_low` and `output_active_low` config settings describe the **electrical polarity** of the line — not what device it connects to.

## How it works

```
Analog device                            AllStarLink server
        │                                        │
   audio out ──► ALSA capture                    │
        │            │                           │
        │        250 Hz HPF  (input_highpass)    │
        │            │                           │
        │        edge trim   (input_*_trim_ms)   │
        │            │                           │
        │           USRP ───────────────► ASL3 chan_usrp
        │                                        │
   audio in ◄── ALSA playback             USRP ◄─┘
        │            ▲
        │        edge trim   (output_*_trim_ms)
        │            ▲
        │        250 Hz HPF  (output_highpass)
        │            ▲
        │      jitter buffer
        │
  input_active ──► VOLDN (HID input, byte 0 bit 1)
  output_active ◄─ GPIO3 (HID output report)
```

- **Audio**: 8 kHz mono 16-bit, 20 ms frames
- **HID device**: CM108/CM119A — COS on VOLDN (pin 48), PTT on GPIO3 (pin 13)
- **USRP protocol**: 352-byte UDP frames, network byte order (ASL3 only)
- **DSP**: 4th-order Butterworth high-pass filter at 250 Hz on both paths, enabled independently (blocks CTCSS/DCS tones)
- **Jitter buffer**: 16-slot sequence-ordered buffer with silence injection for gaps
- **Watchdog**: sd_notify integration; network timeout forces PTT release

## Requirements

### Runtime

- Linux kernel ≥ 4.x (hidraw, ALSA)
- CM108/CM119/CM119A USB audio device
- AllStarLink 3 server with `chan_usrp` configured

### Build dependencies

```bash
sudo apt-get install -y meson ninja-build pkg-config libsystemd-dev libasound2-dev
```

The TOML config parser ([tomlc99](https://github.com/cktan/tomlc99)) is vendored and fetched by `scripts/setup.sh` — no separate install needed.

## Building

```bash
git clone https://github.com/n0mjs710/rusrp.git
cd rusrp
bash scripts/setup.sh          # fetch vendored tomlc99
meson setup build
ninja -C build
```

To install system-wide (binary, example config, systemd unit):

```bash
sudo ninja -C build install
```

## Configuration

`sudo ninja -C build install` places a starter config at `/etc/rusrp/rusrp.toml` on first install. On upgrades it is left untouched. An annotated reference copy is always at `/etc/rusrp/rusrp.toml.example`.

Key settings:

| Section | Key | Description |
|---|---|---|
| `[usrp]` | `remote_host` | IP of your ASL3 server |
| `[usrp]` | `remote_port` | chan_usrp port on ASL3 (default 34001) |
| `[usrp]` | `local_port` | UDP port to bind locally (default 32001) |
| `[audio]` | `alsa_device` | ALSA device string — use `plughw:` prefix (e.g. `plughw:1,0`) |
| `[audio]` | `input_gain_db` | Mic gain in dB (−12 to +12) |
| `[audio]` | `input_highpass` | Enable 250 Hz HPF on captured audio (blocks CTCSS/DCS tones from the analog side) |
| `[audio]` | `output_highpass` | Enable 250 Hz HPF on playback audio (blocks CTCSS/DCS tones from the network side) |
| `[audio]` | `input_leading_trim_ms` | Silence the first N ms of captured audio after input_active rises (0–260, rounds to 20 ms). Removes mic click. |
| `[audio]` | `input_trailing_trim_ms` | Drop the last N ms of captured audio before input_active falls (0–260, rounds to 20 ms). Removes squelch-tail noise. |
| `[audio]` | `output_leading_trim_ms` | Silence the first N ms of output audio after output_active rises (0–260, rounds to 20 ms). Gives CTCSS decoders time to open (~100 ms). |
| `[audio]` | `output_trailing_trim_ms` | Drop the last N ms of output audio before output_active falls (0–260, rounds to 20 ms). Removes PTT-drop burst. |
| `[logic]` | `hid_device` | hidraw device (e.g. `/dev/hidraw0`) |
| `[logic]` | `output_active_gpio` | GPIO number for PTT (default 3) |
| `[logic]` | `input_active_low` | `true` if the input signal is active when the line is pulled low (most hardware) |
| `[logic]` | `output_active_low` | `true` if the output signal is active-low (open-collector driver — most hardware) |
| `[network]` | `jitter_buffer_ms` | Jitter buffer depth, 40–250 ms; non-multiples of 20 round up to the next frame |
| `[watchdog]` | `network_timeout_ms` | Force output_active release after this many ms with no USRP traffic |

See `config/rusrp.toml.example` for all options with comments.

### Audio edge trimming

The four `*_trim_ms` settings clean up the edges of transmissions:

**Leading trim** silences the first N milliseconds of audio after a transmission begins. The channel is captured immediately — the KEY frame is sent to the network and output_active asserts on the hardware without delay — only the audio content is muted. This removes mic clicks and PTT-thump artifacts from the beginning of a transmission.

For the output path, `output_leading_trim_ms` serves a specific purpose when CTCSS is in use: the output_active signal to your radio asserts immediately, but voice audio is held back for N ms, giving the CTCSS decoder in the downstream radio time to open the squelch before voice arrives. Approximately 100 ms is a typical value.

**Trailing trim** removes the last N milliseconds of audio before a transmission falls. Because you cannot predict the future, this works by running the audio through a FIFO delay: audio is time-shifted by N ms, and when the end of the transmission is detected, the buffered pre-edge audio is forwarded and then the path stops. The last N ms of real-time audio — which may contain squelch tail noise or click artifacts — is discarded.

This is the digital equivalent of the analog audio delay board used in FM repeaters for squelch-tail elimination. The cost is N ms of latency added to every transmission; start with values in the 40–100 ms range and adjust by ear.

All trim values are in milliseconds and are rounded to the nearest 20 ms frame (the USRP frame size). The valid range is 0–260 ms; 0 disables the function with no latency penalty.

## Running

rusrp accesses hardware directly (ALSA, hidraw) and runs as root — either via systemd or `sudo` for testing.

### With systemd (recommended)

```bash
# Install places the binary, unit file, udev rule, and — on first install only —
# a starter config at /etc/rusrp/rusrp.toml. Edit it before starting:
sudo $EDITOR /etc/rusrp/rusrp.toml

# Enable and start
sudo systemctl enable --now rusrp

# View logs
journalctl -u rusrp -f
```

### Testing / CLI

```bash
sudo ./build/src/rusrp -c rusrp.toml
```

## udev rule

The CM119A exposes VOLDN as a keyboard key. Without a udev rule, the kernel treats a continuous carrier (COS active) as a held KEY_VOLUMEDOWN and will silently drain system volume over time.

The rule (`udev/90-cm119a.rules`) is installed automatically by `sudo ninja -C build install`. After install or after replugging the device, reload:

```bash
sudo udevadm control --reload && sudo udevadm trigger
```

## Finding your ALSA and HID devices

```bash
# List USB audio devices
aplay -l | grep -i cm

# List hidraw devices
ls -la /dev/hidraw*
udevadm info /dev/hidraw0 | grep -E "(VENDOR|MODEL|NAME)"
```

## Logs

rusrp logs to the systemd journal. Config validation errors also print to stderr so they are visible whether you start via `systemctl` or directly from a terminal.

```bash
journalctl -u rusrp -f             # follow live
journalctl -u rusrp --since today  # today's entries
```

### Startup sequence

A clean start produces these INFO lines in order:

```
rusrp starting, config: /etc/rusrp/rusrp.toml
alsa: opened plughw:1,0 (8 kHz mono s16le, 20 ms frames)
usrp: listening on :32001 → 198.51.100.10:34001
logic: opened /dev/hidraw0 (gpio3, out_low=1)
rusrp ready
```

### Status lines

A status line is written at the end of every transmission, summarising that transmission. The prefix tells you what event triggered it:

| Prefix | Meaning |
|---|---|
| `input-end:` | input_active just dropped — the signal rusrp was receiving ended |
| `output-end:` | output_active just released — rusrp finished keying the output |

Each event type only shows the levels relevant to that stream:

```
input-end:  in=-12.3pk/-18.0rms dBFS input_active=0 output_active=0 jitter=42.0ms late=0 wd_events=0 overruns=0 underruns=0
output-end: out=-14.1pk/-20.3rms dBFS input_active=0 output_active=0 jitter=42.0ms late=0 wd_events=0 overruns=0 underruns=0
```

| Field | Meaning |
|---|---|
| `in=Xpk/Yrms dBFS` | Peak and RMS level of the signal rusrp received, accumulated over that transmission (`input-end` only) |
| `out=Xpk/Yrms dBFS` | Peak and RMS level rusrp sent to the output, accumulated over that transmission (`output-end` only) |
| `input_active=0/1` | Whether input was active at the moment of logging |
| `output_active=0/1` | Whether output was active at the moment of logging |
| `jitter=X ms` | Jitter buffer fill estimate at log time |
| `late=N` | USRP packets that arrived after their playout deadline (network glitches) |
| `wd_events=N` | Times the watchdog forced an unkey due to network timeout |
| `overruns=N` | ALSA capture buffer overruns |
| `underruns=N` | ALSA playback buffer underruns |

When `level = "debug"`, a `heartbeat:` line also fires every `status_interval_sec` seconds. It shows both `in=` and `out=` levels and confirms the daemon is alive; levels between transmissions will read near −96 dBFS (silence).

### Debug messages

These appear at DEBUG level during normal operation. Input and output events are logged independently from their respective code paths.

Input events:

| Message | Meaning |
|---|---|
| `input: active` | Input signal went active |
| `input: ended` | Input signal dropped |

Output events:

| Message | Meaning |
|---|---|
| `output: pending N ms` | Output will assert after the jitter buffer fills (N = `jitter_buffer_ms`) |
| `output: asserted` | Output is now active |
| `output: holding N ms for buffer drain` | Output held briefly while buffered audio plays out |

This appears at WARNING level and indicates something went wrong:

| Message | Meaning |
|---|---|
| `output: forced release: network timeout` | No USRP packets for `network_timeout_ms` ms; output forcibly released |
| `output: forced release: stopping` | Clean shutdown; output released before exit |

### Error messages

| Message | Meaning |
|---|---|
| `config: cannot open /path/rusrp.toml` | Config file not found — check the path |
| `config: parse error in /path: ...` | TOML syntax error; the detail string identifies the line |
| `config: usrp.remote_host is required` | `remote_host` missing from `[usrp]` section |
| `config: usrp ports must be 1–65535` | Invalid `remote_port` or `local_port` |
| `config: gain_db must be in range -12 to +12` | `input_gain_db` or `output_gain_db` out of range |
| `config: jitter_buffer_ms = N is invalid; must be 40–250 ms` | Value out of range; minimum is 40 |
| `config: logic.hid_device is required` | `hid_device` missing from `[logic]` section |
| `alsa: open plughw:X,Y (capture/playback): ...` | ALSA device not found or already in use — verify with `aplay -l` |
| `logic: open /dev/hidrawN: ...` | HID device not found — check udev rule, permissions, and that the device is plugged in |
| `logic: HID write failed` | Lost communication with CM119A during operation |
| `usrp: bind :N: ...` | UDP port in use or permission denied |
| `usrp: cannot resolve host` | Remote host unreachable at startup |

## License

See [LICENSE](LICENSE).
