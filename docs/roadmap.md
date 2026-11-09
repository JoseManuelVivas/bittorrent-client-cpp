# Roadmap

This document outlines the planned evolution of the client, from a minimal single-file downloader to a fully decentralized BitTorrent peer. Each milestone references the relevant [BitTorrent Enhancement Proposal (BEP)](https://www.bittorrent.org/beps/bep_0000.html), which is treated as the source of truth for protocol behavior.

**Status legend:** ✅ Done · 🚧 In progress · ⬜ Planned

---

## Phase 0 — Project Foundation ✅

Tooling and engineering practices in place before writing protocol code.

- ✅ CMake build system (C++20, no compiler extensions)
- ✅ Strict warning set (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow`) with optional `-Werror`
- ✅ AddressSanitizer and UndefinedBehaviorSanitizer enabled in Debug builds
- ✅ Core logic split into a reusable static library (`btcore`) shared by the client and the tests
- ✅ Unit testing infrastructure: GoogleTest fetched at configure time (pinned release, SHA-256 verified) and registered with CTest
- ✅ Learning log documenting technical decisions ([`learning-notes.md`](learning-notes.md))

## Phase 1 — Core Download (BEP 3) 🚧

Download a single-file torrent end to end from a `.torrent` file and an HTTP tracker.

- 🚧 **Bencode decoder** written from scratch, with strict validation of untrusted input
  - ✅ Recursive value type (`std::variant` of integer, byte string, list and dictionary) with equality comparison
  - ✅ Dictionary key lookup
  - ⬜ Decoding of integers, strings, lists and dictionaries from raw bytes
- ⬜ **Metainfo parsing**: extract announce URL, piece length, piece hashes, file name and size
- ⬜ **Info hash**: SHA-1 over the raw bencoded `info` dictionary
- ⬜ **HTTP tracker announce**: build the request, parse the compact peer list ([BEP 23](https://www.bittorrent.org/beps/bep_0023.html))
- ⬜ **Peer wire protocol**: TCP handshake, length-prefixed messages, `bitfield`, `have`, `interested`, `choke`/`unchoke`, `request`, `piece`
- ⬜ **Request pipelining** to keep multiple block requests in flight per peer
- ⬜ **Piece verification** against SHA-1 hashes; discard and re-request corrupt pieces
- ⬜ **Concurrent download** from multiple peers with a shared work queue
- ⬜ **Output**: assemble verified pieces and write the final file to disk

## Phase 2 — Quality and Reliability 🚧

Make correctness verifiable and regressions visible.

- 🚧 Unit tests for the bencode decoder, metainfo parser and message framing
- ⬜ Continuous integration with GitHub Actions (build + tests, with sanitizers)
- ⬜ Fuzz testing of the bencode decoder and message parser (libFuzzer)
- ⬜ Timeouts, retry logic and graceful handling of misbehaving peers
- ⬜ Command-line progress reporting (pieces completed, download rate, connected peers)

## Phase 3 — Real-World Torrents

Support the torrents and trackers found in practice, not just the minimal case.

- ⬜ **Multi-file torrents**: map pieces that span file boundaries onto the correct files ([BEP 3](https://www.bittorrent.org/beps/bep_0003.html))
- ⬜ **UDP tracker protocol** ([BEP 15](https://www.bittorrent.org/beps/bep_0015.html))
- ⬜ **Multitracker metadata** (`announce-list`) with fallback ([BEP 12](https://www.bittorrent.org/beps/bep_0012.html))
- ⬜ **Resume support**: persist and verify downloaded pieces across restarts

## Phase 4 — Event-Driven I/O

Replace thread-per-peer networking with a scalable, non-blocking design.

- ⬜ Non-blocking sockets driven by an `epoll` event loop
- ⬜ Per-connection state machines for the peer wire protocol
- ⬜ Benchmark and compare against the thread-based implementation

## Phase 5 — Full Peer (Seeding)

Become a cooperative member of the swarm instead of a download-only client.

- ⬜ Accept incoming connections and serve `request` messages
- ⬜ Choking algorithm (tit-for-tat with optimistic unchoking)
- ⬜ Rarest-first piece selection

## Phase 6 — Decentralized Peer Discovery

Remove the dependency on central trackers.

- ⬜ **Extension protocol** ([BEP 10](https://www.bittorrent.org/beps/bep_0010.html)): prerequisite for the features below
- ⬜ **Peer Exchange (PEX)** ([BEP 11](https://www.bittorrent.org/beps/bep_0011.html)): learn about new peers from connected ones
- ⬜ **Magnet links and metadata exchange** ([BEP 9](https://www.bittorrent.org/beps/bep_0009.html)): start a download from an info hash alone, fetching and verifying the metadata from peers
- ⬜ **Distributed Hash Table** ([BEP 5](https://www.bittorrent.org/beps/bep_0005.html)): Kademlia-based, trackerless peer discovery over UDP

---

## Out of Scope

To keep the project focused, the following are intentionally not planned:

- Graphical user interface
- Protocol encryption (MSE/PE)
- µTP transport ([BEP 29](https://www.bittorrent.org/beps/bep_0029.html))
