# rusrp Roadmap

## Current: rusrp (endpoint daemon)

`rusrp` is a lightweight endpoint daemon that bridges one analog radio or repeater
controller to an AllStarLink 3 (`chan_usrp`) server over UDP.  It handles full-duplex
audio, PTT/COS signaling via a CM119A USB HID device, a jitter buffer for network
variation, and optional audio edge trimming.

This is the stable foundation everything else builds on.

---

## Next: rusrp-hub (USRP reflector daemon)

A second program in the same repository — `rusrp-hub` — that links multiple rusrp
endpoints directly, without requiring Asterisk or AllStarLink.

The primary use case is operators who want to full-time link their own repeaters over
the Internet and don't need, use, or want the overhead of app_rpt and the ASL
ecosystem just to connect a handful of sites.

### What a reflector does

A USRP reflector receives audio from the active transmitting endpoint and copies it to
all other registered endpoints.  Because USRP carries uncompressed linear PCM, no
decoding or re-encoding is required — packets are forwarded as-is.  The hub is a
packet router, not a conference bridge.

### Floor control

Only one endpoint transmits at a time.  Floor control follows first-in wins, which
mirrors analog RF behavior.

```
IDLE      → KEY received from A    → ACTIVE(A), fanout to all other endpoints
ACTIVE(A) → UNKEY from A           → LOCKOUT (~200 ms timer)
ACTIVE(A) → network timeout on A   → LOCKOUT (~200 ms timer)
LOCKOUT   → timer expires          → IDLE
```

The 200 ms lockout gives the human ear time to register that the channel has cleared
before a new transmission begins — the same gap analog repeater controllers enforce.

When A unkeys while B is already transmitting, no explicit queue is needed: B has been
sending frames continuously (the hub was discarding them), so B's next frame arrives
within one 20 ms period and wins the floor naturally when LOCKOUT ends.

### NAT traversal

NAT compatibility is a first-class design goal.  The biggest operational pain point
with ASL is the requirement for port forwarding or a public IP on every node.
rusrp-hub eliminates this:

- **Startup registration:** on launch, each endpoint sends a USRP `PING` (type=3)
  packet to the hub's known public address.  This opens an outbound NAT mapping.
  The hub records the packet's source IP:port as the endpoint's registered address
  and sends a PING reply to confirm registration.
- **Keepalive:** while idle (no active transmission), each endpoint sends a PING to
  the hub every ~20 seconds.  Most NAT devices expire UDP mappings after 30–60 s,
  so this cadence keeps the translation entry alive indefinitely.
- **During transmission:** voice frames flow at 50 packets/second, which is far more
  than enough to maintain any NAT mapping.  No separate keepalive is sent while
  a transmission is active.
- **Hub requirement:** one publicly reachable UDP port.  Endpoints require no open
  inbound port, no port forwarding, no UPnP, and no static IP address.

The hub never initiates a connection to an endpoint.  The endpoint always speaks
first, which is the fundamental requirement for UDP NAT hole-punching.

### Authentication

The `reserved` field (offset 28, uint32, big-endian) in the USRP header is always
zero in standard protocol use.  rusrp-hub defines it as a **32-bit network token**
on PING frames:

- All endpoints and the hub share the same token, set in config.
- The hub drops any PING whose token does not match — the endpoint is never
  registered and receives no audio.
- Voice frames leave `reserved = 0`; only the registration PING is gated.

A 32-bit shared value (~4 billion possibilities) is sufficient to exclude random
internet traffic and port scanners.  This is intentionally not cryptographic — it is
a cleartext magic word appropriate for amateur radio use where the goal is obscurity
from noise, not security from adversaries.

```toml
[hub]
network_token = 0xA5A5A5A5   # same value on all endpoints and the hub
```

### Implementation notes

- Target language: Rust (`tokio` async UDP, `Arc<Mutex<>>` for floor state)
- Estimated size: 300–400 lines for core logic
- Shares the USRP protocol knowledge documented in `docs/usrp_protocol.md`
- Will be built as a second Meson executable alongside `rusrp` in the same repository

---

## Future considerations

- **Alternative audio codec:** the hub v1 uses raw USRP PCM (compatible with ASL3
  nodes on the same reflector).  A future option could allow native rusrp-hub
  endpoints to negotiate a compressed codec (e.g. Opus at 16 kbps) between
  themselves, reducing bandwidth by roughly 8×.  Hub-to-ASL3 links would continue
  to use raw PCM at the boundary.  This is deferred until the basic hub is working
  and tested.

- **Translating Python projects to Rust:** separately from rusrp-hub, the intention
  is to set up a Rust development environment and translate existing Python tools
  (starting with `ipsc2hbp`) once the Rust workflow is established.
