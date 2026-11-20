# rusrp — Remote USRP

A lightweight audio/control terminal for amateur radio repeater linking. Runs on Linux SBCs and connects an analog repeater controller to an [AllStarLink (ASL3)](https://www.allstarlink.org/) server using the USRP protocol over UDP.

One instance of `rusrp` handles one radio link: a single full-duplex CM119A USB audio/HID device, one ALSA audio stream, and one USRP session.

By default rusrp speaks the standard USRP/PCM protocol, fully compatible with ASL3's `chan_usrp`. An optional **Opus mode** is also available for bandwidth-constrained paths — narrowband Opus at 8 kbps in 20, 40 or 60 ms frames, using a fifth to a tenth of PCM's bandwidth while remaining transparent over a 300–3000 Hz FM communications channel. Every rusrp receives both PCM and Opus, whatever it sends, so Opus mode needs a compatible peer only for the far end to decode it: another rusrp, usrp-rc, or the included `usrp-reflector`, which can convert for PCM-only nodes such as ASL. Two rusrp instances can also link **point-to-point** without any server — each simply points at the other's IP and port. See [Opus mode](#opus-mode) and [usrp-reflector](#usrp-reflector) below.

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
        │    300-2700 Hz bandpass (input_bandpass)   │
        │            │                           │
        │        leading trim (input_leading_trim_ms)   │
        │            │                           │
        │           USRP ───────────────► ASL3 chan_usrp
        │                                        │
   audio in ◄── ALSA playback                    |
        │            ▲                           |
        │        leading trim (output_leading_trim_ms)  |
        │            ▲                           |
        │   300-2700 Hz bandpass (output_bandpass)   |
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
- **USRP protocol**: 352-byte UDP frames, network byte order (ASL3 only in PCM mode)
- **DSP**: 4th-order Butterworth high-pass filter at 250 Hz on both paths, enabled independently (blocks CTCSS/DCS tones); optional 3rd-order Chebyshev Type 1 bandpass filter at 300-2700 Hz (0.2 dB ripple), also independent per path — the exact same coefficients MMDVM-Host's own RX audio chain uses (ported from the sibling `usrp-rc` project's `voice_filter.c`), so an analog repeater linked through rusrp has the same effective bandwidth as one linked through a digital MMDVM path. The bandpass filter already has its own ~300 Hz low-cut built in, so enabling both filters on the same path stacks two low-end cuts for no real benefit — pick one per path, not both
- **Jitter buffer**: plain arrival-order FIFO (no sequence-number reordering — ASL3's sequencing isn't policed, only trusted for arrival order), overflow-capped at `jitter_buffer_ms` with oldest-frame-dropped on a sustained burst, silence injection on genuine underrun
- **Watchdog**: sd_notify integration; network timeout forces PTT release; also defers asserting PTT until `jitter_buffer_ms` after network keyup, independent of the jitter buffer's own fill state
- **Opus mode**: optional narrowband Opus codec (8 kbps VBR, 20/40/60 ms frames) for challenged network paths; the far end must decode Opus (rusrp, usrp-rc, or `usrp-reflector`, which converts for PCM-only nodes)

## Requirements

### Runtime

- Linux kernel ≥ 4.x (hidraw, ALSA)
- CM108/CM119/CM119A USB audio device
- AllStarLink 3 server with `chan_usrp` configured

### Build dependencies

```bash
sudo apt-get install -y build-essential pkg-config libsystemd-dev libasound2-dev libopus-dev
```

Config files are plain `[section]` / `key = value` text with `#` comments, the same format as usrp-rc. Values take no quotes; an unknown setting, a bad value or a duplicate is reported with its line number and stops startup.

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

`sudo make install` places a starter config at `/etc/rusrp/rusrp.conf` on first install. On upgrades it is left untouched. An annotated reference copy is always at `/etc/rusrp/rusrp.conf.example`.

