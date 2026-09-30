# net-sim: two-container bridge harness

Runs a real fuse transfer between two containers joined by a podman bridge
network instead of loopback. That gets you a real veth/IP kernel path and
1500 MTU (loopback lets fuse adapt into a 16 KiB block a real link never
would, per the top-level README's caveats) while staying host-local - a
bridge between two containers has no physical NIC in the loop, so it's still
fast.

This used to boot a full Fedora VM per side under QEMU/KVM just to get that
real network path. A container bridge gives the same real kernel IP path and
1500 MTU for free - two containers on a bridge network already are two
separate network namespaces joined by a veth/bridge pair, which is exactly
what the VMs were emulating with tap devices. No kernel boot, no dracut
initramfs, no `dnf --installroot` rootfs. The old VM harness is kept under
`legacy/` for reference; see `legacy/README.md` for how it worked.

Linux only (podman, bridge networks). Needs root once per run for the actual
network/pod bring-up - see "Why root" below.

## Use

```sh
cmake --build --preset default          # build fuse_quickstart_send/recv first
scripts/net-sim/run.sh                  # stage binaries, build image, run both sides, print result
scripts/net-sim/run.sh --force          # also rebuild the runner image
scripts/net-sim/compare.sh              # run.sh, then quic/run.sh, then a throughput table for both
scripts/net-sim/benchmark.sh [N]        # both protocols, N times each (default 100) - see "Benchmarking"
```

To change transfer size or lane count, edit the `SIZE_MB` / `LANES` env
values in `tx-pod.yaml` / `rx-pod.yaml` directly - net-sim's config lives in
YAML, not script flags. `LANES` is the real throughput lever: one
`std::thread` per lane (`src/proto/transfer.cpp`), and podman containers
already get the full host core count by default - there's no VM to size
(unlike the old harness's `-smp`), so raising it costs nothing but CPU
contention with itself once lanes exceed free cores. `rx-pod.yaml` and
`tx-pod.yaml` must agree on `LANES` (rx listens on
`base_port..base_port+LANES-1`, tx has to send that same count).

## QUIC/HTTP3 comparison

`quic/` runs the same payload over HTTP/3 on the same bridge - `curl`
(already built with `ngtcp2`/`nghttp3` in Fedora's packaged build) against
`caddy` (auto HTTP/3, zero-config TLS via `tls internal`), both stock dnf
packages, no QUIC stack compiled from source. See `quic/entrypoint.sh` for
two non-obvious fixes it needed: a Caddyfile syntax gotcha, and Caddy's
automatic HTTPS needing a matching SNI/hostname (via `curl --resolve`) since
a bare `:443` site block with no hostname can't pick a certificate for a
client connecting straight to an IP.

It's not a fair fight by design, and worth reading that way: fuse is raw UDP
with no TLS record framing, custom lanes/congestion control tuned for
exactly this bridge; QUIC here rides through Caddy's general-purpose HTTP
file-serving stack, not tuned for bulk throughput. One measured run
(SIZE_MB=2048, LANES=6):

| protocol | bytes | seconds | throughput |
|---|---|---|---|
| fuse | 2,147,483,648 | 0.809 | 2533.1 MB/s (9013 retransmits) |
| quic (HTTP/3 via caddy) | 2,147,483,648 | 2.772 | 774.8 MB/s |

fuse's retransmit count is real and worth noting as a tradeoff, not just a
speed win: it's pushing hard enough on this link to lose and resend packets,
while QUIC's loss recovery kept it retransmit-free at a third of the
throughput. Single-run numbers are noisy - see "Benchmarking" for the
repeated version with variance.

## Benchmarking

`benchmark.sh [N]` (default 100) runs both protocols N times each, one
pod-create/destroy cycle per iteration, and writes:

- `.cache/logs/benchmark.csv` - one row per run: `run,protocol,bytes,seconds,mbps,retransmits,sha_match`
- `.cache/logs/benchmark-summary.md` - mean/stdev/min/max/median/p95 per protocol (`bench-summary.py`)

All `N*2` iterations run inside a single `pkexec` call (`bench-loop.sh`,
the actual worker - not meant to be run directly) - one password prompt for
the whole sweep, not `N` of them. It's unattended after that, but not fast:
each iteration regenerates its own random payload and does a couple of
`podman kube play`/`down` round trips on top of the transfer itself, so
N=100 at SIZE_MB=2048 is realistically 10-30+ minutes depending on the
machine. `tail -f .cache/logs/benchmark.csv` in another terminal to watch
rows land as it runs.

`plot-benchmark.py <benchmark.csv> <output-dir>` turns that CSV into
figures (box plot, histograms, empirical CDF, per-run time series,
retransmit distribution + correlation, a mean±stdev bar chart - both `.png`
at 300 DPI and vector `.pdf`) plus `stats.md` (means, 95% CIs, coefficient
of variation, Pearson r). Needs `matplotlib`/`numpy` (already present here
via pip; `dnf install python3-matplotlib python3-numpy` otherwise).

### N=100 results (SIZE_MB=2048, LANES=6)

| protocol | mean MB/s | stdev | 95% CI | min | max | median | p95 |
|---|---|---|---|---|---|---|---|
| fuse | 2533.3 | 82.9 | [2517.1, 2549.6] | 2366.9 | 2721.7 | 2533.1 | 2672.7 |
| quic | 760.6 | 15.4 | [757.6, 763.6] | 702.6 | 794.4 | 760.6 | 784.0 |

100/100 sha256-verified successful transfers for both protocols. fuse's mean
was 3.33x QUIC's, with tighter *relative* variance (CV 3.3% vs 2.0% is close,
but fuse's absolute spread covers a much larger range in absolute MB/s).
fuse's retransmit count (mean 8018/run, range 4213-12009) showed essentially
**no correlation with its own throughput** (Pearson r = -0.07) - the
congestion control appears to absorb retransmission cost without it showing
up as a per-run speed penalty in this range, at least on this host-local
link.

## How it's built

- **`Containerfile`**: minimal Fedora image with just `glibc`/`libstdc++`/
  `libgcc` - enough to run fuse's own binaries, which are *not* baked in.
- **`stage-bin.sh`**: copies the host-built `fuse_quickstart_send`/`recv` +
  their non-system shared libs (`libfuse_proto.so`, `libwolfssl.so`) into
  `.cache/payload/{bin,lib}`, then relabels it `container_file_t` so
  Fedora's enforcing SELinux allows `container_t` to *execute* it (kube
  play's automatic hostPath relabel only covers read/write - without this
  you get `stdbuf: failed to run command ...: Permission denied`). Rerun
  after rebuilding fuse; no root needed either way.
- **`rx-pod.yaml` / `tx-pod.yaml`**: one-container Kube pod each, bind-mount
  `.cache/payload` read-only at `/opt/fuse`, env vars set role/peer/size/
  lanes. Each is played as its *own* pod - its own network namespace - so rx
  and tx are genuinely separate hosts on the bridge, not two processes
  sharing one stack. tx's `PEER` is a literal `@RX_IP@` placeholder -
  `fuse_quickstart_send` takes its peer straight into `inet_pton`, no DNS,
  so `run.sh` fills in rx's real container IP before playing it.
- **`entrypoint.sh`**: the image's `ENTRYPOINT`. Reads `ROLE` and execs
  `fuse_quickstart_send` or `_recv` directly against the container's real
  interface - no VM, no PID 1 tricks, the container runtime already handles
  that.
- **`run.sh`**: creates the `fusenet` bridge network, plays rx, reads back
  its IP (`podman inspect ... NetworkSettings.Networks.fusenet.IPAddress`),
  renders that into tx-pod.yaml and plays it, waits for both to exit, diffs
  the sha256 the two sides printed, tears everything down.

Everything under `.cache/` is generated and gitignored; delete it any time
to force `stage-bin.sh` to redo its work.

## Why root

Rootless podman's default networking (`pasta`) fails outright in this
environment for *any* bridge network, not just this harness -
`podman run --network <bridge-net> ...` alone reproduces it:

```
Error: setting up Pasta: pasta failed with exit code 1:
Failed to remount /: Permission denied
```

Root podman doesn't go through pasta (real netavark bridge plumbing, real
`CAP_NET_ADMIN`), so `run.sh` batches the whole network/pod lifecycle into
one `pkexec` call - same reason the old `setup-net.sh` needed root for host
bridge/tap devices, one graphical password prompt instead of several. If
rootless bridge networking works fine on your machine, there's nothing here
that requires root in principle - only that pkexec wrapper exists to work
around this one environment's pasta breakage.

## Known ceiling

`run.sh` polls `.cache/logs/rx.log` for rx's "listening on" line before
starting tx, and polls container state for both to reach `exited` before
reading results - a polling loop, not an event notification. Fine at this
scale (two containers, one run at a time); would need real synchronization
if this ever ran many pairs concurrently.

## AI assistance

This harness (the container-bridge rearchitecture, the QUIC comparison, the
benchmark/plotting tooling) was built interactively with
[Claude Code](https://claude.com/claude-code), including the debugging that
shows up on the wiki's
[Net-Sim Design Decisions](https://github.com/kamalkoushikd/fuse-nw/wiki/Net-Sim-Design-Decisions)
page - the SELinux exec-permission fix, the `inet_pton`-not-DNS peer-resolution
bug, the Caddy SNI/TLS handshake failure, and the `chown`-after-teardown
ordering bug were all found and fixed in that session, not pre-existing
knowledge applied from outside it. The repo owner directed the requirements
and design tradeoffs, and personally ran every privileged command (the
assistant's execution environment can't invoke `pkexec`, so every actual
benchmark run behind the published numbers was executed by hand, not by the
assistant).
