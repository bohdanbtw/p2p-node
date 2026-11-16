# Voice quality measurement for field tests

Purpose: collect reproducible voice-quality statistics from different countries / networks.

## What to measure (per run)

| Metric | Source in code | How obtained |
|---|---|---|
| `rms_dbfs` | `VoiceEngine::mic_test_stats()` | `p2pnode --record-mic file.wav --seconds 10` |
| `peak` | same | same |
| `clipped` | same | same |
| `SNR` (dB) | `VoiceEngine::loopback_test(snr_db, kbps, err)` | `p2pnode --selftest` (last line reports PASS + SNR) |
| `bitrate_kbps` | `loopback_test()` | same |
| `jitter_ms` | `VoiceStats::jitter_ms` | from `stats()` (network call) |
| `lost` packets | `tx_packets - rx_packets` | from `stats()`; link loss |
| `packet_loss_pct` | `100 * (tx - rx) / tx` | derived; link loss |
| `rx_packets` / `tx_packets` | `NodeStats` | from `stats()` |
| `frames_lost` | `VoiceStats::lost` | frames the listener never heard |
| `rx_gap_lost` | `VoiceStats::rx_gap_lost` | sequence gaps seen on arrival |
| `jitter_overflow` | `VoiceStats::jitter_overflow` | frames dropped by the buffer cap |
| `playout_skips` | `VoiceStats::playout_skips` | playout forward jumps |

> **Two different losses.** `tx - rx` is *link* loss (what the network dropped).
> `VoiceStats::lost` is *playout* loss (20 ms frames the listener never heard, each
> counted once). Network loss is a subset of playout loss: a dropped packet is also a
> missing frame, but a frame can additionally be discarded by the reorder buffer under
> host scheduling pressure and still have arrived on the wire.
>
> Cite `tx - rx` for network quality and `VoiceStats::lost` for what the ear receives.
>
> Both figures were previously broken: `lost` is incremented at `voice.cpp` when the
> reorder buffer overruns *and* again when playout skips the resulting hole, so one
> missing slot was counted twice — a 20 s run with `rx=1000/1000` and zero network loss
> once reported 1264 lost frames, more than the 1000 sent. Overrun now advances playout
> immediately and counts the hole exactly once, and the components are exposed
> separately as `rx_gap_lost`, `jitter_overflow` and `playout_skips`.

## Quick measurement script (Linux / MSYS2)

```bash
# 1. Self-test with SNR measurement (prints PASS + SNR; --report writes JSON)
p2pnode --selftest

# 2. Microphone capture (10 s, 48 kHz, mono)
p2pnode --record-mic /tmp/mic_$(date +%H%M%S).wav --seconds 10

# 3. Save node stats after a short voice call (2 peers connected via relay)
p2pnode --report /tmp/voice_report.json --wait-ms 30000

# 4. WAN transport benchmark without a microphone or GUI (both sides, 20 s)
#    Passive side listens; the active side connects. Each prints one
#    VOICE_BENCH {...} JSON line with tx/rx/net_lost/net_loss_pct/reordered/
#    engine_lost/jitter_ms/bitrate_kbps.
p2pnode --headless --port 47100 --key vps.key --alias VPS --voice-bench 20 --run-seconds 36
p2pnode --headless --port 47101 --key local.key --alias LOCAL \
        --peer <vps-ip>:47100 --voice-bench 20 --run-seconds 36
```

`--voice-bench` sends real encrypted Opus frames through `Node::send_voice` and decodes them
with `VoiceEngine`, so the jitter/loss figures come from a live call path without needing an
audio device.

## Expected JSON output format (`voice_report.json`)

```json
{
  "timestamp_utc": "2026-10-24 22:00:00 UTC",
  "test_version": "1",
  "node_id": "...",
  "nat_profile": "mapped: ... discovered: yes",
  "relay_used": "yes",
  "selftest": {
    "passed": true,
    "snr_db": 33.69,
    "kbps": 32.9
  },
  "mic_test": {
    "file": "mic_220000.wav",
    "duration_s": 10,
    "samples": 480000,
    "rms_dbfs": -18.50,
    "peak": 31200,
    "clipped_samples": 0
  },
  "call_stats": {
    "state": "active",
    "jitter_ms": 4.2,
    "lost_total": 3,
    "packet_loss_pct": 0.19,
    "bitrate_kbps": 32.9,
    "tx_packets": 1542,
    "rx_packets": 1539,
    "dropped": 2,
    "frames_lost": 4,
    "rx_gap_lost": 3,
    "jitter_overflow": 1,
    "playout_skips": 1
  }
}
```

`lost_total` / `packet_loss_pct` are link loss (`tx - rx`). `frames_lost` is the
listener's view and is normally greater than or equal to `lost_total`.

This file is produced by `--report FILE` when a voice session is active.
