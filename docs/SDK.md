# Fuse SDK

Write networked programs over Fuse the way you would over TCP sockets:
listen, accept, connect, send, receive — in C or Python.

```
        server                          client
        ------                          ------
        listen(port)                    connect(host, port)
        accept()          <────────────
        recv()            <────────────  send(b"...")
        send(b"...")      ────────────>  recv()
        close()                          close()
```

## Install

### Python — from PyPI

```sh
pip install fuse-transport      # or: uv add fuse-transport
```

The distribution is `fuse-transport` (plain `fuse` was taken); you still
`import fuse`. Wheels carry the compiled transport and its wolfSSL inside the
package, so nothing needs a compiler or a system library. Verify with:

```sh
python -m fuse selftest
```

which prints the loaded library path and runs a loopback round-trip.

### C, C++, or everything at once

```sh
curl -fsSL https://github.com/kamalkoushikd/fuse-nw/releases/latest/download/install.sh | sh
```

Installs to `/usr/local` when run as root, `~/.local` otherwise — so it never
*requires* sudo. Useful flags:

| flag | meaning |
|---|---|
| `--prefix DIR` | install somewhere specific (e.g. `/opt/fuse`) |
| `--version X.Y.Z` | pin a release instead of taking the latest |
| `--no-python` | skip the Python package |
| `--uninstall` | remove a previous install |

One download provides the C/C++ libraries and headers, CMake and pkg-config
integration, and the Python package. It bundles its own wolfSSL, so there is
nothing else to install.

> **Linux only, x86-64 or arm64.** The transport uses `recvmmsg` and UDP
> segmentation offload. Building from source works anywhere those exist.

Verify it:

```sh
fuse_quickstart_recv 4433 /tmp/out.bin &
fuse_quickstart_send 127.0.0.1 4433 /etc/hostname
```

## Two things to know before you write code

**Messages, not a byte stream.** One `send` becomes exactly one `recv`.
Boundaries are preserved (like `SOCK_SEQPACKET`), so you never length-prefix,
never scan for delimiters, and a partial read cannot happen.

**`send` means delivered.** It returns once the peer has acknowledged the
message — not when it was merely queued locally. A successful send is proof
of arrival.

Everything else is handled underneath: retransmission, ordering, congestion
control, and adaptive block sizing.

## Python

```python
import fuse
```

### Server

```python
import threading
import fuse

def handle(conn):
    host, port = conn.peer
    print(f"{host}:{port} connected")
    with conn:
        for message in conn:          # iterates until the peer closes
            conn.send(b"ECHO:" + message)

with fuse.listen(port=4433) as server:
    for conn in server:               # iterates incoming connections
        threading.Thread(target=handle, args=(conn,), daemon=True).start()
```

### Client

```python
import fuse

with fuse.connect("192.0.2.10", 4433, timeout=5) as conn:
    conn.send(b"hello")               # str is UTF-8 encoded for you
    reply = conn.recv(timeout=5)
    print(reply.decode())
```

### API

| call | notes |
|---|---|
| `fuse.listen(port, bind="0.0.0.0", key=None, timeout=None)` | → `Listener` |
| `fuse.connect(host, port, key=None, timeout=None)` | → `Connection` |
| `Listener.accept(timeout=None)` | → `Connection`; iterate the listener for a loop |
| `Listener.port` | actual port (pass `port=0` to let the OS pick) |
| `Connection.send(data)` | `bytes`/`bytearray`/`memoryview`/`str` |
| `Connection.recv(timeout=None)` | → `bytes`; iterate for a message loop |
| `Connection.peer` | `(address, port)` |
| `Connection.stats` | counters, including `rtt_us` |
| `Connection.closed` | `bool` |
| `.close()` | also via `with` blocks; idempotent |

Timeouts are **seconds** (float). `None` waits indefinitely, `0` polls.

Exceptions all derive from `fuse.FuseError`: `Timeout`, `ConnectionClosed`,
`AuthError`, `ConfigError`, `MessageTooLarge`.

Blocking calls release the GIL, so other Python threads keep running while
one is parked in `recv`.

## C

```c
#include <fuse/sdk.h>
```

Build with pkg-config:

```sh
cc app.c -o app $(pkg-config --cflags --libs fuse)
```

or CMake:

```cmake
find_package(fuse CONFIG REQUIRED)
target_link_libraries(app PRIVATE fuse::proto)
```

### Server

```c
fuse_config cfg;
fuse_config_init(&cfg);              /* always init first */
cfg.bind_address = "0.0.0.0";
cfg.port = 4433;

fuse_status err;
fuse_listener *l = fuse_listen(&cfg, &err);
if (!l) { fprintf(stderr, "listen: %s\n", fuse_strerror(err)); return 1; }

for (;;) {
    fuse_conn *c = fuse_accept(l, -1, &err);   /* -1 = wait indefinitely */
    if (!c) continue;

    void *msg; size_t len;
    if (fuse_recv_alloc(c, &msg, &len, 30000) == FUSE_OK) {
        fuse_send(c, msg, len);                /* echo it back */
        fuse_free(msg);
    }
    fuse_close(c);
}
```

