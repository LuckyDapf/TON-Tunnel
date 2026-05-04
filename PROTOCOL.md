# HOPE — High-performance Overlay Protocol for Egress

**Protocol Name:** HOPE (High-performance Overlay Protocol for Egress)  
**Author:** lucky  
**Version:** 0.1 (draft)  
**Status:** internal implementation specification  
**Transport substrate:** TON ADNL payload channel  
**Endianness:** network byte order (big-endian) for all integer fields

## 1. Scope

This document defines the custom stream-multiplexed frame protocol used by TON Tunnel between:

- Android client transport runtime (Kotlin + JNI + native codecs)
- Windows desktop client (`TON-Tunnel/`, native C++; see `TonProtocol.cpp` / `TonClientCore.cpp`)
- Egress server node

The protocol is carried inside ADNL messages. ADNL provides datagram transport; this protocol provides stream identification and control semantics (`OPEN/DATA/CLOSE/ERROR`).

## 2. Framing Model

Each protocol frame begins with:

- `type` (`u8`)
- `stream_id` (`u32`)

Then type-specific payload follows.

Multiple frames MAY be concatenated in a single ADNL payload. Receiver must parse incrementally until input exhaustion.

## 3. Message Types

- `1` — `OPEN`
- `2` — `DATA`
- `3` — `CLOSE`
- `4` — `ERROR`

Unknown type values MUST be treated as decode failure in current implementation.

## 4. Wire Formats

## 4.1 OPEN (Client -> Server)

Purpose: request creation of an outbound TCP stream.

Binary layout:

- `type: u8` = `1`
- `stream_id: u32`
- `host_len: u16`
- `host: bytes[host_len]` (UTF-8 host string)
- `port: u16`
- `token_len: u16`
- `token: bytes[token_len]` (auth token)

Validation constraints (current implementation):

- `host_len <= 65535`
- `token_len <= 65535`
- `port in 1..65535`

## 4.2 OPEN ACK (Server -> Client)

OPEN ACK is encoded as an `OPEN` frame with only base header:

- `type: u8` = `1`
- `stream_id: u32`

No host/port/token fields are present in ACK direction.

## 4.3 DATA (Bidirectional)

Binary layout:

- `type: u8` = `2`
- `stream_id: u32`
- `payload_len: u32`
- `payload: bytes[payload_len]`

Implementation notes:

- Empty `DATA` may be dropped by sender logic in stability mode.
- Sender may segment larger payloads into chunks (currently 1200-byte target chunks in active implementation paths).

## 4.4 CLOSE (Bidirectional)

Binary layout:

- `type: u8` = `3`
- `stream_id: u32`

Purpose:

- Explicit stream termination notification.

## 4.5 ERROR (Server -> Client, optionally bidirectional in model)

Binary layout:

- `type: u8` = `4`
- `stream_id: u32`
- `msg_len: u16`
- `message: bytes[msg_len]` (UTF-8 diagnostic text)

Purpose:

- Explicit open/data failure context (authorization, policy, connect failure, etc.).

## 5. Stream State Machine (Logical)

Client-side logical flow:

1. Allocate `stream_id`
2. Send `OPEN`
3. Wait for one of:
   - `OPEN ACK` (`OPEN`) -> stream established
   - `ERROR` -> open failed
   - `CLOSE` during pending-open -> fail after short grace window
   - timeout -> fail
4. Exchange `DATA`
5. `CLOSE` from either side terminates stream

Server-side logical flow:

1. Receive `OPEN`
2. Validate policy/auth/host/port
3. Resolve destination and connect TCP
4. On success:
   - register stream
   - send `OPEN ACK`
5. Relay `DATA` in both directions
6. On failures or remote EOF:
   - send `ERROR` or `CLOSE` as appropriate
   - tear down stream resources

## 6. Multiplexing

- `stream_id` is the sole demultiplexing key.
- Frames for different streams may be interleaved in any order.
- Receiver queues per-stream frames and dispatches to stream handlers.

## 7. Error Handling Rules

Decode failure conditions include:

- Frame shorter than required header
- Declared variable-length fields exceeding available bytes
- Invalid field bounds

On decode failure, current implementation typically drops the malformed input unit and logs an error.

## 8. Reliability and Ordering Characteristics

The frame protocol itself does not define full reliability primitives (sequence numbers, ACK ranges, retransmission contract).  
Operational behavior relies on ADNL transport behavior and implementation-level timeout/close handling.

Consequences:

- Reordering can occur
- `CLOSE` may race with pending `OPEN` in edge cases
- Throughput/latency behavior is implementation-sensitive under loss/jitter

## 9. Security-Relevant Fields

- `token` in `OPEN` is used for server-side authorization checks.
- `host` and `port` are policy-validated on server side.
- Server may reject:
  - unauthorized token/client
  - blocked host/address classes
  - disallowed destination ports

## 10. Implementation References

- Client frame construction/parse (Android):
  - `app/src/main/java/com/example/dapf/tongate/data/native/NativeTonTransport.kt`
- Desktop client framing / ADNL bridging (Windows C++):
  - `TON-Tunnel/TonProtocol.cpp`
  - `TON-Tunnel/TonClientCore.cpp`
- Server codec:
  - `ton-egress-node/include/protocol.hpp`
  - `ton-egress-node/src/protocol.cpp`
- Server frame dispatch/stream lifecycle:
  - `ton-egress-node/src/egress_node.cpp`

