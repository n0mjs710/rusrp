# rusrp — Remote USRP

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
        │        leading trim (input_leading_trim_ms)   │
        │            │                           │
        │           USRP ───────────────► ASL3 chan_usrp
        │                                        │
   audio in ◄── ALSA playback                    |
        │            ▲                           |
        │        leading trim (output_leading_trim_ms)  |
        │            ▲                           |
        │        250 Hz HPF  (output_highpass)   |
        │            ▲                           |
        │      jitter buffer ◄──────────────── USRP
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
sudo apt-get install -y build-essential pkg-config libsystemd-dev libasound2-dev
```

The TOML config parser ([tomlc99](https://github.com/cktan/tomlc99), MIT) is vendored in the repository — no separate install or download step needed.

## Building

```bash
git clone https://github.com/n0mjs710/rusrp.git
cd rusrp
make
```

To install system-wide (binary, example config, systemd unit):

```bash
sudo make install
```

## Configuration

`sudo make install` places a starter config at `/etc/rusrp/rusrp.toml` on first install. On upgrades it is left untouched. An annotated reference copy is always at `/etc/rusrp/rusrp.toml.example`.

Key settings:

| Section | Key | Description |
|---|---|---|
| `[usrp]` | `remote_host` | IP of your ASL3 server |
| `[usrp]` | `remote_port` | chan_usrp port on ASL3 (default 34001) |
| `[usrp]` | `local_port` | UDP port to bind locally (default 32001) |
| `[usrp]` | `bind_address` | Local IP to bind the UDP socket (default `"0.0.0.0"` — all interfaces; change only if the SBC has multiple NICs and you need to restrict traffic to one) |
| `[audio]` | `alsa_device` | ALSA device string — use `plughw:` prefix (e.g. `plughw:1,0`) |
| `[audio]` | `input_gain_db` | Mic gain in dB (−12 to +12) |
| `[audio]` | `input_highpass` | Enable 250 Hz HPF on captured audio (blocks CTCSS/DCS tones from the analog side) |
| `[audio]` | `output_highpass` | Enable 250 Hz HPF on playback audio (blocks CTCSS/DCS tones from the network side) |
| `[audio]` | `input_leading_trim_ms` | Silence the first N ms of captured audio after input_active rises (0–260, rounds to 20 ms). Removes mic click. |
| `[audio]` | `output_leading_trim_ms` | Silence the first N ms of output audio after output_active rises (0–260, rounds to 20 ms). Gives CTCSS decoders time to open (~100 ms). |
| `[logic]` | `hid_device` | hidraw device (e.g. `/dev/hidraw0`) |
| `[logic]` | `output_active_gpio` | GPIO number for PTT (default 3) |
| `[logic]` | `input_active_low` | `true` if the input signal is active when the line is pulled low (most hardware) |
| `[logic]` | `output_active_low` | `true` if the output signal is active-low (open-collector driver — most hardware) |
| `[logic]` | `half_duplex` | `true` to block the second direction until the first releases (first-come-first-served); useful when hardware or wireline configuration cannot cleanly handle simultaneous RX and TX |
| `[network]` | `jitter_buffer_ms` | Jitter buffer depth, 40–250 ms; non-multiples of 20 round up to the next frame |
| `[watchdog]` | `network_timeout_ms` | Force output_active release after this many ms with no USRP traffic |
| `[watchdog]` | `unkey_debounce_ms` | Wait N ms before acting on an unkey event; absorbs brief keyup=0 gaps from Asterisk bridging (0 = act immediately) |

See `config/rusrp.toml.example` for all options with comments.

### Audio leading trim

**Leading trim** silences the first N milliseconds of audio after a transmission begins. The channel is captured immediately — the KEY frame is sent to the network and output_active asserts on the hardware without delay — only the audio content is muted.

For the output path, `output_leading_trim_ms` gives CTCSS decoders time to open before voice arrives. Approximately 100 ms is a typical value.

Trim values are in milliseconds, rounded to the nearest 20 ms frame. The valid range is 0–260 ms; 0 disables the function.

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
sudo ./build/rusrp -c rusrp.toml
```

## udev rule

The CM119A exposes VOLDN as a keyboard key. Without a udev rule, the kernel treats a continuous carrier (COS active) as a held KEY_VOLUMEDOWN and will silently drain system volume over time.

