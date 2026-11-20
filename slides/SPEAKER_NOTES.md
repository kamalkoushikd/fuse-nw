# Fuse presentation — speaker script

A word-for-word script for the 20-minute slot, one section per slide, in slide order. Speaking time adds up
to about **18½ minutes**, leaving about 1½ minutes for questions. At a normal pace (about 130 words a minute)
each section fits its time. **Bold** marks the phrase or number to stress. Section-divider slides
("The problem", "Approach", …) are only on screen while you start the next slide's first sentence.

The same text is in each frame's `\note{}` in `presentation.tex`, so a presenter view (for example
`pdfpc`, or `\setbeameroption{show notes on second screen=right}`) shows it next to the slide.

| # | Slide | Time | Running total |
|---|---|---|---|
| 1 | Title | 0:30 | 0:30 |
| 2 | Outline | 0:20 | 0:50 |
| 3 | Problem statement | 1:00 | 1:50 |
| 4 | Two kinds of traffic, one link | 0:45 | 2:35 |
| 5 | Why TCP and QUIC fall short | 1:15 | 3:50 |
| 6 | Goals | 0:45 | 4:35 |
| 7 | Approach in one slide | 1:15 | 5:50 |
| 8 | Architecture | 1:00 | 6:50 |
| 9 | Reliability | 1:30 | 8:20 |
| 10 | Per-stream congestion control | 1:15 | 9:35 |
| 11 | Making it fast | 1:15 | 10:50 |
| 12 | The life of one packet | 0:45 | 11:35 |
| 13 | What a full code review found | 1:00 | 12:35 |
| 14 | Version 8: what we fixed | 1:15 | 13:50 |
| 15 | Testing | 0:45 | 14:35 |
| 16 | Results: correctness under bad networks | 1:15 | 15:50 |
| 17 | Results: speed and the reliability core | 0:45 | 16:35 |
| 18 | Limitations and future work | 0:45 | 17:20 |
| 19 | Conclusion | 0:40 | 18:00 |
| 20 | References (2 slides) | 0:20 | 18:20 |
| 21 | Thank you / questions | 0:10 | 18:30 |

---

## 1. Title — 0:30

Good morning, everyone. Our project is **Fuse**, a transport protocol that we built directly on top of UDP.
In the next twenty minutes I will explain the problem it solves, how it works inside, how we reviewed and
hardened it, and what our tests and measurements show.

## 2. Outline — 0:20

Here is the plan. First the problem, and why existing transports do not solve it. Then our design. Then the
security work. And finally, the results.

## 3. Problem statement — 1:00

This is our problem statement. Many systems send **two very different kinds of data** between the same two
machines at the same time. Some data must arrive **completely**, like a file or a machine-learning model.
Other data only matters if it is **fresh**, like telemetry or control signals: a sample that arrives late is
useless.

The question is: how do we carry both over **one session**, so that each stream gets exactly the guarantee it
needs, so that loss or a big burst on one stream **never slows down or delays the other**, and so that the
whole thing is fast enough to fill a modern link? On top of that we want optional encryption that never
silently falls back to plaintext, and robustness against bad networks and basic attacks.

## 4. Two kinds of traffic, one link — 0:45

Picture it like this. The blue stream is small and frequent, and an old message is worthless; it wants low,
steady latency and would rather drop data than wait. The red stream is large, and every single byte matters;
it wants full reliability and maximum throughput. Today you usually pick **one** transport for both, and
**one of the two streams pays for that choice**.

## 5. Why TCP and QUIC fall short — 1:15

Why not just use TCP or QUIC? TCP gives you one ordered byte stream, so **one lost packet blocks everything
behind it**, including the telemetry. QUIC fixed that with many streams inside one connection. But look at the
congestion-control row: in QUIC, all streams **share one congestion window**. When the bulk stream sees loss,
the window shrinks for the telemetry stream too. QUIC also has no first-class unreliable stream, and it
**always encrypts**, which costs throughput even on a trusted link.

So the key observation is: we need the **stream**, not the connection, to be the unit of congestion control.

## 6. Goals — 0:45

These were our goals. The central one is the first: each stream owns its **reliability, its ordering and its
congestion control**. Each stream picks a policy: lossless or not, ordered or not, and whether to drop old
data under pressure. The data path should not allocate memory or take locks. Encryption is optional but
fails closed. We wanted to **measure** against a real QUIC implementation, not assume. And nothing in the
core may assume a particular workload.

## 7. Approach in one slide — 1:15

