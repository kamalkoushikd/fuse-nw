#!/usr/bin/env python3
"""QUIC receiver for the fuse-vs-QUIC comparison test.

Accepts exactly one QUIC connection and one stream on it, reads an 8-byte
length header the sender sends first (plain QUIC has no analog to fuse's
StreamStart, so without this the receiver would have no total to show a
percentage/ETA against), writes everything after that to OUT_FILE, sends
back a small "OK" once the sender has signalled end-of-data, then exits.
Mirrors fuse_quickstart_recv's shape (bind, wait, write file, report
throughput) so the two are comparable.

Requires: pip install aioquic

Usage: quic_receiver.py <bind-address> <port>
"""

import asyncio
import datetime
import struct
import sys
import time

from aioquic.asyncio import serve
from aioquic.quic.configuration import QuicConfiguration
from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

from quic_common import finish_progress_line, make_progress_printer

OUT_FILE = "/tmp/testoutfile_quic"
ALPN = "fuse-quic-test"

# QUIC's per-connection/per-stream flow-control windows default to 1 MiB —
# fine for small messages, a real bottleneck for a multi-gigabyte transfer
# (the sender stalls waiting for window updates every megabyte). 16 MiB is a
# reasonable "if you were actually doing bulk transfer you'd raise this"
# value, not a hand-tuned maximum — the point of this script is an honest,
# roughly-as-a-user-would-configure-it comparison, not a rigged one.
FLOW_WINDOW = 16 * 1024 * 1024


def make_self_signed_cert():
    # QUIC mandates TLS 1.3; there's no shared filesystem between the two
    # machines to pre-share a cert, so the receiver mints its own ad hoc
    # one on every run and the sender simply doesn't verify it (see
    # quic_sender.py) — fine for a throughput/correctness test, not a
    # substitute for a real deployment's certificate handling.
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "fuse-quic-test")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=1))
        .sign(key, hashes.SHA256())
    )
    return cert, key


async def main(bind_address: str, port: int) -> None:
    cert, key = make_self_signed_cert()
    config = QuicConfiguration(
        is_client=False,
        alpn_protocols=[ALPN],
        max_data=FLOW_WINDOW,
        max_stream_data=FLOW_WINDOW,
    )
    config.certificate = cert
    config.private_key = key

    done = asyncio.Event()

    async def handle_stream_async(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        t0 = time.monotonic()
        (expected,) = struct.unpack(">Q", await reader.readexactly(8))
        report = make_progress_printer("received")

        written = 0
        with open(OUT_FILE, "wb") as f:
            while True:
                chunk = await reader.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                written += len(chunk)
                report(written, expected, time.monotonic() - t0)
        elapsed = time.monotonic() - t0
        report(written, expected, elapsed, force=True)
        finish_progress_line()

        mbps = (written / (1024 * 1024)) / elapsed if elapsed > 0 else 0.0
        print(f"received {written} bytes in {elapsed:.3f} s ({mbps:.1f} MB/s) -> {OUT_FILE}")
        writer.write(b"OK")
        writer.write_eof()
        await writer.drain()
        done.set()

    def stream_handler(reader, writer):
        # aioquic calls this synchronously; the actual work must be
        # scheduled onto the loop, not run inline.
        asyncio.ensure_future(handle_stream_async(reader, writer))

    server = await serve(bind_address, port, configuration=config, stream_handler=stream_handler)
    print(f"listening on {bind_address}:{port} (QUIC)")
    try:
        await done.wait()
    finally:
        server.close()


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <bind-address> <port>", file=sys.stderr)
        sys.exit(2)
    asyncio.run(main(sys.argv[1], int(sys.argv[2])))
