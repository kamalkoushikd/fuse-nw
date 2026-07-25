#!/usr/bin/env python3
"""Fuse SDK example: a concurrent echo server in Python.

    ./echo_server.py 4433 [pre-shared-key]

Same shape as a socketserver-style TCP server: accept in a loop, hand each
connection to a thread.
"""
import sys
import threading

import fuse


def serve(conn):
    host, port = conn.peer
    print(f"[+] {host}:{port} connected", flush=True)
    with conn:
        # Iterating a connection yields messages until the peer closes.
        for message in conn:
            print(f"[>] {host}:{port} {len(message)} bytes", flush=True)
            conn.send(message)
    print(f"[-] {host}:{port} disconnected", flush=True)


def main():
    if len(sys.argv) < 2:
        sys.exit(f"usage: {sys.argv[0]} <port> [pre-shared-key]")
    port = int(sys.argv[1])
    key = sys.argv[2] if len(sys.argv) > 2 else None

    with fuse.listen(port=port, key=key) as server:
        print(f"echo server on port {server.port}"
              f"{', encrypted' if key else ''} (fuse {fuse.__version__})", flush=True)
        for conn in server:
            threading.Thread(target=serve, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
