# NOTES — Low-Latency Media Streaming over a Lossy Relay

## Problem

Move 20ms audio/video frames from a harness `source` to a harness `player`
across a hostile relay (`relay.py`) that drops, delays, duplicates, and
reorders packets according to a hidden profile. The receiver must hand each
frame to the player before its **playout deadline**
(`t0 + delay_ms + i*20ms`). Score is judged on two hard caps:

- **deadline-miss rate ≤ 1.00%**
- **bandwidth overhead ≤ 2.00x** (total bytes sent both directions, relative
  to raw payload bytes)

Among valid runs, **lower playout delay wins**. So the real optimization
target is: *how low can `delay_ms` go while staying under both caps* for a
given loss/jitter profile.

## Architecture

```
source --UDP--> [sender.cpp] --UDP--> relay.py --UDP--> [receiver.cpp] --UDP--> player
                     ^                                        |
                     |________________ NACK (47003->47004) ___|
```

- `sender.cpp` (port 47010 in / 47001 out to relay, 47004 in for feedback):
  receives frames from the harness source, forwards them immediately, and
  proactively sends a **budgeted immediate duplicate** for most frames. Also
  keeps a ring buffer (`RING_SIZE=2048`) of recent frames so it can serve
  NACK-based retransmit requests from the receiver.
- `receiver.cpp` (port 47002 in from relay, 47020 out to player, 47003 out
  for NACKs): reorders/dedups incoming frames by seq, plays out frames
  in order against their deadline, and sends NACKs for missing frames it
  still has time to recover before the deadline.

## Key design decisions

### 1. Redundancy strategy: immediate duplication, not FEC or pure ARQ
The comment in `sender.cpp` explains the reasoning: at ~2% base loss and a
tight (<100ms) playout delay, end-of-block FEC and NACK round-trip times are
too slow relative to the frame interval (20ms) to reliably recover a lost
frame before its deadline. So the sender spends most of the 2x bandwidth
budget on **sending a second copy of each frame immediately**, which:
- Survives independent single-packet loss on the up or down leg with much
  higher probability than a single copy.
- Doesn't depend on RTT, unlike NACK/ARQ.
- Costs bandwidth linearly and predictably, which is easy to budget against
  the 2.0x cap.

### 2. Duplicate budgeting (`DUP_QUOTA` / `DUP_PERIOD`)
`sender.cpp` and `receiver.cpp` both hardcode:
```
DUP_PERIOD = 1500
DUP_QUOTA  = 1407      # ~1.998x uplink budget
```
Every frame is sent once; frames where `(seq % 1500) < 1407` (~93.8% of
frames) get a second immediate copy. This yields uplink overhead of
`(1500 + 1407) / 1500 ≈ 1.938x` before NACK retransmits, leaving headroom
under the 2.0x cap for NACK-triggered retransmissions on the remaining
~6.2% of frames plus the (uncounted against uplink, but present) downlink
NACK bytes.

Both sender and receiver must agree on this schedule independently since
the receiver uses `frame_got_duplicate(seq)` to decide **whether to bother
NACKing** a missing frame at all — if the frame doesn't get a duplicate, the
receiver is more aggressive about requesting it. This is a fixed,
seq-derived function so no coordination messages are needed to keep the two
sides in sync.

### 3. NACK fallback for the ~6% of frames without a duplicate
`receiver.cpp`'s `advance_playout()`:
- Only fires a NACK if there's enough playout slack left
  (`now + NACK_RTT_S < deadline`, `NACK_RTT_S = 90ms`) — no point retransmit
  requesting a frame that can't possibly arrive in time.
- Limits to `MAX_NACKS_PER_SEQ = 2` retries, spaced `NACK_INTERVAL_S = 20ms`
  apart, to avoid burning bandwidth chasing a frame that's simply gone.
- Drops the wait and advances `expected_seq` once the deadline passes,
  matching the score rule that a late frame counts as a miss regardless.

### 4. Playout / reorder buffer
`receiver.cpp` buffers out-of-order arrivals in an `unordered_map<seq,
Frame>` keyed by sequence number, and only ever hands frames to the player
in strict sequence order (`expected_seq` monotonically increases). Stale
entries below `expected_seq` are trimmed each pass (`trim_buffers()`) to
bound memory.

### 5. Deduplication
Both duplicate copies and NACK-triggered retransmits can cause the same
`seq` to arrive twice. `handle_frame()` in `receiver.cpp` drops any packet
whose `seq` is already buffered or already played out
(`seq < expected_seq || frame_buffer.count(seq)`), so duplicates never
reach the player twice and never get counted twice by the harness (the
harness itself also only keeps `first_arrival`, but the receiver shouldn't
rely on that).

## Wire format

```
PT_FRAME (1): [type:1][seq:4 BE][payload:160]   sender <-> relay <-> receiver
PT_NACK  (3): [type:1][seq:4 BE]                receiver -> relay -> sender
```
`PT_FRAME` on the harness legs (source<->sender, receiver<->player) drops
the leading type byte per `endpoints.py`'s framing
(`4-byte seq + 160-byte payload`).

## Tuning knobs for future iterations

- `DUP_QUOTA` / `DUP_PERIOD` — trade uplink overhead vs. single-copy loss
  resilience. Could be made adaptive to the observed loss rate instead of
  a fixed ~93.8%, since a milder profile (e.g. `A_mild`, loss 0.02) can
  likely tolerate a lower duplicate ratio and spend the freed bandwidth
  budget on more aggressive NACKing instead, or on lowering `delay_ms`.
- `NACK_RTT_S` (90ms) — currently a fixed assumption; the actual RTT is a
  function of the profile's `delay_min_ms`/`delay_max_ms` plus any `spike`.
  Could be estimated online from NACK round-trip measurements.
- `delay_ms` itself is an experiment parameter, not compiled in — it's
  swept externally via `run.py --delay_ms`. The real tuning task is
  bisecting the lowest `delay_ms` that stays valid per profile.

## Known risks / edge cases

- `DUP_QUOTA=1407` bakes in an assumption that NACK retransmits are rare
  enough to stay under 2.0x uplink combined with the ~1.938x duplicate
  baseline. Under `B_moderate` (loss 0.05) this may not hold — worth
  re-checking the overhead numbers specifically against `profiles/B.json`,
  not just `A.json` (see RUNLOG.md).
- Burst loss (`burst_loss` in `Impair`) is not currently modeled in the
  duplicate/NACK strategy at all — both are loss-model-agnostic, which is
  fine for correctness but may not be bandwidth-optimal under bursty
  profiles.
- `RING_SIZE = 2048` in the sender must stay larger than any plausible
  NACK-triggered look-back window; at `delay_ms` up to a few hundred ms and
  20ms/frame, this is currently comfortable headroom (~40s of frames).