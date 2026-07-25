"""Fuse — a reliable message transport over UDP, with a socket-style API.

If you have written a socket server, this will look familiar::

    # server
    import fuse

    with fuse.listen(port=4433) as server:
        conn = server.accept()
        print(conn.recv())
        conn.send(b"pong")
        conn.close()

    # client
    import fuse

    with fuse.connect("127.0.0.1", 4433) as conn:
        conn.send(b"ping")
        print(conn.recv())

Two differences from a TCP socket are worth knowing up front:

* **Messages, not a byte stream.** One :meth:`Connection.send` becomes
  exactly one :meth:`Connection.recv`. Boundaries are preserved, so you
  never length-prefix or scan for delimiters, and a partial read cannot
  happen.
* **send() means delivered.** It returns once the peer has acknowledged the
  message, not merely when it was queued locally.

Set ``key=`` on both ends to encrypt with AES-256-GCM. Note that a
pre-shared key gives confidentiality and integrity but *not* forward
secrecy: someone who records traffic and later learns the key can decrypt
it.
"""

from __future__ import annotations

import ctypes
from typing import Iterator, NamedTuple, Optional, Union

from . import _native as _n

__all__ = [
    "listen",
    "connect",
    "Listener",
    "Connection",
    "Stats",
    "FuseError",
    "Timeout",
    "ConnectionClosed",
    "AuthError",
    "ConfigError",
    "MessageTooLarge",
    "encryption_available",
    "max_message",
    "__version__",
]

__version__ = _n.version().decode()

Data = Union[bytes, bytearray, memoryview, str]


# --- errors ---------------------------------------------------------------


class FuseError(Exception):
    """Base class for every Fuse failure."""

    def __init__(self, status: int, message: Optional[str] = None):
        self.status = status
        super().__init__(message or _n.strerror(status).decode())


class Timeout(FuseError):
    """The operation did not complete within its timeout."""


class ConnectionClosed(FuseError):
    """The peer closed the connection."""


class AuthError(FuseError):
    """Authentication failed — usually a pre-shared key mismatch."""


class ConfigError(FuseError):
    """Invalid configuration, address, or port."""


class MessageTooLarge(FuseError):
    """The message exceeds :func:`max_message`."""


_EXC = {
    _n.ERR_TIMEOUT: Timeout,
    _n.ERR_CLOSED: ConnectionClosed,
    _n.ERR_AUTH: AuthError,
    _n.ERR_CONFIG: ConfigError,
    _n.ERR_SOCKET: ConfigError,
    _n.ERR_TOO_LARGE: MessageTooLarge,
}


def _check(status: int, context: str = "") -> None:
    if status == _n.OK:
        return
    cls = _EXC.get(status, FuseError)
    msg = _n.strerror(status).decode()
    raise cls(status, f"{context}: {msg}" if context else msg)


def _timeout_ms(timeout: Optional[float]) -> int:
    """Seconds (or None for 'wait forever') to the C API's millisecond int."""
    if timeout is None:
        return -1
    if timeout < 0:
        raise ValueError("timeout must be >= 0, or None to wait indefinitely")
    return int(timeout * 1000)


def _as_bytes(data: Data) -> bytes:
    if isinstance(data, str):
        return data.encode()
    if isinstance(data, (bytes, bytearray, memoryview)):
        return bytes(data)
    raise TypeError(f"expected bytes-like or str, got {type(data).__name__}")


# --- module-level helpers -------------------------------------------------


def encryption_available() -> bool:
    """True if this build can encrypt. When False, passing ``key=`` raises."""
    return bool(_n.encryption_available())


def max_message() -> int:
    """Largest message a single send/recv can carry, in bytes."""
    return int(_n.max_message())


class Stats(NamedTuple):
    messages_sent: int
    messages_received: int
    bytes_sent: int
    bytes_received: int
    retransmits: int
    auth_failures: int
    rtt_us: int


# --- connection -----------------------------------------------------------


