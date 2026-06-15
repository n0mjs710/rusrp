# rusrp Roadmap

## Current: rusrp v1.0 (endpoint daemon)

`rusrp` is a lightweight endpoint daemon that bridges one analog radio or repeater
controller to an AllStarLink 3 (`chan_usrp`) server over UDP.  It handles full-duplex
audio, PTT/COS signaling via a CM119A USB HID device, a jitter buffer for network
variation, optional audio edge trimming, and half-duplex floor control.

This is the stable foundation everything else builds on.

---

## Next: rusrp-hub (USRP reflector with NAT traversal and optional Opus)

A second C binary in the same repository — `rusrp-hub` — that links multiple rusrp
endpoints directly, without requiring Asterisk or AllStarLink.

The primary use case is operators who want to full-time link their own repeaters over
the Internet without the overhead of app_rpt and the ASL ecosystem.

### Design constraints

- **Backward compatible**: ASL3 `chan_usrp` nodes can participate on the same
  reflector as rusrp endpoints, receiving standard PCM audio.
- **Per-endpoint capability**: any combination of (static/dynamic, PCM/Opus) must
  coexist on the same hub simultaneously.
- **Opus is primary**: Opus is expected to become the standard for rusrp-to-rusrp
  links rapidly.  PCM support is backward compatibility for ASL3 and legacy nodes.
- **Opus parameters are hardcoded**: since we control the entire ecosystem, Opus
  codec parameters are fixed constants in source.  No per-endpoint negotiation and
  no user configuration.

### Endpoint compatibility matrix

Registration and codec are independent choices.  Only PCM with static registration
is fully compatible with ASL3 today.  Any other combination requires rusrp-hub.

| Registration | Codec | Works with ASL3 direct? | Requires rusrp-hub? |
|---|---|---|---|
| static  | PCM  | Yes — this is rusrp v1.0 | No |
| static  | Opus | No | Yes |
| dynamic | PCM  | No | Yes |
| dynamic | Opus | No | Yes |

---

### USRP protocol notes

In the USRP protocol, `keyup` is a field in **every** packet.  There is no separate
key or unkey frame type — the rising and falling edges of a transmission are detected
by transitions in the `keyup` field of normal voice/Opus packets.  In rusrp, the
`usrp_build_key()` helper sends a regular `USRP_TYPE_VOICE` packet with silence in
the audio area and the appropriate `keyup` value; it does not introduce a distinct
frame type.

Standard USRP type values (from `usrp_protocol.h`):

```
type = 0: USRP_TYPE_VOICE   (PCM, 160 × s16le samples)
type = 1: USRP_TYPE_DTMF
type = 2: USRP_TYPE_TEXT
type = 3: USRP_TYPE_PING
type = 4: USRP_TYPE_TLV     (reserved — do not use)
```

We extend the type space with:

```
type = 5: USRP_TYPE_OPUS    (Opus-encoded audio, variable length — see below)
```

---

### Endpoint types: static vs dynamic

These are mutually exclusive per endpoint, independent of codec choice.

**Static endpoints** — configured in `hub.toml` with a fixed IP address and port.
The hub always includes them in fanout regardless of whether any packets have been
received from them recently.  They do not send PINGs; the hub sends them audio
immediately.  Codec type (PCM or Opus) is declared in config.  This is how ASL3
`chan_usrp` nodes connect — ASL3 has no concept of PING-based registration.  ASL3
always requires `codec = "pcm"` and expects exactly 352-byte type=0 USRP packets.

**Dynamic endpoints** — register by sending a `USRP_TYPE_PING` to the hub's public
address.  The hub records the source IP:port from the received UDP datagram (not from
any field in the packet) and adds the endpoint to the fanout table.  Codec capability
is declared in the `mpxid` field of the PING.  Dynamic endpoints are removed from the
table after `endpoint_timeout_sec` of inactivity.

An endpoint is one or the other.  A static endpoint with a known IP may also be an
Opus endpoint — these are independent choices.

**Hub startup validation**: if the hub is compiled without Opus support (`USE_OPUS`
not set) and any `[[endpoint]]` in config declares `codec = "opus"`, the hub logs an
error and refuses to start.

---

### NAT traversal (dynamic endpoints only)

The hub requires one publicly reachable UDP port.  Dynamic endpoints behind NAT
require no port forwarding, no UPnP, and no static IP.

- **Startup registration:** on launch, each dynamic endpoint sends three
  `USRP_TYPE_PING` packets to the hub's known public address at one-second intervals
  (t=0, t+1s, t+2s).  This opens an outbound NAT mapping, ensures the hub receives at
  least one PING despite UDP loss, and registers the endpoint's source IP:port.  The
  hub sends a PING reply to each received registration PING.  Sending all three
  unconditionally is correct — if two arrive, the hub updates the existing entry
  (deduplicates by source IP:port) rather than creating a duplicate.

