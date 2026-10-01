# Graph Report - fuse-nw  (2026-10-01)

## Corpus Check
- 133 files · ~104,160 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1931 nodes · 3361 edges · 146 communities (127 shown, 19 thin omitted)
- Extraction: 85% EXTRACTED · 15% INFERRED · 0% AMBIGUOUS · INFERRED: 503 edges (avg confidence: 0.79)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `b2fd64c0`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- send_lane
- TEST
- PeerAddr
- ReceiverStream
- string
- SetupPayload
- TEST
- TEST
- Worker
- DtlsSession
- WorkerOrchestrator
- TEST
- SendQueue
- TEST
- connection
- SetupInitiator
- sender_thread
- Result
- TEST
- Ack
- orchestrator.cpp
- ReassemblyStream
- main.rs
- encode_ack
- TEST
- Lane
- TEST
- vector
- aux.cpp
- worker.cpp
- TEST
- TEST
- TEST
- test_orchestrator.cpp
- SenderRegistry
- SpscQueue
- TEST
- fuse_bench.cpp
- main
- OrchestratorConfig
- WorkerPool
- StreamConfig
- run_file_benchmark.sh
- registry.cpp
- TEST
- Wire
- BlockHeader
- OrchestratorStats
- RegistrySlot
- TopologyHint
- reassembly.cpp
- RetransmitRequest
- FileSink
- SdkTest
- SpscRing
- send_lane
- fuse_listener
- ResumeRanges
- SenderStreamState
- MuxTransferConfig
- TransferStats
- ReceiverStreamState
- transfer.cpp
- sdk.cpp
- UdpSocket
- PeerAddr
- TransferConfig
- digest.cpp
- MuxSender
- Fuse benchmark results — consolidated
- ConsumerState
- SetupResponder
- Connection
- MuxReceiver
- Fuse SDK
- wait_for
- udp_common.cpp
- quickstart_common.hpp
- Udp
- FuseError
- __init__.py
- mux_transfer.cpp
- Using Fuse in your project
- test_transfer.cpp
- Benchmarks
- socket.c
- quic_common.py
- Ack
- plot-benchmark.py
- LaneResult
- Outcome
- vector
- Listener
- LossyRelay
- _check
- DtlsConfig
- StreamStart
- TransferProgress
- fuse-transport
- fuse
- net-sim: two-container bridge harness
- Sink
- TEST
- quic_latbench.rs
- close
- Injector
- config.sh
- Roadmap / status
- Completion
- run_network_matrix.sh
- BatchScratch
- install.sh
- C
- _native.py
- net-sim: two-VM bridge harness
- PacketSlot
- HandshakeResult
- __main__.py
- .stats
- bench-loop.sh
- entrypoint.sh
- guest-init.sh
- bench-summary.py
- atomic
- TxMeta
- graphify.md
- graphify.md
- make_release.sh
- benchmark.sh
- compare.sh
- entrypoint.sh
- run.sh
- entrypoint.sh
- run.sh
- run.sh
- stage-bin.sh
- test_receiver.sh
- test_receiver_quic.sh
- test_sender.sh
- test_sender_quic.sh
- ms_since
- fuse-transport

## God Nodes (most connected - your core abstractions)
1. `fuse_conn` - 67 edges
2. `TEST()` - 51 edges
3. `TEST()` - 43 edges
4. `PeerAddr` - 37 edges
5. `send_lane()` - 37 edges
6. `Worker` - 36 edges
7. `UdpSocket` - 35 edges
8. `MuxSender` - 35 edges
9. `MuxReceiver` - 33 edges
10. `WorkerOrchestrator` - 32 edges

## Surprising Connections (you probably didn't know these)
- `TEST()` --calls--> `on_block`  [INFERRED]
  tests/proto/test_flags.cpp → include/fuse/proto/reassembly.hpp
- `TEST()` --calls--> `delivery_order_`  [INFERRED]
  tests/proto/test_flags.cpp → include/fuse/proto/reassembly.hpp
- `receiver_thread()` --calls--> `recv`  [INFERRED]
  bench/fuse_bench.cpp → include/fuse/proto/dtls.hpp
- `receiver_thread()` --calls--> `decode_data_datagram()`  [INFERRED]
  bench/fuse_bench.cpp → src/proto/block.cpp
- `sender_thread()` --calls--> `store`  [INFERRED]
  bench/fuse_bench.cpp → include/fuse/proto/registry.hpp

