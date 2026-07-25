# fuse — Python bindings

Socket-style Python API for the [Fuse](https://github.com/kamalkoushikd/fuse-nw)
transport: reliable, message-oriented, optionally encrypted, over UDP.

```python
import fuse

# server
with fuse.listen(port=4433) as server:
    conn = server.accept()
    print(conn.recv())
    conn.send(b"pong")

# client
with fuse.connect("127.0.0.1", 4433) as conn:
    conn.send(b"ping")
    print(conn.recv())
```

These bindings are pure `ctypes` — no compiler, no build step — but they do
need the Fuse shared library. Install the SDK first:

```sh
curl -fsSL https://github.com/kamalkoushikd/fuse-nw/releases/latest/download/install.sh | sh
```

which installs the library *and* this package. If you install the package on
its own (`pip install fuse-transport`), point it at a library with
`FUSE_LIBRARY=/path/to/libfuse_proto.so`.

Full guide: [docs/SDK.md](https://github.com/kamalkoushikd/fuse-nw/blob/main/docs/SDK.md)