- **Keepalive:** while idle, each dynamic endpoint sends a PING at a configurable
  interval (`hub_keepalive_sec`, default 20 s).  Aggressive NAT devices and some
  enterprise firewalls expire UDP mappings in as little as 30 s; operators who
  encounter this should reduce `hub_keepalive_sec` to 5–10 s.  20 s is a safe default
  for most consumer and ISP NAT.

- **Any received packet resets the inactivity timer.**  During an active voice
  transmission, voice packets arrive at 50/s and continuously reset the deregistration
  timer.  A long transmission cannot time out a dynamic endpoint even though no PINGs
  are sent during it.  After transmission ends, keepalive PINGs resume and maintain
  the registration.

- **Hub never initiates:** the endpoint always speaks first — the fundamental
  requirement for NAT hole-punching.

---

### Authentication

The `reserved` field (bytes 28–31, uint32 big-endian, always zero in standard USRP)
is used as a **32-bit network token on PING frames only**:

- All dynamic endpoints and the hub share the same token, set in config.
- The hub drops any PING whose token does not match.  Unregistered endpoints
  receive no audio and no reply.
- Voice and Opus frames carry `reserved = 0`.  No per-packet authentication overhead.
- Static endpoints are in the hub's fanout table by configuration; they do not send
  PINGs and are not subject to token validation.

A 32-bit token excludes random internet noise and port scanners.  This is
intentionally not cryptographic — it is a plaintext magic word appropriate for
amateur radio, where the goal is obscurity from noise, not protection from
adversaries.

```toml
[hub]
hub_token = 0xA5A5A5A5   # same value on all dynamic endpoints and the hub
```

---

### Capability signaling (dynamic endpoints only)

Dynamic endpoints declare capability in the `mpxid` field of PING frames.  Static
endpoints have their codec type set in hub config; they do not send PINGs.

An endpoint is either Opus or PCM — there is no partial capability.  `mpxid = 0`
means PCM; `mpxid` bit 0 set means Opus.

```
PING mpxid (uint32) for dynamic endpoints:
  bit 0 — endpoint uses Opus (sends and receives type=5 packets); 0 = PCM
  bits 1–31 — reserved, zero
```

Voice and Opus frames carry `mpxid = 0`.  Capability is static per registration.
ASL3 `chan_usrp` sets `mpxid = 0`, which correctly identifies it as PCM.

---

### Opus audio compression

Opus reduces on-wire bandwidth by roughly 8× for rusrp-to-rusrp links while
remaining transparent to PCM endpoints.

**Codec parameters — hardcoded in source, not configurable:**
- Sample rate: 8000 Hz (narrowband, matches the existing audio chain)
- Channels: 1 (mono)
- Frame size: 160 samples = 20 ms (matches USRP frame cadence exactly)
- Application: VOIP (speech-optimized Opus mode)
- Bitrate: fixed constant in `src/opus_codec.c`

Since we control the entire ecosystem, all Opus endpoints use identical parameters.
There is no negotiation and no per-endpoint or per-hub configuration.

#### Opus frame format (`USRP_TYPE_OPUS`, type = 5)

```
bytes  0–31:  standard USRP header (type=5, keyup as normal, seq as normal)
bytes 32–33:  uint16_t big-endian — Opus payload length in bytes
bytes 34–N:   Opus-encoded frame
```

The UDP packet is exactly `34 + opus_len` bytes — no padding.  Padding back to
352 bytes would eliminate the bandwidth reduction that motivates Opus.

The `seq` and `keyup` fields work identically to `USRP_TYPE_VOICE`.  Key and unkey
signals ride in the `keyup` field of Opus packets exactly as they do in PCM packets.
In Opus mode, rusrp sends only type=5 packets — no PCM silence frames are mixed in.

#### Hub codec model: always one transcode per frame

For each incoming frame, the hub always produces both a PCM and an Opus
representation exactly once, then fans each out to the appropriate listeners.

```
Incoming Opus frame (type=5):
  decode → PCM (one decode)
  → forward Opus bytes  to every Opus listener
  → forward PCM samples to every PCM listener

Incoming PCM frame (type=0):
  encode → Opus (one encode)
  → forward PCM samples to every PCM listener
  → forward Opus bytes  to every Opus listener
```

The same encoded or decoded buffer is reused for all listeners of that type — not
re-encoded or re-decoded per listener.  If no listeners of a given type are currently
registered, the transcode result is discarded; at 8 kHz narrowband, Opus encode or
decode takes well under 1 ms on any modern processor — not worth conditional logic to
avoid.  This eliminates the need to inspect the listener list before deciding whether
to transcode.

