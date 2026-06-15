# rusrp Roadmap

## Current: rusrp v1.0 (endpoint daemon)

`rusrp` is a lightweight endpoint daemon that bridges one analog radio or repeater
controller to an AllStarLink 3 (`chan_usrp`) server over UDP.  It handles full-duplex
audio, PTT/COS signaling via a CM119A USB HID device, a jitter buffer for network
variation, optional audio edge trimming, and half-duplex floor control.

This is the stable foundation everything else builds on.

---

## Next: rusrp-hub (USRP reflector with NAT traversal and optional Opus)

A second program in the same repository — `rusrp-hub` — that links multiple rusrp
endpoints directly, without requiring Asterisk or AllStarLink.

The primary use case is operators who want to full-time link their own repeaters over
the Internet and don't need the overhead of app_rpt and the ASL ecosystem just to
connect a handful of sites.

### Design constraints

- **Backward compatible**: ASL3 `chan_usrp` nodes can participate on the same
  reflector as rusrp endpoints, using standard USRP PCM throughout.
- **Per-endpoint capability**: any combination of (NAT traversal, Opus) must
  coexist.  One endpoint may use Opus over NAT while another uses plain USRP
  with a static IP.
- **Hub is the codec boundary**: the hub performs all Opus encode/decode so
  endpoints need only talk to the hub, never to each other.

---

### What the reflector does

The hub receives audio from the active transmitting endpoint and copies it to all
other registered endpoints.  Because the hub handles codec translation, a PCM
endpoint and an Opus endpoint can share the same reflector transparently.

```
Endpoint A (PCM, static IP) ──────┐
Endpoint B (PCM, NAT)  ───────────┤
Endpoint C (Opus, NAT) ───────────┼──► rusrp-hub ──► fanout per-endpoint capability
Endpoint D (Opus, static IP) ─────┤
ASL3 chan_usrp node ───────────────┘
```

---

### NAT traversal

NAT compatibility is a first-class design goal.  The hub requires one publicly
reachable UDP port.  Endpoints behind NAT require no port forwarding, no UPnP,
and no static IP.

- **Startup registration:** on launch, each endpoint sends a USRP `PING` (type=3)
  to the hub's known public address.  This opens an outbound NAT mapping and
  registers the endpoint's source IP:port with the hub.  The hub sends a PING
  reply to confirm.
- **Keepalive:** while idle (no active transmission), each endpoint sends a PING
  every 20 seconds.  Most consumer NAT devices expire UDP mappings in 30–60 s;
  20 s keeps the entry alive indefinitely.
- **During transmission:** voice frames arrive at 50 packets/second — far more
  than enough to maintain any NAT mapping.  No separate keepalive is sent while
  transmitting.
- **Hub never initiates:** all contact is endpoint → hub first, which is the
  fundamental requirement for NAT hole-punching.

---

### Authentication

The `reserved` field (bytes 28–31, uint32 big-endian) in the USRP header is always
zero in standard protocol use.  rusrp-hub defines it as a **32-bit network token**
on PING frames only:

- All endpoints and the hub share the same token, set in config.
- The hub drops any PING whose token does not match.  Unregistered endpoints
  receive no audio and no reply.
- Voice frames always carry `reserved = 0`.  There is no per-frame authentication
  overhead on the hot path.

A 32-bit token (~4 billion possibilities) reliably excludes random internet noise
and port scanners.  This is intentionally not cryptographic — it is a plaintext
magic word appropriate for amateur radio use, where the goal is obscurity from
noise, not protection from adversaries.

```toml
[hub]
hub_token = 0xA5A5A5A5   # same value on all endpoints and the hub
```

---

### Capability signaling

Capability is declared in the `mpxid` field of PING frames (always 0 in standard
USRP, so standard nodes register as PCM-only automatically):

```
PING mpxid field (uint32):
  bit 0 — endpoint can encode and send Opus (TX capable)
  bit 1 — endpoint wants to receive Opus from the hub (prefers Opus)
  bits 2–31 — reserved, zero
```

Voice frames always carry `mpxid = 0`.  Capability is static per registration;
no renegotiation during a transmission.

---

### Opus audio compression