Here is the whole approach on one slide. We send **blocks** over UDP; each block carries its stream, a
sequence number, its byte offset and a timestamp. The sender keeps every unacknowledged block in a
**registry**, so it can resend it exactly. The receiver tracks a window of **1,024 blocks** in a bitmap and
reports gaps. Each acknowledgement carries the cumulative position, a bitmap of what else arrived, a
timestamp echo for measuring round-trip time, and how much more the receiver can take.

Each stream runs its own congestion control. And to be fast, we **batch** system calls, we **split** bulk
data across parallel lanes, and we **write each block straight to its final place** on disk.

## 8. Architecture — 1:00

The code is layered. At the bottom: the kernel's UDP and wolfSSL for cryptography. Above that, one place
that encodes and decodes every message, and a socket wrapper. Then reusable building blocks for
**reliability**, **concurrency** and **security**. On top sit three independent transports: a **bulk
transfer** API for files and buffers, a **socket-style** API that looks like TCP, and a **multiplexed**
transfer with many streams on one socket. Each layer only calls the layers below it.

## 9. Reliability: registry, bitmask, NACK and SACK — 1:30

This is the heart of reliability. The receiver keeps this bitmap, starting at the **base**, the first block
it has not received. Say block 5 arrives, but 3 and 4 have not. They are now marked **missing**. We do not
complain immediately; we wait about **half a round trip**, in case they were only reordered. Then we send a
**NACK** listing 3 and 4, and the sender resends exactly those blocks from its registry.

The acknowledgement also carries the bitmap, so the sender knows block 5 already arrived and **never wastes
bandwidth resending it**. That is selective acknowledgement. When 3 and 4 arrive, the base slides forward to
6. In version 8 this bookkeeping is a ring, so the cost per packet is **constant**.

## 10. Per-stream congestion control — 1:15

Every stream has its own controller, so loss on bulk data **does not shrink** the telemetry stream's window.
The controller measures round-trip time from timestamps echoed in acknowledgements, using only the sender's
own clock. Time is divided into epochs of one round trip. While the path is clean the window **doubles** —
that's slow start — then grows step by step. Any loss in an epoch **halves** it, as in the graph.

Timeouts follow the standard TCP formula, including the variance of the round-trip time, and they back off
exponentially. Sending is **paced** across the round trip instead of bursting, and the receiver tells the
sender how much it can absorb.

## 11. Making it fast — 1:15

Speed comes from three things. **Batching**: one system call moves 64 datagrams, using the kernel's
segmentation offload. **Sharding**: a file is split into lanes, each with its own socket, thread and
reliable stream, so the packet rate multiplies. And the **receive pipeline** you see here: reader threads
pass blocks to writer threads through **lock-free rings**, and the writers decrypt and write each block
directly to its final position in the file. Nothing is buffered and stitched together later. And an
acknowledgement only ever covers data that is **authentic and already written**.

## 12. The life of one packet — 0:45

Putting it together, here is one packet's journey. The sender stores it for a possible resend, encrypts it,
frames it and sends it in a batch. The receiver's reader picks it up, checks that it is new and inside the
window, and passes it to a writer. The writer decrypts it and writes it to disk, then reports back. **Only
then** is it acknowledged, and the sender frees its copy.

## 13. What a full code review found — 1:00

Once it worked, we reviewed the entire code base critically. We found **31 issues**. The worst was a
cryptographic flaw: every retransmission **reused an AES-GCM nonce** with different associated data, which
lets an observer recover the authentication key and **forge data**. Control messages were not authenticated,
so a forged acknowledgement could make the sender report data as delivered that **never arrived**. And with no
address validation, one spoofed 70-byte packet made the receiver send about **0.9 megabytes** to a victim.
There were also transport gaps: no slow start, no pacing, no flow control.

## 14. Version 8: what we fixed — 1:15

Version 8 fixes these, **one change at a time, each with its own tests**. A retransmission now resends the
**identical ciphertext**, so there is no second tag to exploit. Every control message is **signed** with a
separate key. A wrong key is detected **at the handshake**, not after twenty failed packets. A sender must
**prove it owns its address**, by echoing a token, before the receiver commits anything, and a peer's address
only changes after a challenge. Keys **rotate** automatically. And the transport gained selective
acknowledgement, better timeouts, slow start, pacing, flow control and awareness of the path MTU.

## 15. Testing — 0:45

