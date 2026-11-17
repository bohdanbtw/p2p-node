# p2p-node

[![Language](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows-0284C7)](#building-from-source)
[![License: MIT](https://img.shields.io/badge/License-MIT-0D9488.svg)](https://github.com/bohdanbtw/p2p-node?tab=MIT-1-ov-file)
[![Downloads](https://img.shields.io/badge/Download-software.bashgate.com-1B365D)](https://software.bashgate.com/p2pnode)

Decentralized peer-to-peer (P2P) voice, text, and file communication system written in C++17. Designed to operate without central registration servers, user accounts, or third-party cloud infrastructure across both global WAN and isolated LAN segments.

Pre-built binaries for Linux and Windows are available at [software.bashgate.com](https://software.bashgate.com/) and [GitHub Releases](https://github.com/bohdanbtw/p2p-node/releases).

## Features

- **Self-Certifying Identity:** Local account generation where `NodeID = BLAKE2b-160(Ed25519_pk)`. No central directory or registration required.
- **Dual-Layer DHT Discovery:**
  - **BitTorrent Mainline DHT (BEP 5):** Global peer lookup by `NodeID` using derived presence ($K_p$) and knock ($K_k$) keys with `implied_port = 1` for simultaneous UDP hole punching without developer servers.
  - **Local Kademlia Overlay:** 160-bit XOR routing ($k = 20, \alpha = 3$) for peer discovery, PIN pairing, and offline envelope storage in isolated LAN segments without Internet access.
- **End-to-End Encryption & Ratcheting:**
  - Signed `HELLO` / `HELLO_ACK` handshake with 4-DH key agreement (`X25519` static and ephemeral keys) and `Ed25519` signatures.
  - `ChaCha20-Poly1305` AEAD encryption for all text, voice, and file streams.
  - Symmetric chain ratchet rotating keys every 64 text messages and 256 voice frames, with two-phase AEAD state commit and a 64-bit sliding replay window.
  - Out-of-band peer verification via a persistent 6-digit + 4-word Short Authentication String (SAS) derived from static identity keys.
- **NAT Traversal & Relay Fallback:**
  - Candidate ranking metric $S(n)$ combining XOR distance, measured RTT, and observed NAT profile.
  - Automatic fallback (1.4 s timeout) to an optional stateless `p2p-relay` daemon that forwards opaque encrypted envelopes by `NodeID` without access to session keys or plaintext.
- **Real-Time Voice Channel (Opus 48 kHz):**
  - 20 ms frame size, configurable bitrate (16, 24, 32, 48 kbps), and adaptive jitter buffer (80-240 ms / 4-12 frames).
  - Encoder-side in-band FEC (enabled above 2% packet loss) combined with decoder-side FEC and PLC recovery.
  - Input audio processing pipeline: 100 Hz high-pass filter, adaptive noise gate, automatic gain control (AGC), and `tanh` soft limiter.
- **Windowed File Transfer & Group Calls:**
  - Reliable UDP file streaming using 1000-byte chunks, a 16-chunk sliding window, `.part` file resumption, and end-to-end `SHA-256` verification.
  - Group messaging with DAG history synchronization (`GroupSync`) and group voice routing that switches from `full mesh` (up to 4 participants) to `p2p-SFU` (more than 4 participants).
- **Network Migration & Local Vault:**
  - Automatic session recovery across network interface changes (Ethernet, Wi-Fi, mobile hotspot) with authenticated peer address updates.
  - Encrypted local storage (`ChaCha20-Poly1305`) for contacts, message history, and offline queues, plus password-protected `P2PK1` identity export (`Argon2id` + `XChaCha20-Poly1305`).

## Protocol Stack

All traffic (KRPC Mainline DHT packets and custom `P2P1` binary frames with a 28-byte header) is multiplexed over a single UDP socket.

| Layer | Implementation |
| :--- | :--- |
| **Application & GUI** | Dear ImGui (SDL2), 1-on-1 & Group Chat, Voice Calls, File Transfer, Diagnostic Inspector |
| **Cryptography** | `libsodium`: `Ed25519`, 4-DH `X25519`, `ChaCha20-Poly1305`, `BLAKE2b-160/256`, `SHA-256`, `Argon2id` |
| **Signaling & NAT** | Signed `HELLO`/`HELLO_ACK`, STUN probing, NAT-aware candidate scoring $S(n)$, `RELAY` envelopes |
| **Discovery** | BitTorrent Mainline DHT (`jech/dht`) + Kademlia DHT ($160\text{-bit ID}, k=20, \alpha=3$) |
| **Transport & Media** | Single-socket UDP framing, 16-chunk sliding window, `libopus` 48 kHz / 20 ms, Adaptive Jitter Buffer |

## Repository Layout

```text
.
├── src/                  # C++17 source code (client node, relay, crypto, DHT, voice, GUI)
├── third_party/          # Bundled third-party dependencies (including jech/dht)
├── pkg_extra/            # Packaging assets and distribution files
├── build_linux.sh        # Build script for the Linux client (p2pnode)
├── build_relay.sh        # Build script for the headless relay/bootstrap node (p2p-relay)
├── setup_win_sysroot.sh  # Helper script to prepare the MinGW-w64 sysroot
└── build_win.sh          # Cross-compilation script for the Windows client (p2pnode.exe)
```

## Building from Source

### Dependencies (Debian / Ubuntu / Linux Mint)

```bash
sudo apt update
sudo apt install build-essential pkg-config libsodium-dev libopus-dev libsdl2-dev
```

For Windows cross-compilation on Linux, install MinGW-w64:

```bash
sudo apt install mingw-w64
```

### Build Client (Linux)

```bash
./build_linux.sh
```

Output binary: `build/p2pnode`

### Build Relay / Bootstrap Daemon (Linux)

```bash
./build_relay.sh
```

Output binary: `build/p2p-relay`

### Cross-Compile Client for Windows (from Linux)

```bash
./setup_win_sysroot.sh
./build_win.sh
```

Output binary: `build/p2pnode.exe` (with required runtime DLLs: `SDL2.dll`, `libopus-0.dll`, `libsodium-26.dll`, `libwinpthread-1.dll`).

## Usage

Run the graphical client node:

```bash
./build/p2pnode
```

Run the automated 3-node loopback self-test suite (verifies signed `HELLO`, 4-DH key exchange, ACKed chat, 50-frame Opus transmission, Opus loopback SNR, DHT lookup/store, group sync, offline delivery, and relay routing):

```bash
./build/p2pnode --selftest
```

Run a standalone headless relay / bootstrap node on a host with a public IP:

```bash
./build/p2p-relay
```

To disable external STUN probing in isolated LAN environments, set `P2P_NO_PUBLIC_STUN=1`.

## License

This project is licensed under the [MIT License](LICENSE).

Third-party libraries used by this software are distributed under their respective permissive licenses:
- `jech/dht` — MIT License (Copyright (c) 2009-2011 by Juliusz Chroboczek)
- `Dear ImGui` — MIT License
- `libsodium` — ISC License
- `libopus` — BSD 3-Clause License
- `SDL2` — zlib License