Opus reduces bandwidth by approximately 8× on links that support it, while
remaining completely invisible to PCM-only endpoints.

**Codec parameters:**
- Sample rate: 8000 Hz (narrowband, matches the existing audio chain)
- Frame size: 160 samples = 20 ms (matches USRP frame cadence)
- Mode: VOIP (speech-optimized)
- Bitrate: 16 kbps default, configurable in hub (`opus_bitrate`)
- Typical payload: ~40 bytes per frame (vs 320 bytes PCM)

#### Opus frame type

A new USRP type value carries Opus-encoded audio:

```
type = 4: USRP_TYPE_OPUS_VOICE
```

Packet layout (352 bytes total, same fixed size as all USRP packets):
```
bytes  0–31:  standard USRP header (type=4, keyup, seq as normal)
bytes 32–33:  uint16_t big-endian — Opus payload length in bytes
bytes 34–N:   Opus-encoded frame (typically ~40 bytes at 16 kbps/20 ms)
bytes N+1–351: zero padding
```

The `seq` and `keyup` fields work identically to type=0 VOICE.  The receiving
endpoint's jitter buffer uses `seq` for ordering after Opus decoding.

Standard USRP implementations (ASL3 `chan_usrp`) ignore unknown type values,
so type=4 frames are silently dropped if they reach a non-Opus peer — though
the hub ensures they never do.

#### Hub internal model: always PCM

The hub works in PCM internally.  All Opus conversion happens at the edge.
This keeps the floor control and fanout logic simple and codec-agnostic.

```
Inbound Opus (type=4) ──► decode → PCM ──► fanout logic
Inbound PCM  (type=0) ──────────────────► fanout logic

Fanout logic ──► PCM endpoint:  encode not needed → send type=0 VOICE
             └─► Opus endpoint: encode PCM → Opus → send type=4 OPUS_VOICE
```

#### Codec instance allocation

| Speaker codec | Listener codec | Hub action |
|---|---|---|
| PCM | PCM | Forward type=0 as-is |
| PCM | Opus | One encoder per Opus listener, send type=4 |
| Opus | PCM | One decoder for speaker, send decoded type=0 |
| Opus | Opus | One decoder for speaker + one encoder per Opus listener |
| Any | ASL3 (standard USRP) | Always type=0 PCM — ASL3 never receives Opus |

For a 5-endpoint hub, worst case: 1 decoder + 4 encoders active simultaneously.
Opus at 8 kHz narrowband is on the order of microseconds per frame.

**Encoder state** persists per Opus-receiving endpoint across transmissions for
smooth quality.  Reset on endpoint re-registration.  
**Decoder state** resets on each new KEY (each transmission begins from a fresh
encoder on the far end).

---

### Floor control

Only one endpoint transmits at a time.  First-in wins, mirroring analog RF
behavior.  The lockout timer gives the ear time to register that the channel
cleared before a new transmission begins.

```
IDLE      → KEY from A (any codec)    → ACTIVE(A), fanout to all other endpoints
ACTIVE(A) → UNKEY from A              → LOCKOUT (~200 ms timer)
ACTIVE(A) → network timeout on A      → LOCKOUT (~200 ms timer)
LOCKOUT   → timer expires             → IDLE
```