**Codec instances in the hub:**
- One Opus encoder — active throughout a floor session; reset on each new KEY
- One Opus decoder — active throughout a floor session; reset on each new KEY
- Both instances persist across transmissions; no allocation per frame

#### Outgoing sequence numbers

The hub maintains a separate outgoing `seq` counter per registered endpoint.  The
speaker's sequence numbers are **not** forwarded to listeners.  Each listener's `seq`
advances monotonically for that destination, so the listener's jitter buffer sees a
clean sequence regardless of how many different speakers have held the floor.

---

### Floor control

```
IDLE      → KEY (keyup=1) from A    → ACTIVE(A), fanout to all other endpoints
ACTIVE(A) → UNKEY (keyup=0) from A  → LOCKOUT (~200 ms timer)
ACTIVE(A) → network timeout on A    → LOCKOUT (~200 ms timer)
LOCKOUT   → timer expires           → IDLE
```

Floor detection uses the `keyup` field, which is present in both type=0 and type=5
packets.  Floor logic is codec-agnostic.

When A unkeys while B is transmitting, no queue is needed: B's packets have been
arriving continuously (discarded by hub during A's floor), so B's next packet wins
the floor naturally when LOCKOUT ends.

---

### Hub telemetry

The hub logs at INFO level:

| Event | Message |
|---|---|
| Dynamic endpoint registered | `endpoint registered: IP:port (pcm\|opus)` |
| Dynamic endpoint updated (re-PING) | `endpoint updated: IP:port` |
| Dynamic endpoint timed out | `endpoint timeout: IP:port` |
| Floor claimed | `floor: active — IP:port` |
| Floor released (unkey) | `floor: idle (unkey)` |
| Floor released (timeout) | `floor: idle (network timeout)` |

A periodic heartbeat at DEBUG level lists all currently registered endpoints and their
last-seen time.

---

### Changes to rusrp (endpoint)

All additions are backward compatible.  Without `dynamic_registration = true` or
`use_opus = true`, rusrp behaves identically to v1.0.

**New config keys (`[usrp]`):**

```toml
dynamic_registration = false      # true = send PING for NAT traversal registration
hub_token            = 0x00000000 # auth token (only used when dynamic_registration = true)
hub_keepalive_sec    = 20         # PING interval while idle; reduce to 5–10 on
                                  # aggressive NAT/firewalls (default 20 s)
use_opus             = false      # send type=5 Opus frames; decode received type=5
```

The existing `remote_host`/`remote_port` in `[usrp]` point to whatever server the
operator is connecting to — ASL3, rusrp-hub, or any other USRP target.
`dynamic_registration` and `use_opus` are orthogonal to the destination address.

**Watchdog changes:**
- On startup (if `dynamic_registration`): send PINGs at t=0, t+1s, t+2s
- New idle timer: send PING every `hub_keepalive_sec` when not transmitting

PING frame construction: `type=3`, `reserved=hub_token`,
`mpxid=1` if `use_opus`, else `mpxid=0`; all other fields zero.

**Capture path (if `use_opus`):**
- After `audio_proc_run`, encode PCM → Opus
- Send `usrp_build_opus(pkt, seq, keyup, opus_buf, opus_len)` with the actual
  packet length (`USRP_HEADER_LEN + 2 + opus_len`)
- `keyup` carries the transmission state exactly as in the PCM path

**Receive path:**
- `on_usrp_packet`: handle `type = USRP_TYPE_OPUS`
- Decode Opus → 160-sample PCM
- `jitter_buffer_push(jb, pkt->seq, pcm)` — identical to the PCM path
- Opus decoder state is reset on each `jitter_buffer_flush()` (new KEY from remote)

The jitter buffer, ALSA paths, audio processing, HID logic, and telemetry are
**unchanged**.  Opus decode produces the same 160-sample PCM frame the downstream
pipeline already consumes.

**New source file: `src/opus_codec.c/.h`**

```c
typedef struct opus_codec opus_codec_t;

opus_codec_t *opus_codec_create(void);
int  opus_encode_frame(opus_codec_t *, const int16_t *pcm,
                       uint8_t *out, int max_len);
int  opus_decode_frame(opus_codec_t *, const uint8_t *in, int len,
                       int16_t *pcm_out);
void opus_codec_reset_decoder(opus_codec_t *);
void opus_codec_destroy(opus_codec_t *);
```

All codec parameters are constants inside `opus_codec.c`.  `opus_codec_create`
takes no arguments.

Build dependency: `libopus-dev` (one apt package).  Added to Makefile as an optional
dependency (`USE_OPUS ?= 1`); the build falls back gracefully if libopus is absent.

---

### rusrp-hub structure

```
hub/
  main.c        — argument parsing, UDP socket, main event loop (epoll)
  config.c/h    — hub config (port, token, lockout, timeout, static endpoints)
  endpoint.c/h  — endpoint registry: static table + dynamic PING registration,
                  deduplication on re-PING, inactivity timeout
  floor.c/h     — floor state machine with lockout timer
  fanout.c/h    — per-frame distribution: transcode once, forward to all,
                  per-destination outgoing seq counter
  opus_hub.c/h  — one encoder + one decoder instance, reset on floor transitions
```

Shares `vendor/tomlc99/` and `src/usrp_protocol.c` with rusrp.  Added as a second
binary target in the Makefile.

**Hub config:**

```toml
[hub]
local_port            = 34001       # UDP port (must be publicly reachable)
hub_token             = 0xA5A5A5A5  # 32-bit auth token for dynamic registrations
lockout_ms            = 200         # post-unkey lockout
endpoint_timeout_sec  = 60          # deregister dynamic endpoint after this many
                                    # seconds with no received packets (PING or voice)

# Static endpoints — fixed IP, no PING registration required
# codec = "pcm" (default, required for ASL3) or "opus"

[[endpoint]]
address = "198.51.100.10"
port    = 34001
codec   = "pcm"
name    = "ASL3-node"

[[endpoint]]
address = "10.0.0.5"
port    = 34002
codec   = "opus"
name    = "Remote-site"
```

---

## Implementation sequence

### Step 1: rusrp-hub core — static endpoints, PCM only

- Static `[[endpoint]]` config: load IP, port, name, codec at startup
- Single UDP socket, epoll event loop
- Receive USRP type=0 (PCM) packets from any source
- Floor state machine with lockout timer
- PCM fanout: forward received frame to all endpoints except the speaker
- Per-destination outgoing `seq` counter
- Hub telemetry: floor events at INFO, heartbeat at DEBUG
- Build and wire into Makefile as second target

**Validates:** core fanout logic; ASL3 compatibility; this step is usable as a
simple PCM repeater bridge with no NAT dependency.

---

### Step 2: dynamic registration (NAT traversal)

- PING receive: validate `reserved` token, record source IP:port from datagram
- Extract codec from `mpxid` (bit 0: 1 = Opus endpoint, 0 = PCM endpoint)
- Reply with PING to confirm registration
- Deduplication: re-PING from known IP:port updates last-seen timestamp and
  refreshes capability; does not create a duplicate entry
- Inactivity timer: any received packet (PING or voice) resets the clock;
  remove endpoint after `endpoint_timeout_sec` of silence
- Dynamic endpoints participate in fanout alongside static endpoints

**Validates:** NAT traversal; can test with a simple tool (nc or short C program)
sending PINGs before rusrp dynamic_registration is implemented.

---

### Step 3: dynamic_registration in rusrp

- Add `dynamic_registration`, `hub_token`, `hub_keepalive_sec` config keys to rusrp
- On startup (if `dynamic_registration`): send PING at t=0, t+1s, t+2s
- Idle keepalive timer: send PING every `hub_keepalive_sec` when not transmitting
- PING construction: type=3, reserved=hub_token, mpxid=1 if use_opus else 0

**Validates:** rusrp ↔ rusrp-hub with plain PCM, both static and dynamic paths.

---

### Step 4: Opus in rusrp

- New `src/opus_codec.c/.h` wrapping libopus; all parameters are source constants
- Add `use_opus` config key
- Capture path: encode PCM → Opus after `audio_proc_run`; send type=5 packets
- Receive path: detect type=5; decode Opus → PCM; push to jitter buffer
- Decoder reset on `jitter_buffer_flush()` (each new remote transmission)
- `USRP_TYPE_OPUS = 5` added to usrp_protocol.h; `usrp_build_opus()` added
- Makefile: `USE_OPUS ?= 1` flag; link libopus if enabled

**Validates:** Opus rusrp endpoint ↔ rusrp-hub ↔ Opus rusrp endpoint.

---

### Step 5: Opus in rusrp-hub

- Add `opus_hub.c/.h`: one encoder instance, one decoder instance; both reset on
  each KEY transition; all parameters are source constants (same as rusrp)
- Always-transcode model: for each incoming frame, produce both PCM and Opus
  representations exactly once, then fan out the appropriate representation to each
  endpoint — no conditional logic on listener type before transcoding
- Hub startup validation: if any `[[endpoint]]` has `codec = "opus"` and
  `USE_OPUS` is not set, log error and refuse to start
- `fanout.c` updated to route PCM or Opus per endpoint's declared capability

**Validates:** full mixed network — PCM endpoints, Opus endpoints, and ASL3 on
the same reflector simultaneously.
