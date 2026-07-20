# Roadmap

This project is currently a build-system and low-level primitives
scaffold, not a working protocol. Rough order of work to get to
something that actually establishes a connection and moves data,
each step building on the last:

1. **Packet protection.** Use `fuse_crypto_derive_initial_secrets` to
   derive per-direction key/iv/hp via `fuse_crypto_hkdf_expand_label`
   (analogous to RFC 9001 Section 5.1's `quic key`/`quic iv`/`quic hp`
   labels), then wire up AES-GCM encrypt/decrypt through wolfSSL's
   `wolfssl/wolfcrypt/aes.h` for whole-packet AEAD protection.

2. **Header protection.** Mask the packet number and type bits using
   the `hp` key, as QUIC does, so on-path observers can't trivially
   correlate packet numbers across a connection.

3. **A real handshake.** Decide what fuse's handshake actually
   negotiates and in how many round trips — this is the project's
   main point of departure from QUIC and needs a design decision, not
   just an implementation, before writing code.

4. **Stream multiplexing.** Frame types for opening/closing streams
   and carrying stream data within a protected packet.

5. **Loss detection and congestion control.** Packet number spaces,
   ACK frames, RTT estimation, and a congestion controller (even a
   simple one) before this is usable over a real network.

6. **Connection migration.** Revisit `fuse_connection_id` usage once
   there's a real handshake — this is the payoff for having connection
   IDs at all.

Each of these is substantial on its own; treat this list as a
dependency order, not a sprint plan.