When A unkeys while B is already transmitting, no queue is needed: B has been
sending frames continuously (discarded by hub during A's floor), so B's next
KEY frame arrives within one 20 ms period and wins naturally when LOCKOUT ends.

KEY/UNKEY detection uses the `keyup` field, which is present in both type=0 and
type=4 frames.  Floor logic is codec-agnostic.

---

### Changes to rusrp (endpoint)

The following additions make rusrp compatible with rusrp-hub.  Everything is
backward compatible: without `hub_mode = true`, rusrp behaves identically to v1.0.

**New config keys (all in `[usrp]`):**

```toml
hub_mode     = false      # enable PING registration and keepalive
hub_token    = 0x00000000 # must match hub's hub_token (only used if hub_mode = true)
use_opus     = false      # send Opus frames; decode received Opus frames
opus_bitrate = 16000      # Opus encoding bitrate in bits/sec
```

**Watchdog changes:**
- On startup (if `hub_mode`): fire one PING immediately to register
- New idle keepalive timer: send PING every 20 s when not transmitting
- PING frame: `reserved = hub_token`, `mpxid = capability_flags` (bit 0 and/or bit 1
  set if `use_opus = true`), `type = 3`, all other fields zero

**Capture path (if `use_opus`):**
- After `audio_proc_run`, encode PCM → Opus via encoder instance
- Call `usrp_build_opus()` instead of `usrp_build_voice()`
- KEY and UNKEY frames are still type=0 with 320 bytes of silence (no Opus wrapping)

**Receive path:**
- `on_usrp_packet`: handle `type = USRP_TYPE_OPUS_VOICE`
- Decode Opus → 160-sample PCM via decoder instance
- Push decoded PCM to `jitter_buffer_push()` — identical to today
- Decoder state reset on each `jitter_buffer_flush()` (new KEY from remote)

The jitter buffer, ALSA paths, audio processing, HID logic, and telemetry are
**unchanged**.  Opus decode produces the same 160-sample PCM frame the rest of
the pipeline already consumes.

**New source file: `src/opus_codec.c/.h`**

```c
/* Thin wrapper around libopus encoder/decoder. */
typedef struct opus_codec opus_codec_t;

opus_codec_t *opus_codec_create(int sample_rate, int bitrate);
int  opus_encode_frame(opus_codec_t *, const int16_t *pcm,
                       uint8_t *out, int max_len);
int  opus_decode_frame(opus_codec_t *, const uint8_t *in, int len,
                       int16_t *pcm_out);
void opus_codec_reset_decoder(opus_codec_t *);   /* call on each new RX TX */
void opus_codec_destroy(opus_codec_t *);
```

Build dependency: `libopus-dev` (one apt package; `USE_OPUS=1` in Makefile, on
by default when libopus is found).

---

### rusrp-hub structure

```
hub/
  main.c        — argument parsing, UDP socket, main event loop
  config.c/h    — hub config (port, token, lockout, bitrate, timeout)
  endpoint.c/h  — endpoint registry, PING handling, keepalive timeout
  floor.c/h     — floor state machine with lockout timer
  fanout.c/h    — per-frame distribution with per-endpoint codec selection
  opus_hub.c/h  — encoder/decoder pool (one per active speaker/listener)
```

Shares `vendor/tomlc99/` and `src/usrp_protocol.c` with rusrp.  Added as a
second binary in the Makefile alongside `rusrp`.

**Target language options:**
- **C**: keeps toolchain identical to rusrp; epoll + non-blocking UDP; estimated
  600–800 lines; same Makefile, same dependencies.
- **Rust**: `tokio::UdpSocket` async; `DashMap` for endpoint registry; clean fit
  for the async fanout pattern; ~400 lines; requires separate `Cargo.toml`.

Either language implements the same architecture above.  Decision deferred until
implementation begins.

**Hub config:**

```toml
[hub]
local_port        = 34001       # UDP port to listen on (must be publicly reachable)
hub_token         = 0xA5A5A5A5  # 32-bit auth token — same on all endpoints
lockout_ms        = 200         # post-unkey lockout before next speaker can take floor
ping_timeout_sec  = 60          # remove endpoint if no PING received for this long
opus_bitrate      = 16000       # Opus encoding bitrate for Opus-capable listeners
```

---

## Implementation sequence

1. **rusrp-hub core** — PCM-only reflector: PING registration (auth + NAT), floor
   control with lockout, PCM fanout.  No Opus yet.  Validates the NAT traversal
   design end-to-end.

2. **rusrp hub_mode** — add PING keepalive to rusrp; add `hub_token`, `hub_mode`
   config keys.  Test rusrp ↔ rusrp-hub with plain PCM.

3. **Opus in rusrp** — add `opus_codec.c`, `use_opus` config, encode/decode paths.
   Test Opus endpoint ↔ hub ↔ Opus endpoint (same-codec path).

4. **Opus transcoding in rusrp-hub** — add per-endpoint codec selection and
   encode/decode pool.  Test mixed PCM + Opus + ASL3 on same reflector.