We test with GoogleTest. All **four build configurations** — debug and release, with and without encryption —
pass every test, with compiler warnings treated as errors and tests running in parallel. Handshakes are
tested with a fake clock and injected loss. Every security fix has an **attack test**, and we checked that
those tests **fail on the old code**: before the nonce fix, our relay saw **133** resent blocks with changed
ciphertext; after it, **zero**.

## 16. Results: correctness under bad networks — 1:15

Here is the protocol under bad network conditions, measured on one machine through a network emulator,
comparing the old version 7 with version 8. The first thing to say is that **every single transfer arrived
byte-identical**, in every condition.

Under packet loss, version 8 is **much faster**: about **2.7 times** at 2 % loss and **4.4 times** at 5 %
loss, mainly because of selective acknowledgements and better timeouts. Jitter and reordering stay about the
same. On long-delay paths it is a little slower — about 11 % at a 50-millisecond round trip, and about a
quarter slower with delay plus loss — and that is **by design**: the window no longer inflates when blocks
grow, which makes it fairer to other traffic sharing the link.

## 17. Results: speed and the reliability core — 0:45

On a plain loopback, version 8 is as fast as version 7 — about **1,080 versus 1,045 megabytes per second**
here — with **fewer than half the retransmissions**. Earlier benchmarks in the repository showed
Fuse faster than QUIC with encryption on both sides, and showed small-message latency staying flat under
heavy bulk traffic.

On the right is the reliability core in action: block 3 is dropped, the base waits at 3, a NACK goes out after
the reorder delay, the resend fills the gap, and the base **jumps to 8**.

## 18. Limitations and future work — 0:45

It is not finished, and we want to be honest about that. The socket-style API still needs the same security
pass. Bulk transfers need a range of open ports. The unreliable streams — the original motivation — are built
and tested as primitives, but **not yet exposed** in a public API. Forward secrecy exists but is not wired in.
And we have not yet tested on **two physical machines**.

## 19. Conclusion — 0:40

To conclude. We wanted one session for mixed traffic in which **one stream never hurts another**. Our answer
is to make the **stream the unit of reliability and congestion control**, on raw UDP. We built it, we reviewed
it critically, we fixed what we found, and we **verified** it with tests and measurements.

## 20. References — 0:20

*(First references slide.)* These are the main sources we built on: the **QUIC and TCP specifications**, the
RFCs for retransmission timers, selective acknowledgement and path MTU discovery, and the key-derivation and
TLS standards. The full list, with every place it is used, is in the bibliography of our report.

*(Second references slide.)* The remaining references: DTLS, the GCM standard and the **attack on GCM nonce
reuse** that motivated our most important fix, SipHash for the address tokens, the classic congestion-control
papers, the Linux batching work, and the netem emulator we modelled our test relay on.

## 21. Thank you — 0:10

Thank you. We are happy to take your questions.

---

## Q&A preparation

**Where do the numbers in [brackets] come from?** They point to the references slides; for example [1] is the
QUIC specification, RFC 9000, and [12, 13] are the GCM standard and the nonce-reuse attack on it.

**Why not just open two QUIC connections?** That does isolate them, and the earlier benchmark shows it
recovers most of the latency. But you pay for two handshakes and two key schedules, lose shared state, and
the application must manage the split. Fuse gives that isolation inside one session.

**Isn't per-stream congestion control unfair to other traffic?** Yes, N lanes compete like N TCP flows on a
shared bottleneck. It is the deliberate trade-off of the design; a shared aggregate limit is possible future
work.

**Why a pre-shared key instead of TLS certificates?** The target is links between known endpoints. A PSK
avoids a PKI. The cost is no forward secrecy on the fast path; a DTLS path with ECDHE-PSK exists for when
that matters.

**How do you avoid head-of-line blocking inside a stream?** Each block carries its byte offset, so the writer
puts it straight into its final place; a missing block never stops later blocks from being written.

**What exactly was the nonce bug?** The retransmission flag was part of the authenticated data, so a resend
encrypted the same plaintext with the same nonce but different associated data. Two GCM tags under one nonce
let an attacker solve for the authentication key. Now the flag is masked, so the resend is byte-identical.

**How did you test the attacks?** With relays and fake peers in the tests: a relay that forges ACKs and
aborts with the right nonce, a spoofer without a token, a foreign address sending data, a replayed SETUP.

**Why is it slower on lossy WAN paths now?** The old window was counted in blocks, and blocks grew up to 13×,
so the bytes in flight inflated without being earned. Counting in bytes is fairer; slow start still reaches
the path's capacity.

**Does it work over IPv6?** Yes; senders now open their socket in the destination's family. Our container
had no IPv6, so that test was skipped there.
