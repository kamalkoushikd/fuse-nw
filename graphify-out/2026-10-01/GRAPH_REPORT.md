# Graph Report - .  (2026-07-21)

## Corpus Check
- cluster-only mode — file stats not available

## Summary
- 805 nodes · 1311 edges · 53 communities (52 shown, 1 thin omitted)
- Extraction: 84% EXTRACTED · 16% INFERRED · 0% AMBIGUOUS · INFERRED: 213 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `2cb0a0e0`
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

## God Nodes (most connected - your core abstractions)
1. `Worker` - 40 edges
2. `WorkerOrchestrator` - 31 edges
3. `TEST()` - 26 edges
4. `DtlsSession` - 23 edges
5. `TEST()` - 23 edges
6. `send_lane()` - 21 edges
7. `ReceiverStream` - 20 edges
8. `SetupPayload` - 20 edges
9. `UdpSocket` - 20 edges
10. `TEST()` - 18 edges

## Surprising Connections (you probably didn't know these)
- `TEST()` --calls--> `on_block`  [INFERRED]
  tests/proto/test_flags.cpp → include/fuse/proto/reassembly.hpp
- `TEST()` --calls--> `delivery_order_`  [INFERRED]
  tests/proto/test_flags.cpp → include/fuse/proto/reassembly.hpp
- `TEST()` --calls--> `confirm`  [INFERRED]
  tests/proto/test_registry.cpp → include/fuse/proto/registry.hpp
- `TEST()` --calls--> `valid_count`  [INFERRED]
  tests/proto/test_registry.cpp → include/fuse/proto/registry.hpp
- `receiver_thread()` --calls--> `recv`  [INFERRED]
  bench/fuse_bench.cpp → include/fuse/proto/dtls.hpp

## Import Cycles
- None detected.

## Communities (53 total, 1 thin omitted)

### Community 0 - "send_lane"
Cohesion: 0.05
Nodes (43): atomic, string, vector, LaneStats, batches, bytes, end_ns, final_block (+35 more)

### Community 1 - "TEST"
Cohesion: 0.06
Nodes (42): CleanHandshakeCompletesWithIdenticalTables, CompletesOverRealUdpLoopback, DecodeRejectsTooManyStreams, FailsAfterRetryCapWhenDataAlwaysLost, HashMessagesRoundTrip, SetupResponder, complete_, config_ (+34 more)

### Community 2 - "PeerAddr"
Cohesion: 0.06
Nodes (32): DtlsRole, DtlsStatus, DtlsConfig, encryption_required, handshake_timeout_init_sec, handshake_timeout_max_sec, psk_identity, psk_key (+24 more)

### Community 3 - "ReceiverStream"
Cohesion: 0.06
Nodes (30): AckReflectsBaseBitmaskAndEchoedSendTime, BlockBeyondWindowIsCountedAsOverflow, DuplicateIsRejected, EchoedSendTimeTracksHighestNotLatestArrival, FillingGapSlidesBaseAndStopsNacks, GapDetectedButNotNackedBeforeReorderDelay, GapNackedAfterReorderDelay, array (+22 more)

### Community 4 - "string"
Cohesion: 0.12
Nodes (20): main(), check(), decode_varint(), encode_varint(), error, fuse_socket, fuse_status, string (+12 more)

### Community 5 - "SetupPayload"
Cohesion: 0.10
Nodes (24): Entry, hash64(), StreamRouter, count_, entries_, SetupPayload, num_streams, num_workers (+16 more)

### Community 6 - "TEST"
Cohesion: 0.10
Nodes (24): DecodeRejectsInvalidType, DecodeRfc9000Vectors, EncodeRejectsBufferTooSmall, EncodeRfc9000Vectors, fuse_long_header, initializer_list, LongHeader, RejectsBufferTooSmall (+16 more)

### Community 7 - "TEST"
Cohesion: 0.13
Nodes (17): Crypto, DeriveInitialSecretsIsDeterministicAndDistinct, fuse_hash_algorithm, MatchesRfc9001InitialSecretVectors, RejectsWrongOutputLength, ReportsUnavailableWhenBuiltWithoutCrypto, fuse_connection_id, fuse_status (+9 more)

### Community 8 - "Worker"
Cohesion: 0.08
Nodes (17): id, atomic, Worker, add_stream, applied_affinity_, enqueue_retransmit, hint_, owned_stream_ids_ (+9 more)

### Community 9 - "DtlsSession"
Cohesion: 0.11
Nodes (12): DtlsSession, config_, ctx_, established_, last_wire_, peer_, socket_, ssl_ (+4 more)

### Community 10 - "WorkerOrchestrator"
Cohesion: 0.11
Nodes (17): unique_ptr, vector, WorkerTask, WorkerOrchestrator, config_, core_load_, last_action_ns_, last_util_milli_ (+9 more)

