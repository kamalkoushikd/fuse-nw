# fuse-transport

Socket-style Python bindings for [Fuse](https://github.com/kamalkoushikd/fuse-nw),
an experimental UDP transport with **per-stream congestion control** — so a
stalled bulk transfer cannot throttle unrelated latency-sensitive traffic on
the same session, which is the thing QUIC's connection-wide controller cannot
do.

```sh
pip install fuse-transport
# or
uv add fuse-transport
```

Linux only. Wheels bundle the compiled transport and its wolfSSL, so there is
no compiler, no CMake, and no system library to install.

```python
import fuse
```

## Server

```python
import threading
import fuse

def handle(conn):
    with conn:
        for message in conn:        # iterates until the peer closes
            conn.send(b"ECHO:" + message)

with fuse.listen(port=4433) as server:
    for conn in server:             # iterates incoming connections
        threading.Thread(target=handle, args=(conn,), daemon=True).start()
```

## Client

```python
import fuse

with fuse.connect("192.0.2.10", 4433, timeout=5) as conn:
    conn.send(b"hello")             # str is UTF-8 encoded for you
    print(conn.recv(timeout=5).decode())
```

## Two ways this differs from a TCP socket

**Messages, not a byte stream.** One `send` becomes exactly one `recv`.
Boundaries are preserved (like `SOCK_SEQPACKET`), so you never length-prefix,
never scan for delimiters, and a partial read cannot happen.

**`send` means delivered.** It returns once the peer has acknowledged the
message, not when it was queued locally. A successful send is proof of arrival.

Retransmission, ordering, congestion control and adaptive block sizing are
handled underneath.

## Encryption

Set the same key on both ends for AES-256-GCM:

```python
fuse.listen(port=4433, key="shared-secret")
fuse.connect("192.0.2.10", 4433, key="shared-secret")
```

A client presenting the wrong key is rejected during the handshake and never
exchanges data. Note this is a **pre-shared key, so there is no forward
secrecy**: someone who records traffic and later obtains the key can decrypt
it. If that matters, run Fuse inside a tunnel that provides it.

## API

| call | notes |
|---|---|
| `fuse.listen(port, bind="0.0.0.0", key=None, timeout=None)` | → `Listener` |
| `fuse.connect(host, port, key=None, timeout=None)` | → `Connection` |
| `Listener.accept(timeout=None)` | → `Connection`; iterate the listener for a loop |
| `Listener.port` | actual port (pass `port=0` to let the OS pick) |
| `Connection.send(data)` | `bytes` / `bytearray` / `memoryview` / `str` |
| `Connection.recv(timeout=None)` | → `bytes`; iterate for a message loop |
| `Connection.peer` | `(address, port)` |
| `Connection.stats` | counters, including `rtt_us` |
| `Connection.closed` | `bool` |
| `.close()` | also via `with` blocks; idempotent |

Timeouts are **seconds** (float): `None` waits indefinitely, `0` polls.

Exceptions all derive from `fuse.FuseError`: `Timeout`, `ConnectionClosed`,
`AuthError`, `ConfigError`, `MessageTooLarge`.

Blocking calls release the GIL, so other threads keep running while one is
parked in `recv`.

## Checking an install

```sh
python -m fuse selftest
```

Prints the version, which library got loaded, and whether encryption is
available, then runs a loopback round-trip — which also confirms this kernel
supports the socket features Fuse needs.

## Also in this project

A C/C++ SDK with the same API (`#include <fuse/sdk.h>`), and a separate
higher-throughput API for moving whole files or large buffers. Both come from
the standalone installer:

```sh
curl -fsSL https://github.com/kamalkoushikd/fuse-nw/releases/latest/download/install.sh | sh
```

- **Full SDK guide:** https://github.com/kamalkoushikd/fuse-nw/blob/main/docs/SDK.md
- **Benchmarks vs QUIC:** https://github.com/kamalkoushikd/fuse-nw/blob/main/bench/RESULTS.md

## License

MIT. Note that the bundled wolfSSL is GPLv2 (a commercial license is
available from wolfSSL Inc.), so a wheel that links it is a combined work
subject to the GPL when redistributed.