## Import Cycles
- 1-file cycle: `python/fuse/__init__.py -> python/fuse/__init__.py`

## Communities (146 total, 19 thin omitted)

### Community 0 - "send_lane"
Cohesion: 0.05
Nodes (33): string, vector, Impair, delay_ms, dup_pct, jitter_ms, loss_pct, reorder_pct (+25 more)

### Community 1 - "TEST"
Cohesion: 0.13
Nodes (16): CleanHandshakeCompletesWithIdenticalTables, CompletesOverRealUdpLoopback, DecodeRejectsTooManyStreams, FailsAfterRetryCapWhenDataAlwaysLost, HashMessagesRoundTrip, RecoversFromCorruptedDataViaHashMismatch, RecoversFromLostData, RecoversFromLostFinAck (+8 more)

### Community 2 - "PeerAddr"
Cohesion: 0.21
Nodes (10): DtlsStatus, dtls_io_recv(), dtls_io_send(), DtlsSession::configure(), DtlsSession::handshake(), DtlsSession::recv(), DtlsSession::send(), psk_client_cb() (+2 more)

### Community 3 - "ReceiverStream"
Cohesion: 0.06
Nodes (37): AckReflectsBaseBitmaskAndEchoedSendTime, BlockBeyondWindowIsCountedAsOverflow, DuplicateIsRejected, EchoedSendTimeTracksHighestNotLatestArrival, FillingGapSlidesBaseAndStopsNacks, GapDetectedButNotNackedBeforeReorderDelay, GapNackedAfterReorderDelay, array (+29 more)

### Community 4 - "string"
Cohesion: 0.21
Nodes (9): check(), encode_varint(), error, fuse_socket, fuse_status, string, socket, sock_ (+1 more)

### Community 5 - "SetupPayload"
Cohesion: 0.25
Nodes (12): hash64(), MsgType, decode_setup_hash(), encode_setup_data(), encode_setup_hash(), serialize_setup_payload(), SetupInitiator::on_datagram(), SetupInitiator::on_timeout() (+4 more)

### Community 6 - "TEST"
Cohesion: 0.05
Nodes (43): Crypto, DecodeRejectsInvalidType, DecodeRfc9000Vectors, DeriveInitialSecretsIsDeterministicAndDistinct, EncodeRejectsBufferTooSmall, EncodeRfc9000Vectors, fuse_hash_algorithm, fuse_long_header (+35 more)

### Community 7 - "TEST"
Cohesion: 0.05
Nodes (38): fuse_conn_stats, vector, fuse_conn, ack_pending, asm_buf, asm_end_seq, asm_total, cc (+30 more)

### Community 8 - "Worker"
Cohesion: 0.09
Nodes (14): Worker, add_stream, applied_affinity_, enqueue_retransmit, hint_, owned_stream_ids_, owns_stream, queue_ (+6 more)

### Community 9 - "DtlsSession"
Cohesion: 0.12
Nodes (12): DtlsSession, config_, ctx_, established_, last_wire_, peer_, socket_, ssl_ (+4 more)

### Community 10 - "WorkerOrchestrator"
Cohesion: 0.11
Nodes (18): unique_ptr, vector, Worker, WorkerTask, WorkerOrchestrator, config_, core_load_, last_action_ns_ (+10 more)

### Community 11 - "TEST"
Cohesion: 0.13
Nodes (18): ApplicationDataArrivesIntactEndToEnd, DisabledIsATransparentNoOp, Dtls, MatchingPskCompletesAndPutsCiphertextOnTheWire, MissingPskIsRejectedWhenEncryptionRequired, RequiringEncryptionNeverFallsBackToPlaintext, dtls_available(), string (+10 more)

### Community 12 - "SendQueue"
Cohesion: 0.15
Nodes (7): vector, SendQueue, buffer_, cap_, dropped_, head_, tail_

### Community 13 - "TEST"
Cohesion: 0.15
Nodes (12): CoalesceDropsOldestUnderPressure, Flags, LosslessOneStreamDoesNack, LosslessOneStreamFullyRecoversUnderLoss, LosslessZeroStreamNeverNacks, NonCoalesceBacksUpUnderPressure, OrderedAndUnorderedProduceIdenticalFinalBytes, OrderedStreamSurfacesInSeqOrder (+4 more)

### Community 14 - "connection"
Cohesion: 0.19
Nodes (12): connection, conn_, fuse_connection, fuse_connection_state, fuse_connection, fuse_connection_id, fuse_connection_state, fill_random_cid() (+4 more)

