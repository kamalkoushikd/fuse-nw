#!/usr/bin/env bash
# Privileged worker for benchmark.sh - invoked via pkexec, not meant to be
# run directly. Loops fuse then QUIC N times each on the fusenet bridge, one
# pod-create/destroy cycle per iteration, appending one CSV row per
# iteration to .cache/logs/benchmark.csv. Builds images/network once,
# up front, if missing.
set -euo pipefail

NETSIM="$1"
QUIC="$NETSIM/quic"
N="$2"
OWNER_UID="$3"
OWNER_GID="$4"
CSV="$NETSIM/.cache/logs/benchmark.csv"

# Always hand the logs back to the invoking user, even if something above
# this point fails under set -e (e.g. `podman network rm` erroring leaves
# everything root-owned and unreadable-for-write by the real user otherwise
# - happened once already, chown was the last line with nothing to run it
# on failure).
trap 'chown -R "$OWNER_UID:$OWNER_GID" "$NETSIM/.cache/logs" 2>/dev/null || true' EXIT

podman image exists fuse-netsim || podman build -t fuse-netsim "$NETSIM" >/dev/null
podman image exists fuse-netsim-quic || podman build -t fuse-netsim-quic "$QUIC" >/dev/null
podman network exists fusenet || podman network create fusenet >/dev/null

fuse_iter() {
  local i="$1"
  podman pod exists rx 2>/dev/null && podman kube down "$NETSIM/rx-pod.yaml" >/dev/null
  podman pod exists tx 2>/dev/null && podman kube down "$NETSIM/tx-pod.yaml" >/dev/null

  podman kube play --network fusenet "$NETSIM/rx-pod.yaml" >/dev/null
  for _ in $(seq 1 100); do
    grep -q 'listening on' "$NETSIM/.cache/logs/rx.log" 2>/dev/null && break
    sleep 0.2
  done

  local rx_ip
  rx_ip=$(podman inspect rx-app --format '{{.NetworkSettings.Networks.fusenet.IPAddress}}')
  sed "s/@RX_IP@/$rx_ip/" "$NETSIM/tx-pod.yaml" > /tmp/bench-fuse-tx.yaml
  podman kube play --network fusenet /tmp/bench-fuse-tx.yaml >/dev/null
  rm -f /tmp/bench-fuse-tx.yaml

  for _ in $(seq 1 300); do
    local state
    state=$(podman inspect rx-app tx-app --format '{{.State.Status}}' 2>/dev/null | sort -u)
    [ "$state" = exited ] && break
    sleep 0.5
  done

  local sent_line src_sha dst_sha bytes secs mbps retx match
  sent_line=$(grep -oE '^sent [0-9]+ bytes in [0-9.]+ s \([0-9.]+ MB/s\), [0-9]+ retransmits' \
    "$NETSIM/.cache/logs/tx.log" || true)
  src_sha=$(awk '/RESULT sha256-source/{print $5}' "$NETSIM/.cache/logs/tx.log")
  dst_sha=$(awk '/RESULT sha256 /{print $5}' "$NETSIM/.cache/logs/rx.log")
  if [ -n "$src_sha" ] && [ "$src_sha" = "$dst_sha" ]; then match=1; else match=0; fi
  if [ -n "$sent_line" ]; then
    bytes=$(grep -oE '^sent [0-9]+' <<<"$sent_line" | awk '{print $2}')
    secs=$(grep -oE 'in [0-9.]+ s' <<<"$sent_line" | awk '{print $2}')
    mbps=$(grep -oE '\([0-9.]+ MB/s\)' <<<"$sent_line" | tr -d '()' | awk '{print $1}')
    retx=$(grep -oE '[0-9]+ retransmits' <<<"$sent_line" | awk '{print $1}')
  else
    bytes=""; secs=""; mbps=""; retx=""
  fi
  echo "$i,fuse,$bytes,$secs,$mbps,$retx,$match" >> "$CSV"

  podman kube down "$NETSIM/rx-pod.yaml" "$NETSIM/tx-pod.yaml" >/dev/null
}

quic_iter() {
  local i="$1"
  podman pod exists quic-rx 2>/dev/null && podman kube down "$QUIC/rx-pod.yaml" >/dev/null
  podman pod exists quic-tx 2>/dev/null && podman kube down "$QUIC/tx-pod.yaml" >/dev/null

  podman kube play --network fusenet "$QUIC/rx-pod.yaml" >/dev/null
  local rx_ip
  rx_ip=$(podman inspect quic-rx-app --format '{{.NetworkSettings.Networks.fusenet.IPAddress}}')
  sed "s/@RX_IP@/$rx_ip/" "$QUIC/tx-pod.yaml" > /tmp/bench-quic-tx.yaml
  podman kube play --network fusenet /tmp/bench-quic-tx.yaml >/dev/null
  rm -f /tmp/bench-quic-tx.yaml

  for _ in $(seq 1 300); do
    local state
    state=$(podman inspect quic-tx-app --format '{{.State.Status}}' 2>/dev/null)
    [ "$state" = exited ] && break
    sleep 0.5
  done

  local curl_line src_sha dst_sha bytes secs mbps match
  curl_line=$(grep -oE 'time_total=[0-9.]+s speed_download=[0-9.]+B/s size_download=[0-9]+B' \
    "$NETSIM/.cache/logs/quic-tx.log" || true)
  src_sha=$(awk '/RESULT sha256-source/{print $5}' "$NETSIM/.cache/logs/quic-rx.log")
  dst_sha=$(awk '/RESULT sha256 /{print $5}' "$NETSIM/.cache/logs/quic-tx.log")
  if [ -n "$src_sha" ] && [ "$src_sha" = "$dst_sha" ]; then match=1; else match=0; fi
  if [ -n "$curl_line" ]; then
    secs=$(grep -oE 'time_total=[0-9.]+' <<<"$curl_line" | cut -d= -f2)
    bytes=$(grep -oE 'size_download=[0-9]+' <<<"$curl_line" | cut -d= -f2)
    mbps=$(awk -v b="$bytes" -v t="$secs" 'BEGIN { if (t > 0) printf "%.1f", b / t / 1000000; else print 0 }')
  else
    bytes=""; secs=""; mbps=""
  fi
  echo "$i,quic,$bytes,$secs,$mbps,,$match" >> "$CSV"

  podman kube down "$QUIC/rx-pod.yaml" "$QUIC/tx-pod.yaml" >/dev/null
}

for i in $(seq 1 "$N"); do
  echo "--- fuse run $i/$N ---"
  fuse_iter "$i"
done
for i in $(seq 1 "$N"); do
  echo "--- quic run $i/$N ---"
  quic_iter "$i"
done

podman network rm fusenet >/dev/null
