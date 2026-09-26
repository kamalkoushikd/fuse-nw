#!/usr/bin/env python3
"""QUIC sender for the fuse-vs-QUIC comparison test.

Connects, opens one stream, sends an 8-byte length header (so the receiver
can show a real percentage/ETA too — plain QUIC has nothing like fuse's
StreamStart to carry that) followed by the whole file, signals end-of-data,
and waits for the receiver's "OK" before reporting throughput. Mirrors
fuse_quickstart_send's shape (connect, send file, report throughput) so the
two are comparable.

Requires: pip install aioquic

Usage: quic_sender.py <receiver-address> <port> <in-file>
"""

import asyncio
import ssl
import struct
import sys
import time

from aioquic.asyncio import connect
from aioquic.quic.configuration import QuicConfiguration

from quic_common import finish_progress_line, make_progress_printer

ALPN = "fuse-quic-test"
FLOW_WINDOW = 16 * 1024 * 1024  # see quic_receiver.py's comment


async def main(host: str, port: int, path: str) -> None:
    config = QuicConfiguration(
        is_client=True,
        alpn_protocols=[ALPN],
        max_data=FLOW_WINDOW,
        max_stream_data=FLOW_WINDOW,
        # The receiver mints an ad hoc self-signed cert per run (no shared
        # filesystem between the two machines to pre-share one) — nothing
        # to verify it against, so this is a deliberate skip, not an
        # oversight.
        verify_mode=ssl.CERT_NONE,
    )

    with open(path, "rb") as f:
        data = f.read()
    total = len(data)
    report = make_progress_printer("sent")

    t0 = time.monotonic()
    async with connect(host, port, configuration=config) as client:
        reader, writer = await client.create_stream()
        writer.write(struct.pack(">Q", total))
        chunk_size = 1 << 20
        sent = 0
        for i in range(0, total, chunk_size):
            writer.write(data[i : i + chunk_size])
            await writer.drain()
            sent += min(chunk_size, total - i)
            report(sent, total, time.monotonic() - t0)
        writer.write_eof()
        ack = await reader.read(2)

    elapsed = time.monotonic() - t0
    report(total, total, elapsed, force=True)
    finish_progress_line()

    mbps = (total / (1024 * 1024)) / elapsed if elapsed > 0 else 0.0
    print(f"sent {total} bytes in {elapsed:.3f} s ({mbps:.1f} MB/s)")
    if ack != b"OK":
        print("receiver did not acknowledge completion", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <receiver-address> <port> <in-file>", file=sys.stderr)
        sys.exit(2)
    asyncio.run(main(sys.argv[1], int(sys.argv[2]), sys.argv[3]))