### Community 11 - "TEST"
Cohesion: 0.17
Nodes (16): DisabledIsATransparentNoOp, Dtls, MatchingPskCompletesAndPutsCiphertextOnTheWire, MissingPskIsRejectedWhenEncryptionRequired, RequiringEncryptionNeverFallsBackToPlaintext, dtls_available(), string, HandshakeOutcome (+8 more)

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
Cohesion: 0.20
Nodes (12): HintsPinEachWorkerToItsCore, NoHintIsANoOp, NumaHookDegradesGracefullyWhenUnavailable, PinCurrentThreadRestrictsAffinityToOneCore, vector, current_thread_affinity(), numa_supported(), Pred (+4 more)

### Community 19 - "Ack"
Cohesion: 0.14
Nodes (13): Ack, base_seq_no, echoed_send_time, received_bitmask, stream_id, Heartbeat, highest_seq_no, stream_id (+5 more)

### Community 20 - "orchestrator.cpp"
Cohesion: 0.19
Nodes (10): least_loaded_core, run_worker, task_, vector, now_ns(), WorkerOrchestrator::assigned_cores(), WorkerOrchestrator::run_worker(), WorkerOrchestrator::start() (+2 more)

### Community 21 - "ReassemblyStream"
Cohesion: 0.14
Nodes (12): kMaxWindow, vector, ReassemblyStream, block_size_, buffer_, delivery_order_, next_expected_, on_block (+4 more)

### Community 22 - "main.rs"
Cohesion: 0.27
Nodes (12): main(), recv_lane(), String, run_client(), run_server(), send_lane(), transport(), CertificateDer (+4 more)

### Community 23 - "encode_ack"
Cohesion: 0.23
Nodes (13): Nack, count, missing, stream_id, put_u16(), put_u64(), MsgType, encode_ack() (+5 more)

### Community 24 - "TEST"
Cohesion: 0.15
Nodes (13): assigned_cores, start, stats, stop, worker_count, PlacesWorkersOnDistinctLeastLoadedCores, ReportsUtilizationAndActionCounts, ScalesInWhenIdleDownToMin (+5 more)

### Community 25 - "Lane"
Cohesion: 0.17
Nodes (12): atomic, vector, Lane, bulk_blocks, bulk_bytes_rx, done, latencies, rx (+4 more)

### Community 26 - "TEST"
Cohesion: 0.20
Nodes (10): Block, DecodeRejectsWrongVersion, PeekMsgTypeDispatches, RejectsOverlargePayload, RejectsTooSmallBuffer, encode_data_datagram(), DataDatagramRoundTrip, DecodeRejectsTruncated (+2 more)

### Community 27 - "vector"
Cohesion: 0.30
Nodes (5): vector, atomic, thread_, Setup, topology()

### Community 28 - "aux.cpp"
Cohesion: 0.38
Nodes (10): get_u16(), get_u64(), get_u8(), check_outer(), decode_ack(), decode_heartbeat(), decode_nack(), decode_stream_start() (+2 more)

### Community 29 - "worker.cpp"
Cohesion: 0.18
Nodes (7): drain_queue, run, prefer_current_thread_numa_node(), now_ns(), Worker::run(), Worker::start(), WorkerPool::WorkerPool()

### Community 30 - "TEST"
Cohesion: 0.20
Nodes (10): AckRoundTrip, Aux, DecodeRejectsWrongType, EmptyNack, FullWindowNackFits, HeartbeatRoundTrip, NackRoundTrip, StreamStartDoesNotDecodeAsOtherTypes (+2 more)

### Community 31 - "TEST"
Cohesion: 0.18
Nodes (11): ConcurrentProducerConsumer, EachWorkerOnDistinctThreadOwningItsSetupStreams, MapsStreamsToOwningWorkers, NackForUnknownStreamRoutesNowhere, NackRoutedOnlyToOwningWorker, PushPopFifo, ReportsFull, Spsc (+3 more)

### Community 32 - "TEST"
Cohesion: 0.20
Nodes (9): ConfirmInvalidatesSlot, store, LookupMissingSeqReturnsNull, OverwriteByWindowAdvanceLosesOldSeq, Registry, StoreAndLookup, TEST(), ValidCountTracksLiveSlots (+1 more)

### Community 33 - "test_orchestrator.cpp"
Cohesion: 0.22
Nodes (9): tick, Orchestrator, atomic, Pred, fast_config(), now_ns(), SyntheticLoad, busy (+1 more)

### Community 34 - "SenderRegistry"
Cohesion: 0.18
Nodes (10): array, kMaxWindow, SenderRegistry, confirm, lookup, slots_, valid_count, registry_for (+2 more)