### Client

```c
fuse_config cfg;
fuse_config_init(&cfg);
cfg.host = "192.0.2.10";
cfg.port = 4433;

fuse_status err;
fuse_conn *c = fuse_connect(&cfg, &err);
if (!c) { fprintf(stderr, "connect: %s\n", fuse_strerror(err)); return 1; }

fuse_send(c, "hello", 5);

char buf[65536];
size_t n;
if (fuse_recv(c, buf, sizeof buf, &n, 5000) == FUSE_OK)
    printf("%.*s\n", (int)n, buf);

fuse_close(c);
```

### Receiving without guessing a size

`fuse_recv` into a fixed buffer returns `FUSE_ERR_BUFFER` if the message is
larger, sets `*out_len` to the size needed, and **leaves the message
queued** — so you can allocate exactly and call again. Or let
`fuse_recv_alloc` do it and release with `fuse_free`.

### Timeouts

Every blocking call takes `int timeout_ms`: `-1` waits indefinitely, `0`
polls, `>0` bounds the wait.

### Status codes

| code | usual cause |
|---|---|
| `FUSE_OK` | success |
| `FUSE_ERR_CONFIG` | bad address, port, or arguments |
| `FUSE_ERR_SOCKET` | port already in use or blocked |
| `FUSE_ERR_TIMEOUT` | nobody there, or the peer went quiet |
| `FUSE_ERR_CLOSED` | the peer closed |
| `FUSE_ERR_AUTH` | pre-shared key mismatch |
| `FUSE_ERR_BUFFER` | buffer too small; `*out_len` says how much is needed |
| `FUSE_ERR_TOO_LARGE` | above `fuse_max_message()` |
| `FUSE_ERR_UNSUPPORTED` | key given to a build with no crypto |

`fuse_strerror()` turns any of them into a printable string.

## Encryption

Set the same key on both ends:

```python
fuse.listen(port=4433, key="shared-secret")
fuse.connect("host", 4433, key="shared-secret")
```

```c
cfg.pre_shared_key = "shared-secret";
```

This gives AES-256-GCM confidentiality and integrity, with a key derived
freshly per session, and costs roughly 18% throughput. A client presenting
the wrong key is rejected during the handshake — it never exchanges data.

> **No forward secrecy.** The key comes from your pre-shared key plus a
> per-session salt, so an attacker who records traffic and *later* obtains
> the key can decrypt it. If that matters, terminate Fuse inside a tunnel
> that does provide it, or use the DTLS path in `fuse/proto/dtls.hpp`.
>
> A build without the crypto backend refuses a key outright
> (`FUSE_ERR_UNSUPPORTED`) rather than quietly sending plaintext. Check with
> `fuse_encryption_available()` / `fuse.encryption_available()`.

## Ports and firewalls

A server needs **one** UDP port open — the one you pass to `listen`. Each
accepted connection then moves to its own ephemeral port automatically, so a
single listener serves many clients concurrently. Clients need no inbound
ports.

(The separate bulk file-transfer API in `fuse/transfer.hpp` is different: it
uses a *range* of consecutive ports. See [USAGE.md](USAGE.md).)

## Threading

- A `Connection` may be used from two threads, one sending and one
  receiving. That pattern is supported directly.
- Accept from one thread at a time.
- Each connection runs a background thread and holds about 1 MB of
  retransmit buffers, so plan for a few thousand concurrent connections per
  process rather than hundreds of thousands.

## Choosing between the two APIs

| you want | use |
|---|---|
| a server/client exchanging messages | `fuse/sdk.h` — this page |
| to move one big buffer or file, fast | `fuse/transfer.hpp` — [USAGE.md](USAGE.md) |
| to build your own protocol on the parts | `fuse/proto/*.hpp` |

## Runnable examples

`examples/sdk/` has a concurrent echo server and client in both languages:

```sh
# C
cc examples/sdk/echo_server.c -o server $(pkg-config --cflags --libs fuse)
cc examples/sdk/echo_client.c -o client $(pkg-config --cflags --libs fuse)
./server 4433 &
./client 127.0.0.1 4433 "hello"

# Python
python3 examples/sdk/echo_server.py 4433 &
python3 examples/sdk/echo_client.py 127.0.0.1 4433 "hello"
```

They interoperate: the Python client talks to the C server and vice versa.

## Troubleshooting

**`could not load the Fuse shared library`** — the SDK is not installed
where the loader looks. Point at it directly:

```sh
export FUSE_LIBRARY=/path/to/libfuse_proto.so
```

**`cannot find -lfuse_proto`** — pkg-config is not finding a non-standard
prefix:

```sh
export PKG_CONFIG_PATH=/your/prefix/lib/pkgconfig:$PKG_CONFIG_PATH
```

**Client times out** — check the *listener* port is open in the firewall
(UDP, not TCP), and that both ends agree on whether a key is set.

**`FUSE_ERR_AUTH`** — the keys differ, or one side set a key and the other
did not.