### Community 15 - "SetupInitiator"
Cohesion: 0.13
Nodes (12): SetupInitiator, data_sent_, failed_, last_send_ns_, matched_, max_retries_, my_hash_, on_datagram (+4 more)

### Community 16 - "sender_thread"
Cohesion: 0.18
Nodes (14): string, receiver_thread(), sender_thread(), Workload, bulk_block, bulk_bytes, lanes, telemetry_block (+6 more)

### Community 17 - "Result"
Cohesion: 0.16
Nodes (12): print_row(), Result, bulk_bytes_rx, bulk_bytes_tx, cpu_seconds, name, p50_us, p99_us (+4 more)

### Community 18 - "TEST"
Cohesion: 0.39
Nodes (6): current_thread_affinity(), numa_supported(), prefer_current_thread_numa_node(), spin_until(), TEST(), topology_payload()

### Community 19 - "Ack"
Cohesion: 0.14
Nodes (13): Heartbeat, highest_seq_no, stream_id, Nack, count, missing, stream_id, StreamStart (+5 more)

### Community 20 - "orchestrator.cpp"
Cohesion: 0.19
Nodes (9): least_loaded_core, run_worker, task_, Worker, now_ns(), WorkerOrchestrator::run_worker(), WorkerOrchestrator::start(), WorkerOrchestrator::tick() (+1 more)

### Community 21 - "ReassemblyStream"
Cohesion: 0.14
Nodes (13): kMaxWindow, vector, ReassemblyStream, block_size_, buffer_, delivery_order_, next_expected_, on_block (+5 more)

### Community 22 - "main.rs"
Cohesion: 0.27
Nodes (12): main(), recv_lane(), String, TransportConfig, run_client(), run_server(), send_lane(), transport() (+4 more)

### Community 23 - "encode_ack"
Cohesion: 0.26
Nodes (19): main(), put_u16(), put_u64(), put_u8(), encode_ack(), encode_heartbeat(), encode_nack(), encode_stream_start() (+11 more)

### Community 24 - "TEST"
Cohesion: 0.26
Nodes (15): start, stats, stop, tick, worker_count, TransferStatus, vector, all_done (+7 more)

### Community 25 - "Lane"
Cohesion: 0.17
Nodes (12): atomic, vector, Lane, bulk_blocks, bulk_bytes_rx, done, latencies, rx (+4 more)

### Community 26 - "TEST"
Cohesion: 0.17
Nodes (10): Block, DecodeRejectsWrongVersion, now_ns(), PeekMsgTypeDispatches, RejectsOverlargePayload, RejectsTooSmallBuffer, DataDatagramRoundTrip, DecodeRejectsTruncated (+2 more)

### Community 27 - "vector"
Cohesion: 0.33
Nodes (6): atomic, thread_, Setup, spin_until(), TEST(), topology()

### Community 28 - "aux.cpp"
Cohesion: 0.25
Nodes (20): main(), get_u16(), get_u64(), get_u8(), check_outer(), decode_ack(), decode_heartbeat(), decode_nack() (+12 more)

### Community 29 - "worker.cpp"
Cohesion: 0.14
Nodes (10): RetransmitRequest, seq_no, stream_id, drain_queue, run, now_ns(), Worker::enqueue_retransmit(), Worker::run() (+2 more)

### Community 30 - "TEST"
Cohesion: 0.20
Nodes (14): AckRoundTrip, Aux, DecodeRejectsWrongType, DigestMessageRoundTrip, EmptyNack, FullWindowNackFits, HeartbeatRoundTrip, NackRoundTrip (+6 more)

### Community 31 - "TEST"
Cohesion: 0.06
Nodes (32): 1.1 Block header, 1.2 Sender-side registry, 1.3 Receiver-side tracking, 1.4 Missing-block detection (O(1)), 1.5 Aux channel (separate flow or distinguished by msg_type), 1.6 Retransmission, 2.1 SETUP payload, 2.2 Three-way handshake (+24 more)

### Community 32 - "TEST"
Cohesion: 0.07
Nodes (29): ConfirmInvalidatesSlot, vector, RegistrySlot, flags, offset, payload, payload_len, send_time_ns (+21 more)

### Community 33 - "test_orchestrator.cpp"
Cohesion: 0.15
Nodes (16): assigned_cores, Orchestrator, Pred, ReportsUtilizationAndActionCounts, ScalesInWhenIdleDownToMin, ScalesOutUnderLoadUpToMax, StabilizationWindowLimitsScalingRate, StartsAtMinimumAndStopsCleanly (+8 more)

