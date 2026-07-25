"""Behavioural tests for the Python bindings.

Run with:  FUSE_LIBRARY=build/default/libfuse_proto.so python3 -m pytest python/tests
or plainly: python3 python/tests/test_sdk.py   (no pytest needed)
"""
import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import fuse  # noqa: E402


def _serve(listener, out, count=1):
    """Accept `count` connections and stash them for the test to use."""
    for _ in range(count):
        try:
            out.append(listener.accept(timeout=5))
        except fuse.FuseError:
            return


class SdkTest(unittest.TestCase):
    port = 39000

    def _next_port(self):
        SdkTest.port += 4
        return SdkTest.port

    def _pair(self, key=None):
        """A connected (client, server) pair on a fresh port."""
        listener = fuse.listen(port=self._next_port(), bind="127.0.0.1", key=key)
        served = []
        t = threading.Thread(target=_serve, args=(listener, served))
        t.start()
        client = fuse.connect("127.0.0.1", listener.port, key=key, timeout=5)
        t.join()
        self.assertEqual(len(served), 1, "server did not accept the connection")
        return client, served[0], listener

    def test_version_and_capabilities(self):
        self.assertTrue(fuse.__version__)
        self.assertGreater(fuse.max_message(), 0)
        self.assertIn(fuse.encryption_available(), (True, False))

    def test_round_trip(self):
        cli, srv, l = self._pair()
        with cli, srv, l:
            cli.send(b"hello")
            self.assertEqual(srv.recv(timeout=5), b"hello")

    def test_str_is_encoded(self):
        cli, srv, l = self._pair()
        with cli, srv, l:
            cli.send("héllo")
            self.assertEqual(srv.recv(timeout=5).decode(), "héllo")

    def test_message_boundaries_preserved(self):
        cli, srv, l = self._pair()
        with cli, srv, l:
            for m in (b"a", b"bb", b"ccc"):
                cli.send(m)
            self.assertEqual(srv.recv(timeout=5), b"a")
            self.assertEqual(srv.recv(timeout=5), b"bb")
            self.assertEqual(srv.recv(timeout=5), b"ccc")

    def test_bidirectional(self):
        cli, srv, l = self._pair()
        with cli, srv, l:
            cli.send(b"ping")
            self.assertEqual(srv.recv(timeout=5), b"ping")
            srv.send(b"pong")
            self.assertEqual(cli.recv(timeout=5), b"pong")

    def test_large_message_beyond_one_buffer(self):
        # Bigger than the binding's initial 64 KiB read buffer, so this also
        # exercises the size-report-and-retry path.
        payload = bytes((i * 7) % 251 for i in range(200_000))
        cli, srv, l = self._pair()
        with cli, srv, l:
            t = threading.Thread(target=cli.send, args=(payload,))
            t.start()
            got = srv.recv(timeout=15)
            t.join()
            self.assertEqual(got, payload)

    def test_empty_message(self):
        cli, srv, l = self._pair()
        with cli, srv, l:
            cli.send(b"")
            self.assertEqual(srv.recv(timeout=5), b"")

    def test_recv_timeout(self):
        cli, srv, l = self._pair()
        with cli, srv, l:
            start = time.monotonic()
            with self.assertRaises(fuse.Timeout):
                srv.recv(timeout=0.3)
            self.assertGreaterEqual(time.monotonic() - start, 0.25)

    def test_close_notifies_peer(self):
        cli, srv, l = self._pair()
        with l:
            cli.close()
            with self.assertRaises(fuse.ConnectionClosed):
                srv.recv(timeout=3)
            self.assertTrue(srv.closed)
            srv.close()

    def test_iteration_ends_when_peer_closes(self):
        cli, srv, l = self._pair()
        with l:
            for m in (b"1", b"2", b"3"):
                cli.send(m)
            cli.close()
            self.assertEqual(list(srv), [b"1", b"2", b"3"])
            srv.close()

    def test_peer_and_stats(self):
        cli, srv, l = self._pair()
        with cli, srv, l:
            host, port = srv.peer
            self.assertEqual(host, "127.0.0.1")
            self.assertNotEqual(port, 0)

            cli.send(b"counted")
            srv.recv(timeout=5)
            self.assertEqual(cli.stats.messages_sent, 1)
            self.assertEqual(srv.stats.messages_received, 1)

    def test_context_manager_closes(self):
        listener = fuse.listen(port=self._next_port(), bind="127.0.0.1")
        served = []
        t = threading.Thread(target=_serve, args=(listener, served))
        t.start()
        with fuse.connect("127.0.0.1", listener.port, timeout=5) as c:
            self.assertFalse(c.closed)
        self.assertTrue(c.closed)
        t.join()
        for s in served:
            s.close()
        listener.close()

    def test_connect_refused_when_nobody_listening(self):
        with self.assertRaises(fuse.Timeout):
            fuse.connect("127.0.0.1", 1, timeout=0.5)

    def test_use_after_close_raises(self):
        cli, srv, l = self._pair()
        with l:
            srv.close()
            cli.close()
            with self.assertRaises(fuse.ConnectionClosed):
                cli.send(b"nope")

    @unittest.skipUnless(fuse.encryption_available(), "build has no crypto backend")
    def test_encrypted_round_trip(self):
        cli, srv, l = self._pair(key="a-shared-secret")
        with cli, srv, l:
            payload = bytes(range(256)) * 40
            t = threading.Thread(target=cli.send, args=(payload,))
            t.start()
            self.assertEqual(srv.recv(timeout=10), payload)
            t.join()
            self.assertEqual(srv.stats.auth_failures, 0)

    @unittest.skipUnless(fuse.encryption_available(), "build has no crypto backend")
    def test_wrong_key_is_rejected(self):
        listener = fuse.listen(port=self._next_port(), bind="127.0.0.1", key="right")
        served = []
        t = threading.Thread(target=_serve, args=(listener, served))
        t.start()
        with self.assertRaises(fuse.AuthError):
            fuse.connect("127.0.0.1", listener.port, key="wrong", timeout=3)
        t.join()
        for s in served:
            s.close()
        listener.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