The rule (`udev/90-cm119a.rules`) is installed automatically by `sudo make install`. After install or after replugging the device, reload:

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
logic: opened /dev/hidraw0 (gpio3, out_low=1)
usrp: listening on :32001 → 198.51.100.10:34001
alsa: opened plughw:1,0 (8 kHz mono s16le, 20 ms frames)
rusrp ready
```

### Status lines

A status line is written at the end of every transmission, summarising that transmission. The prefix tells you what event triggered it:

| Prefix | Meaning |
|---|---|
| `input-end:` | input_active just dropped — the signal rusrp was receiving ended |
| `output-end:` | output_active just released — rusrp finished keying the output |

Each event type shows only the fields relevant to that path:

```
input-end:  in=-12.3pk/-18.0rms dBFS overruns=0 clips=0
output-end: out=-14.1pk/-20.3rms dBFS jitter=42.0ms late=0 silence=0 underruns=0 clips=0
```

**`input-end` fields:**

| Field | Meaning |
|---|---|
| `in=Xpk/Yrms dBFS` | Peak and RMS level of captured audio, accumulated over the transmission |
| `overruns=N` | ALSA capture buffer overruns — ALSA filled its ring buffer before rusrp could read it; audio frames were lost |
| `clips=N` | Samples clipped by the gain stage — non-zero means `input_gain_db` is set too high and audio is distorting |

**`output-end` fields:**

| Field | Meaning |
|---|---|
| `out=Xpk/Yrms dBFS` | Peak and RMS level of audio sent to the output device, accumulated over the transmission |
| `jitter=X ms` | Estimated network jitter at log time |
| `late=N` | USRP packets dropped on arrival (arrived behind the playout cursor or outside the jitter buffer window) |
| `silence=N` | Playout slots where no frame was available — silence injected; high values with low `late` indicate packet loss before the frame reached us |
| `underruns=N` | ALSA playback buffer underruns |
| `clips=N` | Samples clipped by the output gain stage — non-zero means `output_gain_db` is set too high |

### Reading the numbers

A healthy output-end line looks like this:

```
output-end: out=-14.1pk/-20.3rms dBFS jitter=8.0ms late=0 silence=0 underruns=0 clips=0
```

What elevated values mean:

| Field | What it tells you | What to try |
|---|---|---|
| `jitter` high | Variable packet delivery from the network or ASL server | Values up to ~30 ms are normal on typical Internet paths; sustained values above 80–100 ms indicate a congested or poor-quality path |
| `late` > 0 | Packets arrived after the playout cursor and were discarded | Increase `jitter_buffer_ms` in 20 ms steps until `late` reaches zero |
| `silence` > 0 with `late` = 0 | The buffer ran dry — packets simply stopped arriving (packet loss or a gap from Asterisk) | A consistent 6–8 frames per transmission is a known Asterisk characteristic and is benign; values above ~20 suggest packet loss on the network path; increasing `jitter_buffer_ms` will not help when `late=0` |
| `silence` > 0 with `late` > 0 | Buffer too shallow — frames arrived but too late to use | Increase `jitter_buffer_ms` |
| `underruns` > 0 | ALSA playback buffer ran dry — CPU couldn't keep up | System load issue; reduce other load on the SBC |
| `overruns` > 0 (input-end) | ALSA capture buffer filled before rusrp could read it | Same as underruns — system load |
| `clips` > 0 | Gain stage is clipping — audio is distorting | Reduce `input_gain_db` (for `input-end`) or `output_gain_db` (for `output-end`) |

The `jitter_buffer_ms` setting trades latency for reliability. A deeper buffer absorbs more network jitter but delays the start of each transmission by the same amount. Increase it until `late=0`, then stop.

When `level = "debug"`, a `heartbeat:` line also fires every `status_interval_sec` seconds. It shows both `in=` and `out=` levels, `input_active=` and `output_active=` flags, and all stats from both paths — useful to confirm the daemon is alive between transmissions.

### Half-duplex messages

These appear at INFO level when `half_duplex = true` and a transmission arrives while the floor is already held by the other direction:

| Message | Meaning |
|---|---|
| `input: blocked (output active)` | An input signal rose while the network is keyed; this input is skipped until the floor is released |
| `output: blocked (input active)` | A network key event arrived while input is active; this output is skipped until the floor is released |

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