### Community 34 - "SenderRegistry"
Cohesion: 0.07
Nodes (29): HugeStreamStartSizeFailsCleanlyInsteadOfCrashing, InconsistentStreamStartIsRejected, LossyLinkRecoversThroughRetransmitThread, ManyLanesSplitAndReassemble, MissingReceiverFailsAfterConnectTimeout, PayloadSmallerThanOneBlock, PeerAbortEndsReceiverPromptly, ProgressIsReportedOnBothSides (+21 more)

### Community 35 - "SpscQueue"
Cohesion: 0.28
Nodes (6): SpscQueue, buffer_, cap_, head_, tail_, T

### Community 36 - "TEST"
Cohesion: 0.27
Nodes (9): DropNackRetransmitRoundTrip, NoHeapAllocationInSteadyState, size_t, Stage1Loopback, make_payload(), operator delete(), operator new(), set_recv_timeout() (+1 more)

### Community 37 - "fuse_bench.cpp"
Cohesion: 0.44
Nodes (8): cpu_seconds_used(), main(), now_ns(), peak_rss_kb(), print_header(), run_config(), set_recv_timeout(), set_sock_buffers()

### Community 38 - "main"
Cohesion: 0.06
Nodes (48): bulk_recv(), bulk_send(), atomic, string, vector, main(), now_ns(), pct() (+40 more)

### Community 39 - "OrchestratorConfig"
Cohesion: 0.22
Nodes (9): OrchestratorConfig, max_workers, min_workers, pin_to_core, scale_in_utilization, stabilization_ns, target_utilization, WorkerTask (+1 more)

### Community 40 - "WorkerPool"
Cohesion: 0.17
Nodes (9): StreamRouter, count_, entries_, WorkerPool, route_nack, router_, start, stop (+1 more)

### Community 41 - "StreamConfig"
Cohesion: 0.13
Nodes (12): SetupPayload, num_streams, num_workers, protocol_version, streams, StreamConfig, block_size, stream_flags (+4 more)

### Community 42 - "run_file_benchmark.sh"
Cohesion: 0.38
Nodes (3): run_fuse(), run_quic(), run_file_benchmark.sh script

### Community 43 - "registry.cpp"
Cohesion: 0.08
Nodes (27): AcceptTimesOutWithNoClient, BidirectionalTraffic, CloseNotifiesPeer, CloseWhileRecvBlockedUnblocksCleanly, ConcurrentSendsDoNotCorruptMessages, ConnectAcceptAndRoundTrip, EmptyMessageIsDelivered, LargeMessageSpansManyBlocks (+19 more)

### Community 44 - "TEST"
Cohesion: 0.33
Nodes (5): HeaderSizesMatchSpec, TEST(), U16BigEndianRoundTrip, U64BigEndianRoundTrip, U64Extremes

### Community 45 - "Wire"
Cohesion: 0.36
Nodes (3): vector, array, Wire

### Community 46 - "BlockHeader"
Cohesion: 0.29
Nodes (6): BlockHeader, flags, offset, payload_len, seq_no, stream_id

### Community 47 - "OrchestratorStats"
Cohesion: 0.33
Nodes (6): OrchestratorStats, last_utilization, scale_ins, scale_outs, ticks, WorkerOrchestrator::stats()

### Community 48 - "RegistrySlot"
Cohesion: 0.08
Nodes (28): DoneRing, PacketRing, Channel, in, lane, out, cipher_for(), consumer_main() (+20 more)

### Community 49 - "TopologyHint"
Cohesion: 0.33
Nodes (4): TopologyHint, cpu_core, numa_node, WorkerPool::start()

### Community 50 - "reassembly.cpp"
Cohesion: 0.33
Nodes (5): deliver, write_sink, ReassemblyStream::deliver(), ReassemblyStream::on_block(), ReassemblyStream::ReassemblyStream()

### Community 51 - "RetransmitRequest"
Cohesion: 0.09
Nodes (23): AadBindsBlockToItsPosition, Available, DifferentSaltYieldsDifferentKey, FailsClosedWithoutBackend, LanesShareKeyButNotNonceSpace, RejectsTamperedCiphertext, RejectsTamperedTag, SameInputsDeriveSameKeyOnBothEnds (+15 more)

### Community 53 - "FileSink"
Cohesion: 0.10
Nodes (15): End, FileSink, chunk_, counters_, existed_, fd_, file_id_, kFooterLen (+7 more)

