# USRP Protocol Reference

The USRP protocol is a simple UDP-based audio transport used by the
[AllStarLink](https://www.allstarlink.org/) ecosystem (`chan_usrp` in Asterisk/ASL3)
to carry push-to-talk voice between nodes. It carries uncompressed PCM audio with a
minimal fixed header, making it easy to implement from scratch.

---

## Transport

- **Protocol:** UDP (IPv4)
- **Direction:** full-duplex; one socket, two directions
- **Default ports:** transmit to remote port **34001**, receive on local port **32001** (both configurable)
- **Socket model:** `connect()`ed UDP — a single socket is bound locally and connected to the remote address, so `send()`/`recv()` are used rather than `sendto()`/`recvfrom()`

The remote end (ASL3 `chan_usrp`) listens on its configured port and responds to the
source address of incoming packets.  Ports are symmetric by convention but not required
to be equal on both sides.

---

## Packet Format

Every packet is exactly **352 bytes**:

```
 Offset  Size  Field
 ──────  ────  ─────────────────────────────────────────────────────────
  0       4    Magic: ASCII "USRP" (0x55 0x53 0x52 0x50)
  4       4    Sequence number (uint32, big-endian)
  8       4    Memory / channel (uint32, big-endian) — see notes
 12       4    Keyup flag (uint32, big-endian) — 0 = unkeyed, non-zero = keyed
 16       4    Talker / talkgroup ID (uint32, big-endian)
 20       4    Type (uint32, big-endian) — see Packet Types below
 24       4    MPXID — multiplex ID (uint32, big-endian)
 28       4    Reserved — always 0
 ──────  ────  ─────────────────────────────────────────────────────────
 32     320    Audio payload — 160 × int16_t signed PCM samples (little-endian)
```

**Total: 352 bytes per packet.**

Header fields are in **network byte order (big-endian)**.
Audio samples are **signed 16-bit little-endian** — they are copied to/from the wire
without byte-swapping (matching the behavior of ASL3 on standard x86/ARM hardware).

### Field notes

| Field | Practical use |
|---|---|
| `magic` | Always `"USRP"` — used to validate packets on receive |
| `seq` | Monotonically increasing per transmission, reset to 0 on each new KEY |
| `memory` | Channel/memory slot identifier; set to 0 for basic use |
| `keyup` | **The PTT signal.** Non-zero = transmitter keyed; 0 = unkeyed |
| `talker` | Identifies the source; useful in hub scenarios; set to 0 for basic use |
| `type` | Payload type — in practice only `VOICE (0)` matters for audio; see below |
| `mpxid` | Multiplex ID for multi-channel use; set to 0 for basic use |
| `reserved` | Always 0 in standard use; available as an extension point (e.g. a 32-bit network token on PING frames for non-ASL hub authentication) |

---

## Packet Types

| Value | Name | Meaning |
|---|---|---|
| 0 | `VOICE` | Audio frame — the only type used in normal operation |
| 1 | `DTMF` | DTMF digit payload |
| 2 | `TEXT` | Text message payload |
| 3 | `PING` | Keepalive — no audio payload, audio field is zero |
| 4 | `TLV` | Type-length-value metadata |

In practice, nearly all packets are `VOICE (0)`.  KEY frames, UNKEY frames, and voice
frames all use `type = VOICE`.  The `keyup` field carries the PTT state regardless of
packet type.

---

## Transmission Lifecycle

A complete transmission consists of three phases:

```
  ┌─ KEY frame ──────────────────────────────────────────────────────────┐
  │  type=VOICE, keyup=1, seq=0, audio=silence (zeros)                   │
  └──────────────────────────────────────────────────────────────────────┘
          │
          │  ← jitter_buffer_ms delay before PTT asserts on receive end
          ▼
  ┌─ VOICE frames (N × 20 ms) ───────────────────────────────────────────┐
  │  type=VOICE, keyup=1 (non-zero), seq=1…N, audio=PCM samples          │
  └──────────────────────────────────────────────────────────────────────┘
          │
          ▼
  ┌─ UNKEY frame ────────────────────────────────────────────────────────┐
  │  type=VOICE, keyup=0, seq=N+1, audio=silence (zeros)                 │
  └──────────────────────────────────────────────────────────────────────┘
```

Key observations:

- **The sequence number resets to 0 on every new KEY.** The remote end always
  starts at 0 for a new transmission.  Receivers must flush their playout state on KEY
  and re-seed the playout cursor from the incoming sequence number.

- **The KEY frame carries silence audio.**  It signals "PTT has risen" but does not
  carry audio.  Receivers use the jitter buffer fill delay to absorb network latency
  before keying the output, so the KEY arrives and buffers silently for
  `jitter_buffer_ms` before the output asserts.

- **Every VOICE frame while keyed also has `keyup = non-zero`.** There is no separate
  "continue" message — the `keyup` flag is repeated in every frame.

- **The UNKEY frame also carries silence audio.**  It signals "PTT has fallen."
  Receivers should flush their jitter buffer on UNKEY so stale audio from the ending
  transmission does not bleed into the next one.

- **All frames are the same size (352 bytes)** regardless of type or whether audio is
  present.  There is no variable-length framing.

---

## Audio Format

| Parameter | Value |
|---|---|
| Encoding | Signed 16-bit PCM (linear, no compression) |
| Sample rate | 8000 Hz |
| Channels | Mono |
| Samples per frame | 160 |
| Frame duration | 160 ÷ 8000 = **20 ms** |
| Audio bytes per frame | 160 × 2 = **320 bytes** |
| Byte order | Little-endian (host order; no byte-swap on wire) |

A frame of silence is 320 zero bytes.  Every packet — including KEY, UNKEY, and PING
frames — carries a full 320-byte audio payload field; it is simply zeroed when not
carrying voice.

---

## Timing and Packet Rate

Packets arrive at a fixed rate driven by the audio clock:

| Quantity | Value |
|---|---|
| Packets per second | 8000 ÷ 160 = **50 pps** |
| Inter-packet interval | **20 ms** |
| Audio bitrate (raw) | 8000 × 16 = **128 kbps** |

The 20 ms cadence is the fundamental clock of the system.  ALSA and the USRP stream
must be kept locked to the same 20 ms frame boundary for glitch-free audio.

---

## Network Bandwidth

Per direction, per transmission (50 packets/second):

| Layer | Bytes/packet | Bits/second |
|---|---|---|
| USRP payload | 352 | 140.8 kbps |
| + UDP header (8 B) | 360 | 144.0 kbps |
| + IPv4 header (20 B) | 380 | **152.0 kbps** |
| + Ethernet header + FCS (18 B) | 398 | 159.2 kbps |
| + Ethernet preamble + IFG (20 B) | 418 | **167.2 kbps** (on the wire) |

Full-duplex (both directions simultaneously): approximately **300 kbps** at the IP
level, **335 kbps** at the Ethernet wire level.

The overhead ratio is modest: the 320-byte audio payload is 91% of the USRP packet
and 84% of the IP packet.  Bandwidth is not a practical concern on any link faster
than a 3G cellular connection.

### Comparison with G.711

G.711 (standard telephone PCM) uses 8-bit µ-law or A-law encoding at 8000 Hz —
exactly the same sample rate as USRP.  USRP uses 16-bit samples, so USRP audio is
exactly **2× the bitrate of G.711** (128 kbps vs. 64 kbps) with correspondingly
higher dynamic range and no companding distortion.

---

## Receiver Implementation Notes

### Jitter buffer

Because UDP provides no delivery ordering or timing guarantees, a jitter buffer is
required on the receive path.  Recommended depth is 40–250 ms (2–12 frames).  A
depth of 60 ms (3 frames) works well on typical internet paths; increase if `late=`
counter is non-zero in operational logs.

The buffer should be seeded from the sequence number of the first received frame of
each transmission.  On UNKEY, flush the buffer so the playout cursor resets cleanly
for the next transmission (which will start at seq=0 again).

Frames that arrive after their playout deadline should be dropped; a 32-bit signed
comparison of `(int32_t)(incoming_seq - playout_seq)` cleanly handles the case where
the buffer depth is much smaller than the 32-bit sequence space.

### KEY/UNKEY detection

Watch the `keyup` field, not the `type` field.  The transition from `keyup=0` to
`keyup≠0` is KEY; the transition from `keyup≠0` to `keyup=0` is UNKEY.  Process
both state changes even if audio is not being played (e.g. during the startup inhibit
window).

### Network timeout

The sender does not guarantee that an UNKEY frame is always delivered (the network
may drop it).  Implement a watchdog: if no USRP packet arrives for a configurable
period (500 ms is a reasonable default), force PTT release.