class Connection:
    """An established connection. Create one with :func:`connect` or
    :meth:`Listener.accept` — not directly."""

    __slots__ = ("_h", "_peer")

    def __init__(self, handle: int):
        self._h = handle
        self._peer = None

    # -- data --

    def send(self, data: Data) -> None:
        """Send one message. Returns once the peer has acknowledged it.

        ``str`` is encoded as UTF-8 for convenience; everything else must be
        bytes-like.
        """
        self._ensure_open()
        payload = _as_bytes(data)
        buf = ctypes.c_char_p(payload) if payload else None
        _check(_n.send(self._h, buf, len(payload)), "send")

    def recv(self, timeout: Optional[float] = None) -> bytes:
        """Receive one whole message.

        ``timeout`` is in seconds; ``None`` waits indefinitely and ``0``
        polls. Raises :class:`Timeout` or :class:`ConnectionClosed`.
        """
        self._ensure_open()
        ms = _timeout_ms(timeout)
        need = ctypes.c_size_t(0)

        # Most messages fit the first buffer; if not, the C API reports the
        # size required and keeps the message queued, so the retry is exact.
        cap = 65536
        buf = ctypes.create_string_buffer(cap)
        status = _n.recv(self._h, buf, cap, ctypes.byref(need), ms)
        if status == _n.ERR_BUFFER:
            cap = need.value
            buf = ctypes.create_string_buffer(cap)
            status = _n.recv(self._h, buf, cap, ctypes.byref(need), ms)
        _check(status, "recv")
        return buf.raw[: need.value]

    # -- state --

    @property
    def peer(self) -> tuple[str, int]:
        """``(address, port)`` of the far end."""
        if self._peer is None:
            self._ensure_open()
            addr = ctypes.create_string_buffer(64)
            port = ctypes.c_uint16(0)
            _check(_n.conn_peer(self._h, addr, 64, ctypes.byref(port)), "peer")
            self._peer = (addr.value.decode(), int(port.value))
        return self._peer

    @property
    def closed(self) -> bool:
        return self._h is None or bool(_n.conn_is_closed(self._h))

    @property
    def stats(self) -> Stats:
        """Counters for this connection, useful for logging and debugging."""
        self._ensure_open()
        s = _n.ConnStats()
        _n.conn_get_stats(self._h, ctypes.byref(s))
        return Stats(
            s.messages_sent,
            s.messages_received,
            s.bytes_sent,
            s.bytes_received,
            s.retransmits,
            s.auth_failures,
            s.rtt_us,
        )

    def close(self) -> None:
        """Tell the peer we are done and release the connection. Idempotent."""
        if self._h is not None:
            _n.close(self._h)
            self._h = None

    # -- ergonomics --

    def _ensure_open(self) -> None:
        if self._h is None:
            raise ConnectionClosed(_n.ERR_CLOSED, "connection is already closed")

    def __iter__(self) -> Iterator[bytes]:
        """Iterate messages until the peer closes::

        for message in conn:
            handle(message)
        """
        while True:
            try:
                yield self.recv()
            except ConnectionClosed:
                return

    def __enter__(self) -> "Connection":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __repr__(self) -> str:
        if self._h is None:
            return "<fuse.Connection closed>"
        try:
            host, port = self.peer
            return f"<fuse.Connection peer={host}:{port}>"
        except Exception:
            return "<fuse.Connection>"


# --- listener -------------------------------------------------------------


class Listener:
    """A bound server socket. Create one with :func:`listen`."""

    __slots__ = ("_h",)

    def __init__(self, handle: int):
        self._h = handle

    @property
    def port(self) -> int:
        """The port actually bound — useful when you asked for port 0."""
        self._ensure_open()
        return int(_n.listener_port(self._h))

    def accept(self, timeout: Optional[float] = None) -> Connection:
        """Wait for a client and return its :class:`Connection`.

        Each accepted connection is independent, so the usual pattern of
        accepting in a loop and handing each connection to a thread works.
        """
        self._ensure_open()
        err = ctypes.c_int(0)
        handle = _n.accept(self._h, _timeout_ms(timeout), ctypes.byref(err))
        if not handle:
            _check(err.value or _n.ERR_INTERNAL, "accept")
        return Connection(handle)

    def close(self) -> None:
        if self._h is not None:
            _n.listener_close(self._h)
            self._h = None

    def _ensure_open(self) -> None:
        if self._h is None:
            raise ConnectionClosed(_n.ERR_CLOSED, "listener is already closed")

    def __iter__(self) -> Iterator[Connection]:
        """Iterate incoming connections::

        for conn in listener:
            threading.Thread(target=handle, args=(conn,)).start()
        """
        while True:
            yield self.accept()

    def __enter__(self) -> "Listener":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __repr__(self) -> str:
        if self._h is None:
            return "<fuse.Listener closed>"
        return f"<fuse.Listener port={self.port}>"


# --- entry points ---------------------------------------------------------


def _build_config(
    *,
    bind: Optional[str],
    host: Optional[str],
    port: int,
    key: Optional[str],
    timeout: Optional[float],
) -> _n.Config:
    cfg = _n.Config()
    _n.config_init(ctypes.byref(cfg))
    if bind is not None:
        cfg.bind_address = bind.encode()
    if host is not None:
        cfg.host = host.encode()
    cfg.port = port
    if key:
        if not encryption_available():
            raise ConfigError(
                _n.ERR_UNSUPPORTED,
                "this build has no crypto backend, so key= cannot be honoured "
                "(refusing to fall back to plaintext)",
            )
        cfg.pre_shared_key = key.encode()
    if timeout is not None:
        cfg.timeout_ms = int(timeout * 1000)
    return cfg


def listen(
    port: int,
    bind: str = "0.0.0.0",
    key: Optional[str] = None,
    timeout: Optional[float] = None,
) -> Listener:
    """Bind ``port`` and return a :class:`Listener`.

    Only this one UDP port needs to be reachable: each accepted connection
    moves to its own ephemeral port automatically.

    Pass ``port=0`` to let the OS choose, then read :attr:`Listener.port`.
    """
    cfg = _build_config(bind=bind, host=None, port=port, key=key, timeout=timeout)
    err = ctypes.c_int(0)
    handle = _n.listen(ctypes.byref(cfg), ctypes.byref(err))
    if not handle:
        _check(err.value or _n.ERR_INTERNAL, f"listen on {bind}:{port}")
    return Listener(handle)


def connect(
    host: str,
    port: int,
    key: Optional[str] = None,
    timeout: Optional[float] = None,
) -> Connection:
    """Connect to ``host:port`` and return a :class:`Connection`.

    Raises :class:`Timeout` if nothing answers, or :class:`AuthError` if the
    peer cannot prove it holds the same ``key``.
    """
    cfg = _build_config(bind=None, host=host, port=port, key=key, timeout=timeout)
    err = ctypes.c_int(0)
    handle = _n.connect(ctypes.byref(cfg), ctypes.byref(err))
    if not handle:
        _check(err.value or _n.ERR_INTERNAL, f"connect to {host}:{port}")
    return Connection(handle)
