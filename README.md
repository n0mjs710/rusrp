# rusrp — Remote USRP

> **Alpha — not production ready.** This project is in early testing. It may not work, may behave unexpectedly, and will change without notice. We are not accepting bug reports at this stage. If you're experimenting, we'd love to hear what you find — but please don't deploy this on a live repeater you depend on.

A lightweight audio/control terminal for amateur radio repeater linking. Runs on Linux SBCs and connects an analog repeater controller to an [AllStarLink (ASL3)](https://www.allstarlink.org/) server using the USRP protocol over UDP.

One instance of `rusrp` handles one radio link: a single full-duplex CM119A USB audio/HID device, one ALSA audio stream, and one USRP session.

## How it works

```
Repeater controller                      AllStarLink server
        │                                        │
   audio out ──► ALSA capture                    │
        │            │                           │
        │         250 Hz HPF                      │
        │            │                           │
        │        USRP TX ─────────────────────► ASL3 chan_usrp
        │                                        │
   audio in ◄── ALSA playback              USRP RX
        │            ▲                           │
        │        jitter buffer ◄─────────────────┘
        │
   COS input ──► VOLDN (HID input report byte 0 bit 1)
   PTT output ◄─ GPIO3 (HID output report)
```

- **Audio**: 8 kHz mono 16-bit, 20 ms frames
- **HID device**: CM108/CM119A — COS on VOLDN (pin 48), PTT on GPIO3 (pin 13)
- **USRP protocol**: 352-byte UDP frames, network byte order (ASL3 only)
- **DSP**: 4th-order Butterworth high-pass filter at 250 Hz (blocks CTCSS/DCS tones)
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
| `[audio]` | `input_highpass` | Enable 250 Hz HPF on captured audio (blocks CTCSS/DCS) |
| `[logic]` | `hid_device` | hidraw device (e.g. `/dev/hidraw0`) |
| `[logic]` | `output_active_gpio` | GPIO number for PTT (default 3) |
| `[logic]` | `input_active_low` | Invert COS polarity if needed |
| `[logic]` | `output_active_low` | Invert PTT polarity if needed |
| `[network]` | `jitter_buffer_ms` | Jitter buffer depth (40–250 ms) |
| `[watchdog]` | `network_timeout_ms` | Force PTT release after this many ms with no USRP traffic |

See `config/rusrp.toml.example` for all options with comments.

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

## License

See [LICENSE](LICENSE).