**Upgrading from a TOML config:** older releases read `/etc/rusrp/rusrp.toml`. Rename it and drop the quotes around values; the settings themselves are unchanged:

```
sudo sed -E 's/= *"([^"]*)"/= \1/' /etc/rusrp/rusrp.toml | sudo tee /etc/rusrp/rusrp.conf >/dev/null
```

Do the same for `opus-gateway.toml` → `opus-gateway.conf`. Convert before `make install`, or it installs the starter config as `rusrp.conf`.

Key settings:

| Section | Key | Description |
|---|---|---|
| `[usrp]` | `remote_host` | IP of your ASL3 server |
| `[usrp]` | `remote_port` | chan_usrp port on ASL3 (default 34001) |
| `[usrp]` | `local_port` | UDP port to bind locally (default 32001) |
| `[usrp]` | `bind_address` | Local IP to bind the UDP socket (default `0.0.0.0` — all interfaces; change only if the SBC has multiple NICs and you need to restrict traffic to one) |
| `[audio]` | `alsa_device` | ALSA device string — use `plughw:` prefix (e.g. `plughw:1,0`) |
| `[audio]` | `input_gain_db` | Mic gain in dB (−12 to +12) |
| `[audio]` | `input_highpass` | Enable 250 Hz HPF on captured audio (blocks CTCSS/DCS tones from the analog side) |
| `[audio]` | `output_highpass` | Enable 250 Hz HPF on playback audio (blocks CTCSS/DCS tones from the network side) |
| `[audio]` | `input_bandpass` | Enable 300-2700 Hz Chebyshev bandpass on captured audio — matches MMDVM-Host's RX bandwidth exactly. Not recommended together with `input_highpass` — the bandpass already has its own ~300 Hz low-cut, so stacking the 250 Hz HPF on top adds a second low-end filter for no real benefit |
| `[audio]` | `output_bandpass` | Enable 300-2700 Hz Chebyshev bandpass on playback audio — matches MMDVM-Host's RX bandwidth exactly. Same caution: not recommended together with `output_highpass` |
| `[audio]` | `input_leading_trim_ms` | Silence the first N ms of captured audio after input_active rises (0–260, rounds to 20 ms). Removes mic click. |
| `[audio]` | `output_leading_trim_ms` | Silence the first N ms of output audio after output_active rises (0–260, rounds to 20 ms). Gives CTCSS decoders time to open (~100 ms). |
| `[logic]` | `hid_device` | hidraw device (e.g. `/dev/hidraw0`) |
| `[logic]` | `output_active_gpio` | GPIO number for PTT (default 3) |
| `[logic]` | `input_active_low` | `true` if the input signal is active when the line is pulled low (most hardware) |
| `[logic]` | `output_active_low` | `true` if the output signal is active-low (open-collector driver — most hardware) |
| `[logic]` | `half_duplex` | `true` to block the second direction until the first releases (first-come-first-served); useful when hardware or wireline configuration cannot cleanly handle simultaneous RX and TX |
| `[audio]` | `codec` | What rusrp sends: `pcm` (default, standard ASL3) or `opus` (narrowband Opus). Both are always received, with any Opus frame size. |
| `[audio]` | `opus_bitrate` | Opus VBR target in bps (4000–64000); default 8000. 8 kbps is transparent for 300–3000 Hz communications voice |
| `[audio]` | `opus_frame_ms` | Opus frame duration sent: 20, 40 or 60 ms only; default 60. All three are received. See [Choosing a frame size](#choosing-a-frame-size) |
| `[network]` | `jitter_buffer_ms` | Two things at once: the overflow ceiling on the arrival-order jitter buffer (frames beyond this depth get dropped oldest-first during a sustained burst), and the delay `watchdog.c` waits after network keyup before asserting PTT. 40–250 ms; non-multiples of 20 round up to the next frame; recommend ≥ 120 ms in Opus mode with 60 ms frames |
| `[watchdog]` | `network_timeout_ms` | Force output_active release after this many ms with no USRP traffic |
| `[watchdog]` | `unkey_debounce_ms` | Wait N ms before acting on an unkey event; absorbs brief keyup=0 gaps from Asterisk bridging (0 = act immediately) |

See `config/rusrp.conf.example` for all options with comments.

### Audio leading trim

**Leading trim** silences the first N milliseconds of audio after a transmission begins. The channel is captured immediately — the KEY frame is sent to the network and output_active asserts on the hardware without delay — only the audio content is muted.

For the output path, `output_leading_trim_ms` gives CTCSS decoders time to open before voice arrives. Approximately 100 ms is a typical value.

Trim values are in milliseconds, rounded to the nearest 20 ms frame. The valid range is 0–260 ms; 0 disables the function.

## Running

rusrp accesses hardware directly (ALSA, hidraw) and runs as root — either via systemd or `sudo` for testing.

### With systemd (recommended)

```bash
# Install places the binary, unit file, udev rule, and — on first install only —
# a starter config at /etc/rusrp/rusrp.conf. Edit it before starting:
sudo $EDITOR /etc/rusrp/rusrp.conf

# Enable and start
sudo systemctl enable --now rusrp

# View logs
journalctl -u rusrp -f
```

### Testing / CLI

```bash
sudo ./build/rusrp -c rusrp.conf
```

### Opus mode

Set `codec = opus` in `[audio]` to send narrowband Opus instead of raw PCM. The far end must decode Opus: rusrp, usrp-rc, or `usrp-reflector` (included), which converts for PCM-only nodes. An Opus transmission is all Opus — 20 ms of encoded silence keys it and a bare header unkeys it — so a PCM-only node such as ASL ignores it rather than keying on part of it.

**Why Opus?** On challenged network paths (high jitter, limited bandwidth), PCM costs 152 kbps on the wire. Opus at 8 kbps costs 15–31 kbps depending on frame size, with quality that is transparent over a ±5 kHz FM communications channel (300–3000 Hz passband).

#### Choosing a frame size

The only valid values of `opus_frame_ms` are 20, 40 and 60. Each node sets its own, and every rusrp, usrp-rc and opus-gateway receives all three. (Opus itself has other sizes, but sequence numbers count 20 ms frames and receivers decode up to 60 ms.)

Measured at `opus_bitrate = 8000`, with the USRP and IP/UDP headers:

| Frame | Bandwidth | Packets/s | Added delay | One lost packet |
| --- | --- | --- | --- | --- |
| 20 ms | 31.2 kbps | 50 | none | 20 ms gap |
| 40 ms | 19.0 kbps | 25 | +20 ms | 40 ms gap |
| 60 ms | 14.7 kbps | 16.7 | +40 ms | 60 ms gap |
| PCM | 152 kbps | 50 | none | 20 ms gap |

- **20 ms** suits most links — cable, fiber, cellular with spare capacity. Least delay, shortest gap for a lost packet, and one Opus packet per PCM packet.
- **60 ms** suits links where packet rate is the limit — RF mesh such as AREDN, satellite. A third of the packets.
- **40 ms** sits between.

Opus itself is no more efficient with longer frames; SILK always codes 20 ms at a time. A longer frame only sends fewer headers, which is why the savings shrink past 60 ms.

Set `jitter_buffer_ms` to at least two packets: 60 for 20 ms frames, 120 for 60 ms.

**Packet format:** Opus packets use USRP type 7 (`USRP_TYPE_OPUS`), a 32-byte standard USRP header followed by the raw Opus frame bytes. Length is determined by the UDP packet size. An Opus transmission is all type 7: a 20 ms frame of encoded silence keys it, and a bare 32-byte header with `keyup = 0` unkeys it.

**Sequence numbers:** the counter advances by one per 20 ms of audio: 1 for the key frame, then `opus_frame_ms / 20` per packet. The decoder splits each decoded frame into 160-sample (20 ms) sub-frames and pushes them to the jitter buffer in order.

**Example Opus settings:**
```ini
[audio]
codec           = opus
opus_bitrate    = 8000    # 8 kbps VBR — transparent for communications voice
opus_frame_ms   = 20      # 20, 40 or 60; see Choosing a frame size

[network]
jitter_buffer_ms = 60     # at least 2 Opus packets of headroom
```

### usrp-reflector

`usrp-reflector` is a USRP conference bridge for a fixed set of repeaters: a drop-in for an ASL node whose only job is linking USRP channels. Each repeater gets the same port pair it had in ASL, so nothing changes on the repeaters. Stop ASL, start the reflector.

Each ASL `rxchannel = USRP/<ip>:<txport>:<rxport>` becomes one line in `/etc/rusrp/usrp-reflector.conf`:

```ini
[repeaters]
w1abc = 4811 172.17.14.36:3910    # name = rxport ip:txport
```

The reflector listens on `rxport` for that repeater and sends the others' audio to `ip:txport` *from* `rxport`. That matters: usrp-rc connects its link socket and drops anything not from exactly its `remote_host:remote_port`. Half-duplex floor: the first repeater to key holds it until it unkeys or goes quiet for `floor_timeout_ms`; then the others get an unkey.

**Codecs:** a word after a repeater's address says what that repeater is sent. `pcm` (the default) is always PCM: use it for ASL and other legacy USRP devices. `opus` is always Opus, to save bandwidth on that link. `multi` is whatever the talker sent, which suits usrp-rc and rusrp since they play either. Audio is converted at most once per transmission, and the result shared by every repeater that needs it. Opus to PCM adds no delay. PCM to Opus waits for a full frame (`opus_frame_ms` − 20 ms) plus ~6.5 ms of encoder look-ahead, the same as an Opus repeater encoding its own audio. Lost packets are covered: silence for missing PCM frames, Opus concealment for missing Opus packets. Unkeys are sent in each repeater's codec. The KEY and UNKEY log lines show the talker's codec.

rusrp and opus-gateway nodes join the same way: listen port = their `remote_port`, address = their IP and `local_port`.

```bash
sudo cp /etc/rusrp/usrp-reflector.conf.example /etc/rusrp/usrp-reflector.conf
sudo $EDITOR /etc/rusrp/usrp-reflector.conf
usrp-reflector -v                          # interactive, logs every dropped packet
sudo systemctl enable --now usrp-reflector
```

It logs each key/unkey with duration and packet count, and packets from addresses not in the config. `-v` (or `log_level = debug`) adds every dropped packet and why.

The reflector works for both PCM and Opus because it forwards raw UDP bytes without inspecting the audio payload — it only looks at the USRP header's `keyup` field for floor control. No header format changes were needed for Opus; the packet length from `recv()` implicitly carries the payload size for both types.

**Point-to-point without a reflector:** two rusrp instances can link directly by pointing at each other — no server of any kind required. Set each node's `remote_host`/`remote_port` to the other node's IP and `local_port`. This works for both PCM and Opus mode.

```
Node A: remote_host = <B IP>, remote_port = 34001, local_port = 34001
Node B: remote_host = <A IP>, remote_port = 34001, local_port = 34001
```

The reflector is only needed when three or more nodes need to share a conference.

### opus-gateway

`opus-gateway` bridges MMDVMHost's native FM protocol (PCM audio over loopback) to Opus-encoded USRP on the network side. It lets an MMDVMHost FM instance join an Opus-mode rusrp/`usrp-reflector` conference directly, without a rusrp analog leg in between — MMDVMHost talks PCM FM to it on `127.0.0.1`, it talks Opus USRP out to the network. It sends its configured frame size and receives all three (20, 40, 60 ms).

```
MMDVMHost (FM protocol, PCM, loopback) ◄──► opus-gateway ◄──► rusrp peer / usrp-reflector (Opus USRP, network)
```

MMDVMHost's `MMDVM.ini` `[FM]` network settings must point at it:

```ini
GatewayAddress=127.0.0.1
GatewayPort=34001     # matches [mmdvm] local_port in opus-gateway.conf
LocalAddress=127.0.0.1
LocalPort=32001       # matches [mmdvm] rpt_port in opus-gateway.conf
```

```bash
# The installed file is the *.example* copy — rename/copy it before first run,
# unlike rusrp.conf this isn't done for you on install.
sudo cp /etc/rusrp/opus-gateway.conf.example /etc/rusrp/opus-gateway.conf
sudo nano /etc/rusrp/opus-gateway.conf   # set [network] remote_host at minimum

sudo opus-gateway                        # uses /etc/rusrp/opus-gateway.conf
sudo opus-gateway -c /path/to/config.conf
```

Three config sections: `[mmdvm]` (where it listens for MMDVMHost, and where it sends FMD packets back), `[network]` (the rusrp peer or `usrp-reflector` to send/receive Opus USRP from), and `[audio]` (Opus bitrate/frame size — must match what the far end expects). PCM USRP peers (type-0) are also accepted on the receive path without reconfiguration, for compatibility with nodes not running Opus. See `config/opus-gateway.conf.example` for the full annotated reference.

This is a convenience utility for interactive use right now, not a supported headless service — no systemd unit yet. Run it manually in a terminal or your own process supervisor; more packaging polish is planned but not there yet.

### Digital loopback

The `-l` / `--loopback` flag routes USRP packets back to the output path internally without touching the network. This is intended for bench testing the full analog signal chain without an ASL3 server.

```bash
sudo ./build/rusrp -c rusrp.conf --loopback
```

In loopback mode:
- No UDP socket is opened; the `[usrp]` network settings are ignored
- `half_duplex` is overridden to `false` — both directions must be active simultaneously for audio to loop
- Everything else (ALSA, HID, DSP, jitter buffer, watchdog, telemetry) runs normally

To test: key the radio or assert input_active. The captured audio will play back through the output path with the configured jitter buffer delay.

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
rusrp starting, config: /etc/rusrp/rusrp.conf
logic: opened /dev/hidraw0 (gpio3, out_low=1)
usrp: listening on :32001 → 198.51.100.10:34001
alsa: opened plughw:1,0 (8 kHz mono s16le, 20 ms frames)
rusrp ready
```

In Opus mode an `opus:` line appears after the HID line:

```
rusrp starting, config: /etc/rusrp/rusrp.conf (opus)
logic: opened /dev/hidraw0 (gpio3, out_low=1)
opus: 60 ms frames, 8000 bps VBR narrowband SILK
usrp: listening on :32001 → 198.51.100.10:34001
alsa: opened plughw:1,0 (8 kHz mono s16le, 20 ms frames)
rusrp ready
```

In loopback mode the `usrp:` line is absent and the startup message notes the mode:

```
rusrp starting, config: /etc/rusrp/rusrp.conf (loopback)
logic: opened /dev/hidraw0 (gpio3, out_low=1)
alsa: opened plughw:1,0 (8 kHz mono s16le, 20 ms frames)
rusrp ready
```

Both flags may be combined (`--loopback` with `codec = opus`):

```
rusrp starting, config: /etc/rusrp/rusrp.conf (loopback) (opus)
logic: opened /dev/hidraw0 (gpio3, out_low=1)
opus: 60 ms frames, 8000 bps VBR narrowband SILK
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
input-end:  codec=pcm dur=3.0s in=-12.3pk/-18.0rms dBFS overruns=0 clips=0
output-end: codec=opus dur=2.0s out=-14.1pk/-20.3rms dBFS jitter=42.0ms overflow=0 silence=0 underruns=0 clips=0
```

**`input-end` fields:**

| Field | Meaning |
|---|---|
| `codec=` | What was sent: `pcm` or `opus` |
| `dur=X.Xs` | Transmission duration in seconds |
| `in=Xpk/Yrms dBFS` | Peak and RMS level of captured audio, accumulated over the transmission |
| `overruns=N` | ALSA capture buffer overruns — ALSA filled its ring buffer before rusrp could read it; audio frames were lost |
| `clips=N` | Samples clipped by the gain stage — non-zero means `input_gain_db` is set too high and audio is distorting |

**`output-end` fields:**

| Field | Meaning |
|---|---|
| `codec=` | What was received in this transmission: `opus` if any Opus audio arrived, otherwise `pcm` |
| `dur=X.Xs` | Transmission duration in seconds |
| `out=Xpk/Yrms dBFS` | Peak and RMS level of audio sent to the output device, accumulated over the transmission |
| `jitter=X ms` | Estimated network jitter at log time |
| `overflow=N` | USRP packets dropped because the jitter buffer was at its `jitter_buffer_ms` capacity — only happens on a sustained burst arriving faster than the 20 ms drain rate; expect 0 under normal conditions |
| `silence=N` | Playout slots where no frame was available — silence injected; reflects packet loss or a delivery gap before the frame reached us |
| `underruns=N` | ALSA playback buffer underruns |
| `clips=N` | Samples clipped by the output gain stage — non-zero means `output_gain_db` is set too high |

### Reading the numbers

A healthy output-end line looks like this:

```
output-end: codec=pcm dur=3.0s out=-14.1pk/-20.3rms dBFS jitter=8.0ms overflow=0 silence=0 underruns=0 clips=0
```

What elevated values mean:

| Field | What it tells you | What to try |
|---|---|---|
| `jitter` high | Variable packet delivery from the network or ASL server | Values up to ~30 ms are normal on typical Internet paths; sustained values above 80–100 ms indicate a congested or poor-quality path |
| `overflow` > 0 | A sustained burst arrived faster than the 20 ms drain rate and the buffer hit its `jitter_buffer_ms` capacity — genuinely rare | Increase `jitter_buffer_ms` if it recurs; this is not a sequencing symptom (the buffer no longer polices ASL3's sequence numbers, only arrival order), so persistent overflow points at a real delivery-rate problem on the network path, not routine jitter |
| `silence` > 0 | The buffer ran dry — packets simply stopped arriving for a stretch (packet loss, or a gap from Asterisk) | A consistent 6–8 frames per transmission is a known Asterisk characteristic and is benign; values above ~20 suggest packet loss on the network path; increasing `jitter_buffer_ms` will not help a genuine delivery gap, only a too-tight overflow ceiling |
| `underruns` > 0 | ALSA playback buffer ran dry — CPU couldn't keep up | System load issue; reduce other load on the SBC |
| `overruns` > 0 (input-end) | ALSA capture buffer filled before rusrp could read it | Same as underruns — system load |
| `clips` > 0 | Gain stage is clipping — audio is distorting | Reduce `input_gain_db` (for `input-end`) or `output_gain_db` (for `output-end`) |

`jitter_buffer_ms` does two things: it's the overflow ceiling on the jitter buffer (a burst beyond this depth starts dropping the oldest frame to make room for the newest), and it's how long `watchdog.c` waits after network keyup before asserting PTT (see `ptt_ready_ts` in `watchdog.c`), giving the buffer a moment to have real audio queued before the radio keys up. Raise it if `overflow` shows up in practice; otherwise the default is fine — unlike the old sequence-windowed design, this number is no longer something you need to tune away routine `late` warnings with.

When `level = debug`, a `heartbeat:` line also fires every `status_interval_sec` seconds. It shows both `in=` and `out=` levels, `input_active=` and `output_active=` flags, and all stats from both paths — useful to confirm the daemon is alive between transmissions.

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
| `config: cannot open /path/rusrp.conf` | Config file not found — check the path |
| `/path/rusrp.conf:12: ...` | Config error on that line: unknown setting, bad value, duplicate, or quoted value |
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
