"""ctypes binding to libfuse_proto's C ABI (`fuse/sdk.h`).

Kept separate from the friendly API in ``__init__`` so that everything
touching raw pointers lives in one place.

Loading order for the shared library, first hit wins:

1. ``$FUSE_LIBRARY`` — an explicit path, for unusual installs or testing
   against a build tree.
2. Alongside this package (how a self-contained wheel would ship it).
3. The normal loader search path, which covers a system install.
4. A few well-known prefixes, so an install into ``/usr/local`` or
   ``/opt/fuse`` works even when it is not in the loader's cache.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys
from pathlib import Path

_LIB_NAMES = ("libfuse_proto.so.0", "libfuse_proto.so")

_SEARCH_PREFIXES = (
    "/usr/local/lib64",
    "/usr/local/lib",
    "/usr/lib64",
    "/usr/lib",
    "/opt/fuse/lib64",
    "/opt/fuse/lib",
)


def _candidates():
    explicit = os.environ.get("FUSE_LIBRARY")
    if explicit:
        yield explicit

    here = Path(__file__).resolve().parent
    for name in _LIB_NAMES:
        yield str(here / name)
        yield str(here / "lib" / name)

    for name in _LIB_NAMES:
        yield name

    found = ctypes.util.find_library("fuse_proto")
    if found:
        yield found

    for prefix in _SEARCH_PREFIXES:
        for name in _LIB_NAMES:
            yield str(Path(prefix) / name)


def _load():
    tried = []
    for cand in _candidates():
        try:
            # RTLD_GLOBAL so libwolfssl, if it is loaded as a dependency,
            # resolves consistently.
            return ctypes.CDLL(cand, mode=ctypes.RTLD_GLOBAL)
        except OSError as exc:  # keep looking, but remember why
            tried.append(f"  {cand}: {exc}")
    raise ImportError(
        "could not load the Fuse shared library (libfuse_proto.so).\n"
        "Install the SDK, or point FUSE_LIBRARY at the library:\n\n"
        "    curl -fsSL https://github.com/kamalkoushikd/fuse-nw/releases/latest/"
        "download/install.sh | sh\n"
        "    # or, against a build tree:\n"
        "    export FUSE_LIBRARY=/path/to/build/libfuse_proto.so\n\n"
        "Tried:\n" + "\n".join(tried)
    )


lib = _load()

# --- status codes (mirror fuse_status in sdk.h) ---------------------------

OK = 0
ERR_CONFIG = -1
ERR_SOCKET = -2
ERR_TIMEOUT = -3
ERR_CLOSED = -4
ERR_AUTH = -5
ERR_TOO_LARGE = -6
ERR_BUFFER = -7
ERR_UNSUPPORTED = -8
ERR_INTERNAL = -9


class Config(ctypes.Structure):
    _fields_ = [
        ("bind_address", ctypes.c_char_p),
        ("host", ctypes.c_char_p),
        ("port", ctypes.c_uint16),
        ("pre_shared_key", ctypes.c_char_p),
        ("timeout_ms", ctypes.c_uint32),
    ]


class ConnStats(ctypes.Structure):
    _fields_ = [
        ("messages_sent", ctypes.c_uint64),
        ("messages_received", ctypes.c_uint64),
        ("bytes_sent", ctypes.c_uint64),
        ("bytes_received", ctypes.c_uint64),
        ("retransmits", ctypes.c_uint64),
        ("auth_failures", ctypes.c_uint64),
        ("rtt_us", ctypes.c_uint64),
    ]


_c_status = ctypes.c_int
_p_listener = ctypes.c_void_p
_p_conn = ctypes.c_void_p


def _bind(name, restype, argtypes):
    fn = getattr(lib, name)
    fn.restype = restype
    fn.argtypes = argtypes
    return fn


strerror = _bind("fuse_strerror", ctypes.c_char_p, [_c_status])
version = _bind("fuse_version", ctypes.c_char_p, [])
encryption_available = _bind("fuse_encryption_available", ctypes.c_int, [])
max_message = _bind("fuse_max_message", ctypes.c_size_t, [])
config_init = _bind("fuse_config_init", None, [ctypes.POINTER(Config)])

listen = _bind(
    "fuse_listen", _p_listener, [ctypes.POINTER(Config), ctypes.POINTER(_c_status)]
)
accept = _bind(
    "fuse_accept", _p_conn, [_p_listener, ctypes.c_int, ctypes.POINTER(_c_status)]
)
listener_port = _bind("fuse_listener_port", ctypes.c_uint16, [_p_listener])
listener_close = _bind("fuse_listener_close", None, [_p_listener])

connect = _bind(
    "fuse_connect", _p_conn, [ctypes.POINTER(Config), ctypes.POINTER(_c_status)]
)
send = _bind("fuse_send", _c_status, [_p_conn, ctypes.c_void_p, ctypes.c_size_t])
recv = _bind(
    "fuse_recv",
    _c_status,
    [
        _p_conn,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.c_int,
    ],
)
conn_peer = _bind(
    "fuse_conn_peer",
    _c_status,
    [_p_conn, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint16)],
)
conn_is_closed = _bind("fuse_conn_is_closed", ctypes.c_int, [_p_conn])
conn_get_stats = _bind(
    "fuse_conn_get_stats", None, [_p_conn, ctypes.POINTER(ConnStats)]
)
close = _bind("fuse_close", None, [_p_conn])