### Community 54 - "SdkTest"
Cohesion: 0.13
Nodes (7): _import_fuse(), Behavioural tests for the Python bindings.  Run against an installed package:  p, Prefer an installed package; fall back to the source tree beside us.      Order, Accept `count` connections and stash them for the test to use., A connected (client, server) pair on a fresh port., SdkTest, _serve()

### Community 55 - "SpscRing"
Cohesion: 0.09
Nodes (15): BatchedWriteAndRead, CapacityRoundsUpToPowerOfTwo, FullAndEmptyAreReportedNotBlocked, atomic, unique_ptr, SpscRing, cap_, head_ (+7 more)

### Community 56 - "send_lane"
Cohesion: 0.14
Nodes (23): build_aad(), atomic, string, vector, LaneStats, batches, bytes, end_ns (+15 more)

### Community 57 - "fuse_listener"
Cohesion: 0.10
Nodes (22): main(), fuse_config, Seen, string, fuse_config_init(), fuse_encryption_available(), fuse_listen(), fuse_listener (+14 more)

### Community 58 - "ResumeRanges"
Cohesion: 0.08
Nodes (23): DigestMessage, digest, nonce, stream_id, Heartbeat, highest_seq_no, stream_id, Nack (+15 more)

### Community 59 - "SenderStreamState"
Cohesion: 0.09
Nodes (21): SenderStreamState, block_size, cc, done, last_progress_ns, last_real_progress_ns, last_rtt_echo_ns, last_start_send_ns (+13 more)

### Community 60 - "MuxTransferConfig"
Cohesion: 0.10
Nodes (19): string, MuxTransferConfig, bind_address, block_size, host, max_workers, min_workers, num_streams (+11 more)

### Community 61 - "TransferStats"
Cohesion: 0.16
Nodes (20): TransferStats, auth_failures, bytes, final_block_size, resumed_bytes, retransmits, seconds, check_receive_config() (+12 more)

### Community 62 - "ReceiverStreamState"
Cohesion: 0.10
Nodes (20): atomic, ReceiverStreamState, delivered, done, final_seq, have_start, last_ack_delivered, last_ack_ns (+12 more)

### Community 63 - "transfer.cpp"
Cohesion: 0.19
Nodes (19): send_to, cancel_requested(), coarsen_ranges(), atomic, encode_resume_answer(), Keys, enabled, key (+11 more)

### Community 64 - "sdk.cpp"
Cohesion: 0.24
Nodes (18): condition_variable, deque, build_aad(), decode_hello(), decode_hello_ack(), encode_hello(), encode_hello_ack(), fuse_accept() (+10 more)

### Community 65 - "UdpSocket"
Cohesion: 0.12
Nodes (15): UdpSocket, batch_scratch_, gso_failed_, local_port, open, recv_batch, send_segmented, set_nonblocking (+7 more)

### Community 66 - "PeerAddr"
Cohesion: 0.14
Nodes (13): sockaddr_storage, socklen_t, PeerAddr, addr, len, operator==, UdpSocket::recv_batch(), UdpSocket::send_segmented() (+5 more)

### Community 67 - "TransferConfig"
Cohesion: 0.11
Nodes (18): atomic, function, string, TransferConfig, base_port, bind_address, block_size, cancel (+10 more)

### Community 68 - "digest.cpp"
Cohesion: 0.18
Nodes (10): FileDigest, finish, fnv_, state_, update, function, file_digest(), segmented() (+2 more)

### Community 69 - "MuxSender"
Cohesion: 0.12
Nodes (17): MuxSender, all_done, cursor_, data_, dst_, end_ns_, init_, len_ (+9 more)

### Community 70 - "Fuse benchmark results — consolidated"
Cohesion: 0.12
Nodes (16): 10. Correctness, 1. File transfer throughput — unencrypted, 2. File transfer throughput — encrypted, like-for-like, 3. Latency under load, 4. Goodput vs wire bytes, 4a. Behaviour under real network conditions, 4a-fix. Retransmission timeout: a bug this testing found and fixed, 4b. Why throughput does not scale linearly with lanes (+8 more)

### Community 71 - "ConsumerState"
Cohesion: 0.12
Nodes (11): LaneCipher, aes_, initialized_, open, seal, ConsumerState, ciphers, iov (+3 more)

### Community 72 - "SetupResponder"
Cohesion: 0.17
Nodes (13): SetupResponder, complete_, config_, failed_, last_send_ns_, max_retries_, my_hash_, on_datagram (+5 more)