### Community 35 - "SpscQueue"
Cohesion: 0.22
Nodes (8): atomic, vector, SpscQueue, buffer_, cap_, head_, tail_, T

### Community 36 - "TEST"
Cohesion: 0.27
Nodes (9): DropNackRetransmitRoundTrip, NoHeapAllocationInSteadyState, size_t, Stage1Loopback, make_payload(), operator delete(), operator new(), set_recv_timeout() (+1 more)

### Community 37 - "fuse_bench.cpp"
Cohesion: 0.44
Nodes (8): cpu_seconds_used(), main(), now_ns(), peak_rss_kb(), print_header(), run_config(), set_recv_timeout(), set_sock_buffers()

### Community 38 - "main"
Cohesion: 0.31
Nodes (7): main(), now_ns(), main(), now_ns(), recv_from, resolve, set_nonblocking

### Community 39 - "OrchestratorConfig"
Cohesion: 0.22
Nodes (9): OrchestratorConfig, max_workers, min_workers, pin_to_core, scale_in_utilization, stabilization_ns, target_utilization, WorkerTask (+1 more)

### Community 40 - "WorkerPool"
Cohesion: 0.22
Nodes (8): unique_ptr, vector, WorkerPool, route_nack, router_, start, stop, workers_

### Community 41 - "StreamConfig"
Cohesion: 0.25
Nodes (7): StreamConfig, block_size, stream_flags, stream_id, window_size, worker_id, Worker::add_stream()

### Community 42 - "run_file_benchmark.sh"
Cohesion: 0.38
Nodes (3): run_fuse(), run_quic(), run_file_benchmark.sh script

### Community 43 - "registry.cpp"
Cohesion: 0.38
Nodes (3): SenderRegistry::confirm(), SenderRegistry::lookup(), SenderRegistry::store()

### Community 44 - "TEST"
Cohesion: 0.33
Nodes (5): HeaderSizesMatchSpec, TEST(), U16BigEndianRoundTrip, U64BigEndianRoundTrip, U64Extremes

### Community 46 - "BlockHeader"
Cohesion: 0.33
Nodes (6): BlockHeader, flags, offset, payload_len, seq_no, stream_id

### Community 47 - "OrchestratorStats"
Cohesion: 0.33
Nodes (6): OrchestratorStats, last_utilization, scale_ins, scale_outs, ticks, WorkerOrchestrator::stats()

### Community 48 - "RegistrySlot"
Cohesion: 0.33
Nodes (6): RegistrySlot, payload, payload_len, send_time_ns, seq_no, valid

### Community 49 - "TopologyHint"
Cohesion: 0.33
Nodes (5): TopologyHint, cpu_core, numa_node, vector, WorkerPool::start()

### Community 50 - "reassembly.cpp"
Cohesion: 0.40
Nodes (3): deliver, ReassemblyStream::on_block(), ReassemblyStream::ReassemblyStream()

### Community 51 - "RetransmitRequest"
Cohesion: 0.50
Nodes (4): RetransmitRequest, seq_no, stream_id, Worker::enqueue_retransmit()

## Knowledge Gaps
- **195 isolated node(s):** `bulk_bytes`, `bulk_block`, `telemetry_msgs`, `telemetry_block`, `lanes` (+190 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **1 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Worker` connect `Worker` to `SenderRegistry`, `SpscQueue`, `WorkerPool`, `WorkerOrchestrator`, `TopologyHint`, `TEST`, `RetransmitRequest`, `orchestrator.cpp`, `vector`, `worker.cpp`?**
  _High betweenness centrality (0.130) - this node is a cross-community bridge._
- **Why does `WorkerOrchestrator` connect `WorkerOrchestrator` to `test_orchestrator.cpp`, `OrchestratorConfig`, `Worker`, `orchestrator.cpp`, `TEST`, `vector`?**
  _High betweenness centrality (0.073) - this node is a cross-community bridge._
- **Why does `send_lane()` connect `send_lane` to `SetupPayload`, `main`, `encode_ack`, `TEST`, `aux.cpp`?**
  _High betweenness centrality (0.070) - this node is a cross-community bridge._
- **Are the 8 inferred relationships involving `TEST()` (e.g. with `config_` and `.has_config()`) actually correct?**
  _`TEST()` has 8 INFERRED edges - model-reasoned connections that need verification._
- **Are the 11 inferred relationships involving `TEST()` (e.g. with `hash64()` and `.delivered_count()`) actually correct?**
  _`TEST()` has 11 INFERRED edges - model-reasoned connections that need verification._
- **What connects `bulk_bytes`, `bulk_block`, `telemetry_msgs` to the rest of the system?**
  _195 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `send_lane` be split into smaller, more focused modules?**
  _Cohesion score 0.05200501253132832 - nodes in this community are weakly interconnected._