#!/usr/bin/env python3
"""Deterministic registration-readiness tests; no daemon, network, or sleeps."""

import unittest
from unittest import mock

import precommand_client


class Clock:
    def __init__(self):
        self.now = 0
        self.sleeps = []

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


class RegistrationTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.monotonic = mock.patch.object(
            precommand_client.time, "monotonic", self.clock.monotonic
        )
        self.sleep = mock.patch.object(precommand_client.time, "sleep", self.clock.sleep)
        self.monotonic.start()
        self.sleep.start()
        self.addCleanup(self.monotonic.stop)
        self.addCleanup(self.sleep.stop)

    def connected_socket(self, *responses):
        sock = mock.Mock()
        sock.recv.side_effect = responses
        return sock

    def test_registration_reset_retries_and_discards_partial_buffer(self):
        failed = self.connected_socket(b":old partial", ConnectionResetError("not ready"))
        ready = self.connected_socket(b":server 001 nick :Welcome\r\n")
        with mock.patch.object(
            precommand_client.socket, "create_connection", side_effect=[failed, ready]
        ) as connect:
            client = precommand_client.Client("localhost", 6667, "nick")
        self.addCleanup(client.close)
        self.assertEqual(connect.call_count, 2)
        failed.close.assert_called_once_with()
        ready.close.assert_not_called()
        self.assertIs(client.sock, ready)
        self.assertEqual(client.buffer, b"")
        for sock in (failed, ready):
            self.assertEqual(
                sock.sendall.call_args_list,
                [mock.call(b"NICK nick\r\n"), mock.call(b"USER nick 0 * :Hook test\r\n")],
            )
        self.assertEqual(self.clock.sleeps, [0.2])

    def test_send_reset_retries_full_registration(self):
        failed = self.connected_socket()
        failed.sendall.side_effect = BrokenPipeError("proxy closed")
        ready = self.connected_socket(b":server 001 nick :Welcome\r\n")
        with mock.patch.object(
            precommand_client.socket, "create_connection", side_effect=[failed, ready]
        ) as connect:
            client = precommand_client.Client("localhost", 6667, "nick")
        self.addCleanup(client.close)
        self.assertEqual(connect.call_count, 2)
        failed.close.assert_called_once_with()
        self.assertEqual(ready.sendall.call_count, 2)

    def test_registration_timeouts_exhaust_shared_90_second_budget(self):
        sockets = []

        def connect(address, timeout):
            self.assertEqual(address, ("localhost", 6667))
            self.assertLessEqual(timeout, min(1, 90 - self.clock.now))
            sock = mock.Mock()

            def receive(_size):
                self.clock.now += sock.settimeout.call_args.args[0]
                raise TimeoutError("registration not ready")

            sock.recv.side_effect = receive
            sockets.append(sock)
            return sock

        with mock.patch.object(precommand_client.socket, "create_connection", connect):
            with self.assertRaisesRegex(TimeoutError, "registration not ready"):
                precommand_client.Client("localhost", 6667, "nick")
        self.assertAlmostEqual(self.clock.now, 90)
        self.assertEqual(len(sockets), 6)
        self.assertEqual(len(self.clock.sleeps), 5)
        for sock in sockets:
            sock.close.assert_called_once_with()

    def test_connection_refusal_retries_within_same_deadline(self):
        ready = self.connected_socket(b":server 001 nick :Welcome\r\n")
        with mock.patch.object(
            precommand_client.socket,
            "create_connection",
            side_effect=[ConnectionRefusedError("not listening"), ready],
        ) as connect:
            client = precommand_client.Client("localhost", 6667, "nick")
        self.addCleanup(client.close)
        self.assertEqual(connect.call_count, 2)
        self.assertEqual(self.clock.sleeps, [0.2])

    def test_registration_assertion_is_not_retried_and_socket_is_closed(self):
        failed = self.connected_socket(b"")
        with mock.patch.object(
            precommand_client.socket, "create_connection", return_value=failed
        ) as connect:
            with self.assertRaisesRegex(AssertionError, "Server closed the connection"):
                precommand_client.Client("localhost", 6667, "nick")
        connect.assert_called_once()
        failed.close.assert_called_once_with()
        self.assertEqual(self.clock.sleeps, [])

    def test_invalid_protocol_bytes_are_not_retried(self):
        failed = self.connected_socket(b"\xff\r\n")
        with mock.patch.object(
            precommand_client.socket, "create_connection", return_value=failed
        ) as connect:
            with self.assertRaises(UnicodeDecodeError):
                precommand_client.Client("localhost", 6667, "nick")
        connect.assert_called_once()
        failed.close.assert_called_once_with()
        self.assertEqual(self.clock.sleeps, [])

    def test_connection_reset_after_registration_is_not_retried(self):
        ready = self.connected_socket(
            b":server 001 nick :Welcome\r\n", ConnectionResetError("real failure")
        )
        with mock.patch.object(
            precommand_client.socket, "create_connection", return_value=ready
        ) as connect:
            client = precommand_client.Client("localhost", 6667, "nick")
            self.addCleanup(client.close)
            with self.assertRaisesRegex(ConnectionResetError, "real failure"):
                client.barrier("check")
        connect.assert_called_once()
        self.assertEqual(self.clock.sleeps, [])


if __name__ == "__main__":
    unittest.main()
