#!/bin/bash
# QUIC/HTTP3 side of the comparison, mirroring ../entrypoint.sh's role
# dispatch and RESULT-line conventions so run.sh's sha256/throughput
# parsing works the same way for both protocols.
#
# rx = server: generates the payload itself (pull model - HTTP serves files,
# it doesn't accept pushed ones) and serves it over HTTP/3 via caddy. Runs
# forever (a server has no natural "done"); run.sh tears it down once tx
# finishes.
# tx = client: curl --http3-only downloads it, timed.
set -euo pipefail

LOG="/var/log/quic-netsim/quic-${ROLE:-unknown}.log"
mkdir -p "$(dirname "$LOG")"
SIZE_MB="${SIZE_MB:-2048}"

case "${ROLE:-}" in
  rx)
    {
      mkdir -p /srv
      dd if=/dev/urandom of=/srv/payload.bin bs=1M count="$SIZE_MB" 2>/dev/null
      echo "=== quic-net-sim RESULT sha256-source $(sha256sum /srv/payload.bin) ==="
      # A bare ":443" site block gives caddy's automatic HTTPS no hostname to
      # pick a certificate for, and a client connecting straight to an IP
      # sends no SNI to match against - handshake fails with a generic TLS
      # "internal error" alert before caddy logs anything. A named site
      # address fixes it; tx uses curl --resolve to point that name at
      # quic-rx's real IP without needing actual DNS (the standard trick for
      # testing HTTPS servers by IP).
      cat > /tmp/Caddyfile <<'EOF'
quic-rx.local:443 {
	tls internal
	root * /srv
	file_server
}
EOF
      echo "=== quic-net-sim: starting caddy (http/3) on :443 ==="
      exec caddy run --config /tmp/Caddyfile --adapter caddyfile
    } 2>&1 | tee "$LOG"
    ;;
  tx)
    {
      : "${PEER:?PEER env var required for tx}"
      echo "=== quic-net-sim: downloading via HTTP/3 from quic-rx.local ($PEER) ==="
      # --retry-all-errors rides out the race against rx's dd+caddy startup
      # (no TCP "connection refused" equivalent to key off for QUIC/UDP), so
      # no log-polling handshake is needed before playing this pod.
      # --resolve: no real DNS here, but caddy needs a matching SNI/Host to
      # pick a certificate (see rx's Caddyfile comment) - this sends
      # "quic-rx.local" as both while still dialing $PEER's real IP.
      curl -sk --http3-only --retry 60 --retry-delay 1 --retry-all-errors \
        --resolve "quic-rx.local:443:${PEER}" \
        -o /tmp/out.bin \
        -w "=== quic-net-sim RESULT curl time_total=%{time_total}s speed_download=%{speed_download}B/s size_download=%{size_download}B http_version=%{http_version} ===\n" \
        "https://quic-rx.local/payload.bin"
      echo "=== quic-net-sim RESULT sha256 $(sha256sum /tmp/out.bin) ==="
    } 2>&1 | tee "$LOG"
    ;;
  *)
    echo "ROLE must be rx or tx (got '${ROLE:-<unset>}')" >&2
    exit 1
    ;;
esac