### Community 73 - "Connection"
Cohesion: 0.15
Nodes (8): Connection, Seconds (or None for 'wait forever') to the C API's millisecond int., An established connection. Create one with :func:`connect` or     :meth:`Listene, Receive one whole message.          ``timeout`` is in seconds; ``None`` waits in, Iterate messages until the peer closes::          for message in conn:, Wait for a client and return its :class:`Connection`.          Each accepted con, Iterate incoming connections::          for conn in listener:             thread, _timeout_ms()

### Community 74 - "MuxReceiver"
Cohesion: 0.12
Nodes (16): unique_ptr, MuxReceiver, cursor_, end_ns_, have_peer_, mux_stats_, out_, peer_ (+8 more)

### Community 75 - "Fuse SDK"
Cohesion: 0.13
Nodes (15): API, C, C++, or everything at once, Choosing between the two APIs, Client, Encryption, Fuse SDK, Install, Ports and firewalls (+7 more)

### Community 76 - "wait_for"
Cohesion: 0.22
Nodes (13): main(), serve(), Sdk, function, fuse_status, encode_close(), fuse_close(), fuse_recv() (+5 more)

### Community 77 - "udp_common.cpp"
Cohesion: 0.17
Nodes (12): peer, sockaddr_storage, socklen_t, parse_sockaddr(), peer_port(), peer_to_string(), PeerAddr::operator==(), UdpSocket::local_port() (+4 more)

### Community 78 - "quickstart_common.hpp"
Cohesion: 0.19
Nodes (7): attach(), exit_code_for(), TransferStatus, install_signal_handlers(), main(), main(), Transfer

### Community 79 - "Udp"
Cohesion: 0.18
Nodes (12): now_ns(), Ipv4LoopbackRoundTrip, Ipv6LoopbackRoundTrip, PeerAddrEqualityIsFamilyAware, PeerToStringAndPortRoundTripIpv4, PeerToStringAndPortRoundTripIpv6, ResolveRejectsGarbageAddress, string (+4 more)

### Community 80 - "FuseError"
Cohesion: 0.14
Nodes (11): Exception, AuthError, ConnectionClosed, FuseError, MessageTooLarge, Base class for every Fuse failure., The operation did not complete within its timeout., The peer closed the connection. (+3 more)

### Community 81 - "__init__.py"
Cohesion: 0.20
Nodes (13): _build_config(), ConfigError, connect(), encryption_available(), listen(), max_message(), Fuse — a reliable message transport over UDP, with a socket-style API.  If you h, True if this build can encrypt. When False, passing ``key=`` raises. (+5 more)

### Community 82 - "mux_transfer.cpp"
Cohesion: 0.32
Nodes (13): build_streams, dispatch_incoming, run_setup_handshake, try_service_one, worker_task, build_streams, dispatch_incoming, run_setup_handshake (+5 more)

### Community 83 - "Using Fuse in your project"
Cohesion: 0.15
Nodes (13): 10. Going lower-level, 1. Install, 2. Use it — CMake, 3. Use it — pkg-config (no CMake), 4. Send something, 5. Encryption, 6. Ports and firewalls, 7. Choosing `lanes` (+5 more)

### Community 84 - "test_transfer.cpp"
Cohesion: 0.33
Nodes (12): path_, string, vector, interrupt_midway(), make_payload(), next_port(), read_all(), receiver_config() (+4 more)

### Community 85 - "Benchmarks"
Cohesion: 0.17
Nodes (12): Benchmarks, Building, Encrypted, like-for-like, Goodput vs wire bytes, Latency and goodput, Latency caveats, Loss recovery, Results (+4 more)

### Community 86 - "socket.c"
Cohesion: 0.33
Nodes (9): main(), fuse_socket, fuse_status, fuse_socket_close(), fuse_socket_fd(), fuse_socket_open(), fuse_socket_recv_from(), fuse_socket_send_to() (+1 more)

### Community 87 - "quic_common.py"
Cohesion: 0.23
Nodes (8): main(), serve(), finish_progress_line(), make_progress_printer(), Shared by quic_sender.py/quic_receiver.py: a progress line matching the fuse qui, main(), make_self_signed_cert(), main()

### Community 88 - "Ack"
Cohesion: 0.17
Nodes (12): Ack, base_seq_no, echoed_send_time, received_bitmask, stream_id, Ack, base_seq_no, echoed_send_time (+4 more)

