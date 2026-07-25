"""``python -m fuse`` — check that an installed Fuse works on this machine.

A wheel carries a compiled transport inside it, so "did it install" and "does
it run here" are different questions: the kernel needs ``recvmmsg`` and UDP
segmentation offload, and the library has to resolve its own dependencies. A
loopback round-trip answers both in about a second, which beats debugging it
from an application.

    python -m fuse            # library info + a loopback round-trip
    python -m fuse info       # just the library info
"""

from __future__ import annotations

import sys
import threading


def _info() -> int:
    import fuse
    from fuse import _native

    lib = getattr(_native.lib, "_name", "<unknown>")
    print(f"fuse-transport  {fuse.__version__}")
    print(f"library         {lib}")
    print(f"encryption      {'available' if fuse.encryption_available() else 'not built in'}")
    print(f"max message     {fuse.max_message():,} bytes")
    print(f"python          {sys.version.split()[0]} on {sys.platform}")
    return 0


def _selftest() -> int:
    import fuse

    rc = _info()
    print()

    payload = b"fuse selftest" * 1000  # a few blocks, so it exercises framing
    errors: list[BaseException] = []

    # Port 0 lets the OS choose, so a busy machine cannot make this flaky.
    server = fuse.listen(port=0, bind="127.0.0.1")
    port = server.port

    def echo() -> None:
        try:
            with server.accept(timeout=10) as conn:
                for message in conn:
                    conn.send(message)
        except BaseException as exc:  # reported from the main thread
            errors.append(exc)

    thread = threading.Thread(target=echo, daemon=True)
    thread.start()

    try:
        with fuse.connect("127.0.0.1", port, timeout=10) as conn:
            conn.send(payload)
            echoed = conn.recv(timeout=10)
            host, peer_port = conn.peer
            rtt = conn.stats.rtt_us
    except BaseException as exc:
        errors.append(exc)
        echoed = b""
    finally:
        server.close()
        thread.join(timeout=5)

    if errors:
        print(f"FAILED: {errors[0]!r}", file=sys.stderr)
        return 1
    if echoed != payload:
        print(
            f"FAILED: echoed {len(echoed)} bytes, sent {len(payload)}",
            file=sys.stderr,
        )
        return 1

    print(f"loopback round-trip  {len(payload):,} bytes via {host}:{peer_port}  OK")
    print(f"rtt                  {rtt} us")
    return rc


def main(argv: list[str]) -> int:
    command = argv[1] if len(argv) > 1 else "selftest"
    if command == "info":
        return _info()
    if command == "selftest":
        return _selftest()
    print(f"usage: python -m fuse [selftest|info]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
