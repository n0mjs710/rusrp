# rusrp — USRP Remote Link

A lightweight audio/control terminal for amateur radio repeater linking. Runs on Linux SBCs and connects an analog repeater controller to an [AllStarLink (ASL3)](https://www.allstarlink.org/) server using the USRP protocol over UDP.

One instance of `rusrp` handles one radio link: a single full-duplex CM119A USB audio/HID device, one ALSA audio stream, and one USRP session. Run multiple instances (via systemd template units) for multiple links.

## How it works

```
Repeater controller                      AllStarLink server
        │                                        │
   audio out ──► ALSA capture                    │
        │            │                           │
        │         300 Hz HPF                      │
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
- **DSP**: first-order IIR high-pass filter at 300 Hz (blocks CTCSS/DCS tones)
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

Copy the example config and edit for your site:

```bash
cp config/usrp-remote-link.toml.example usrp-remote-link.toml
$EDITOR usrp-remote-link.toml
```

Key settings:

| Section | Key | Description |
|---|---|---|
| `[usrp]` | `remote_host` | IP of your ASL3 server |
| `[usrp]` | `remote_port` | chan_usrp port on ASL3 (default 34001) |
| `[usrp]` | `local_port` | UDP port to bind locally (default 32001) |
| `[audio]` | `alsa_device` | ALSA device string (e.g. `hw:1,0`) |
| `[audio]` | `input_gain_db` | Mic gain in dB (−12 to +12) |
| `[audio]` | `input_highpass` | Enable 300 Hz HPF on captured audio |
| `[logic]` | `hid_device` | hidraw device (e.g. `/dev/hidraw0`) |
| `[logic]` | `output_active_gpio` | GPIO number for PTT (default 3) |
| `[logic]` | `input_active_low` | Invert COS polarity if needed |
| `[logic]` | `output_active_low` | Invert PTT polarity if needed |
| `[network]` | `jitter_buffer_ms` | Jitter buffer depth (40–250 ms) |
| `[watchdog]` | `network_timeout_ms` | Force PTT release after this many ms with no USRP traffic |

See `config/usrp-remote-link.toml.example` for all options with comments.

## Running

### Directly

```bash
./build/usrp-remote-link -c usrp-remote-link.toml
```

### With systemd (recommended)

The installed unit is a template (`usrp-remote-link@.service`). The instance name is used as the config filename under `/etc/usrp-remote-link/`.

```bash
# Install config for an instance named "node1"
sudo cp usrp-remote-link.toml /etc/usrp-remote-link/node1.toml

# Enable and start
sudo systemctl enable --now usrp-remote-link@node1

# View logs
journalctl -u usrp-remote-link@node1 -f
```

Multiple instances for multiple radio links:

```bash
sudo systemctl enable --now usrp-remote-link@node1
sudo systemctl enable --now usrp-remote-link@node2
```

## udev rule (recommended)

The CM119A exposes VOLDN as a keyboard key. Without a udev rule, the kernel will treat a continuous carrier as a held KEY_VOLUMEDOWN, which can affect system volume. Add:

```
# /etc/udev/rules.d/90-cm119a-hid.rules
SUBSYSTEM=="input", ATTRS{idVendor}=="0d8c", ATTRS{idProduct}=="0012", \
    ENV{ID_INPUT_KEY}="", ENV{ID_INPUT}=""
```

Then reload: `sudo udevadm control --reload && sudo udevadm trigger`

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