### Community 89 - "plot-benchmark.py"
Cohesion: 0.41
Nodes (11): fig_boxplot(), fig_cdf(), fig_histogram(), fig_retransmits_hist(), fig_retx_vs_throughput(), fig_summary_bar(), fig_timeseries(), load() (+3 more)

### Community 90 - "LaneResult"
Cohesion: 0.17
Nodes (12): LaneResult, auth_failures, bytes, end_ns, expected, final_block, finished, retransmits (+4 more)

### Community 91 - "Outcome"
Cohesion: 0.17
Nodes (12): TransferStatus, Outcome, received, recv_stats, recv_status, send_stats, send_status, Run (+4 more)

### Community 92 - "vector"
Cohesion: 0.24
Nodes (7): ResumeRange, length, offset, vector, MemorySink, out_, wait_for_lanes()

### Community 93 - "Listener"
Cohesion: 0.22
Nodes (3): Listener, Tell the peer we are done and release the connection. Idempotent., A bound server socket. Create one with :func:`listen`.

### Community 94 - "LossyRelay"
Cohesion: 0.22
Nodes (8): atomic, LossyRelay, back_, drop_every_, front_, receiver_, stop_, thread_

### Community 95 - "_check"
Cohesion: 0.24
Nodes (6): Data, _as_bytes(), _check(), Send one message. Returns once the peer has acknowledged it.          ``str`` is, ``(address, port)`` of the far end., The port actually bound — useful when you asked for port 0.

### Community 96 - "DtlsConfig"
Cohesion: 0.20
Nodes (9): DtlsRole, DtlsConfig, encryption_required, handshake_timeout_init_sec, handshake_timeout_max_sec, psk_identity, psk_key, role (+1 more)

### Community 97 - "StreamStart"
Cohesion: 0.20
Nodes (10): StreamStart, block_size, file_id, file_total_bytes, nonce, session_salt, stream_base_offset, stream_id (+2 more)

### Community 98 - "TransferProgress"
Cohesion: 0.20
Nodes (7): TransferProgress, bytes_done, bytes_total, lanes_done, lanes_total, retransmits, seconds

### Community 99 - "fuse-transport"
Cohesion: 0.20
Nodes (9): Also in this project, API, Checking an install, Client, Encryption, fuse-transport, License, Server (+1 more)

### Community 100 - "fuse"
Cohesion: 0.20
Nodes (10): A note on licensing, Benchmarks, Bringing your own wolfSSL, Building, Examples, fuse, Install, Installing a package instead of building from source (+2 more)

### Community 101 - "net-sim: two-container bridge harness"
Cohesion: 0.20
Nodes (9): AI assistance, Benchmarking, How it's built, Known ceiling, N=100 results (SIZE_MB=2048, LANES=6), net-sim: two-container bridge harness, QUIC/HTTP3 comparison, Use (+1 more)

### Community 102 - "Sink"
Cohesion: 0.20
Nodes (4): Sink, finish, prepare, write

### Community 103 - "TEST"
Cohesion: 0.31
Nodes (8): AnyChangeChangesTheDigest, Digest, MemoryAndFileAgree, ReadErrorIsReported, bytes(), vector, temp_fd(), TEST()

### Community 104 - "quic_latbench.rs"
Cohesion: 0.42
Nodes (7): main(), now_ns(), String, TransportConfig, run_client(), run_server(), transport()

### Community 105 - "close"
Cohesion: 0.22
Nodes (8): close, BatchScratch, UdpSocket::recv_batch(), UdpSocket::send_segmented(), UdpSocket::~UdpSocket(), UdpSocket::~UdpSocket(), UdpSocket::open(), UdpSocket::~UdpSocket()

### Community 106 - "Injector"
Cohesion: 0.22
Nodes (8): Inject, MsgType, vector, Injector, mode, nth, seen, target

### Community 107 - "config.sh"
Cohesion: 0.25
Nodes (5): config.sh script, run_vm(), run-test.sh script, setup-net.sh script, teardown.sh script

### Community 108 - "Roadmap / status"
Cohesion: 0.25
Nodes (8): Explicit non-goals (do not implement without revisiting), Gaps / not yet done, Head-to-head vs QUIC (file transfer), Implemented, Key design points, Latency under load — the design premise, finally tested, Measured results, Roadmap / status

### Community 109 - "Completion"
Cohesion: 0.25
Nodes (8): Completion, bytes, flags, result, send_time, seq, src, WriteResult

