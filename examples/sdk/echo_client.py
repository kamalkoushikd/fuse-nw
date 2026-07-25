#!/usr/bin/env python3
"""Fuse SDK example: the matching echo client.

    ./echo_client.py 127.0.0.1 4433 "hello" [pre-shared-key]
"""
import sys

import fuse


def main():
    if len(sys.argv) < 4:
        sys.exit(f"usage: {sys.argv[0]} <host> <port> <message> [pre-shared-key]")
    host, port, message = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    key = sys.argv[4] if len(sys.argv) > 4 else None

    with fuse.connect(host, port, key=key, timeout=5) as conn:
        conn.send(message)
        print("echo:", conn.recv(timeout=5).decode())
        s = conn.stats
        print(f"({s.messages_sent} sent, {s.messages_received} received, "
              f"rtt {s.rtt_us} us)")


if __name__ == "__main__":
    main()