### Community 110 - "run_network_matrix.sh"
Cohesion: 0.43
Nodes (5): row(), run_fuse(), run_quic(), run_network_matrix.sh script, step_ports()

### Community 111 - "BatchScratch"
Cohesion: 0.29
Nodes (7): mmsghdr, BatchScratch, addrs, iovs, msgs, iovec, sockaddr_storage

### Community 112 - "install.sh"
Cohesion: 0.57
Nodes (6): die(), fetch(), say(), install.sh script, step(), warn()

### Community 114 - "C"
Cohesion: 0.33
Nodes (6): C, Client, Receiving without guessing a size, Server, Status codes, Timeouts

### Community 115 - "_native.py"
Cohesion: 0.40
Nodes (4): _candidates(), ConnStats, _load(), ctypes binding to libfuse_proto's C ABI (`fuse/sdk.h`).  Kept separate from the

### Community 116 - "net-sim: two-VM bridge harness"
Cohesion: 0.33
Nodes (5): How it's built, Known ceiling, net-sim: two-VM bridge harness, Podman: run it as a pod instead, Use

### Community 117 - "PacketSlot"
Cohesion: 0.33
Nodes (6): PacketSlot, dg, lane_base, lane_total, len, src

### Community 118 - "HandshakeResult"
Cohesion: 0.33
Nodes (6): HandshakeResult, init_failed, init_matched, resp_complete, resp_failed, rounds

### Community 119 - "__main__.py"
Cohesion: 0.70
Nodes (4): _info(), main(), ``python -m fuse`` — check that an installed Fuse works on this machine.  A whee, _selftest()

### Community 120 - ".stats"
Cohesion: 0.50
Nodes (3): NamedTuple, Counters for this connection, useful for logging and debugging., Stats

### Community 121 - "bench-loop.sh"
Cohesion: 0.83
Nodes (3): fuse_iter(), quic_iter(), bench-loop.sh script

### Community 122 - "entrypoint.sh"
Cohesion: 0.50
Nodes (3): LD_LIBRARY_PATH, PATH, entrypoint.sh script

### Community 123 - "guest-init.sh"
Cohesion: 0.50
Nodes (3): LD_LIBRARY_PATH, PATH, guest-init.sh script

### Community 126 - "TxMeta"
Cohesion: 0.67
Nodes (3): TxMeta, flags, offset

## Knowledge Gaps
- **636 isolated node(s):** `bulk_bytes`, `bulk_block`, `telemetry_msgs`, `telemetry_block`, `lanes` (+631 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **19 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `SenderStreamState` connect `SenderStreamState` to `send_lane`, `TEST`, `MuxSender`, `WorkerOrchestrator`, `SendQueue`, `mux_transfer.cpp`, `ReceiverStreamState`?**
  _High betweenness centrality (0.047) - this node is a cross-community bridge._
- **Why does `fuse_conn` connect `TEST` to `sdk.cpp`, `send_lane`, `TEST`, `ReceiverStream`, `PeerAddr`, `UdpSocket`, `ConsumerState`, `WorkerOrchestrator`, `registry.cpp`, `wait_for`, `udp_common.cpp`, `LossyRelay`, `atomic`, `TxMeta`?**
  _High betweenness centrality (0.044) - this node is a cross-community bridge._
- **Why does `MuxSender` connect `MuxSender` to `UdpSocket`, `PeerAddr`, `WorkerOrchestrator`, `MuxReceiver`, `SetupInitiator`, `mux_transfer.cpp`, `TEST`, `SenderStreamState`, `MuxTransferConfig`, `TransferStats`, `ReceiverStreamState`?**
  _High betweenness centrality (0.039) - this node is a cross-community bridge._
- **Are the 5 inferred relationships involving `TEST()` (e.g. with `PeerAddr` and `send_to`) actually correct?**
  _`TEST()` has 5 INFERRED edges - model-reasoned connections that need verification._
- **Are the 18 inferred relationships involving `TEST()` (e.g. with `send_to` and `encode_data_datagram()`) actually correct?**
  _`TEST()` has 18 INFERRED edges - model-reasoned connections that need verification._
- **Are the 22 inferred relationships involving `send_lane()` (e.g. with `.on_loss()` and `.on_rtt_sample()`) actually correct?**
  _`send_lane()` has 22 INFERRED edges - model-reasoned connections that need verification._
- **What connects `bulk_bytes`, `bulk_block`, `telemetry_msgs` to the rest of the system?**
  _666 weakly-connected nodes found - possible documentation gaps or missing edges._