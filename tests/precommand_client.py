#!/usr/bin/env python3
"""Exercise the pre-command hook against a running server using only the Python standard library."""

import argparse
import socket
import time


class Client:
    def __init__(self, host, port, nick):
        deadline = time.monotonic() + 90
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=1)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.2)
        self.nick = nick
        self.buffer = b""
        self.send(f"NICK {nick}")
        self.send(f"USER {nick} 0 * :Hook test")
        self.until(lambda line: f" 001 {nick} " in line)

    def send(self, line):
        self.sock.sendall((line + "\r\n").encode("utf-8"))

    def until(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        lines = []
        while time.monotonic() < deadline:
            if b"\n" not in self.buffer:
                self.sock.settimeout(max(0.01, deadline - time.monotonic()))
                data = self.sock.recv(4096)
                if not data:
                    raise AssertionError(f"Server closed the connection; saw {lines!r}")
                self.buffer += data
                continue
            raw, self.buffer = self.buffer.split(b"\n", 1)
            line = raw.rstrip(b"\r").decode("utf-8")
            lines.append(line)
            if line.startswith("PING "):
                self.send("PONG " + line[5:])
            if predicate(line):
                return lines
        raise AssertionError(f"Response not received; saw {lines!r}")

    def barrier(self, token):
        self.send(f"PING :{token}")
        return self.until(lambda line: " PONG " in line and line.endswith(token))

    def close(self):
        self.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    args = parser.parse_args()
    caller = Client(args.host, args.port, "hookcaller")
    observer = None
    try:
        observer = Client(args.host, args.port, "hookobserver")
        caller.barrier("hook-start-secret")
        observer.barrier("observer-ready-secret")
        caller.send("JOIN #hooktest")
        caller.barrier("after-join-secret")
        caller.send("MODE #hooktest +k channel-key-secret")
        caller.barrier("after-mode-secret")
        observer.send("JOIN #hooktest channel-key-secret")
        observer.until(lambda line: " JOIN " in line and "#hooktest" in line)

        caller.send("PRIVMSG hookobserver :private-message-secret two  spaces café")
        lines = observer.until(lambda line: " PRIVMSG hookobserver :" in line)
        assert any(line.endswith(":private-message-secret two  spaces café") for line in lines), (
            lines
        )

        # Failed handlers still pass core validation. All values here are synthetic.
        caller.send("OPER operator-name-secret operator-password-secret")
        caller.send("PASS connection-password-secret")
        caller.send("AUTHENTICATE sasl-payload-secret")
        caller.send(
            "WEBIRC gateway-password-secret gateway-user-secret gateway-host-secret 192.0.2.1"
        )
        caller.send("PRIVMSG NickServ :IDENTIFY service-password-secret")
        caller.send("VERSION")
        caller.send("DIE :admin-password-secret")  # Operator-only; rejected before validation.
        caller.send("@+trace=message-tag-secret PING :tag-ping-secret")
        caller.send("PING")  # Missing required argument: no validated-stage record.
        caller.send("UNKNOWNSECRET unknown-argument-secret")
        lines = caller.barrier("hook-finish-secret")
        assert any(" 461 hookcaller PING " in line for line in lines), lines
        assert any(" 421 hookcaller UNKNOWNSECRET " in line for line in lines), lines
        assert any(" 481 hookcaller " in line for line in lines), lines
        assert any(" 351 hookcaller " in line for line in lines), lines
        assert any(" PONG " in line and line.endswith("hook-finish-secret") for line in lines), (
            lines
        )
        assert not any(" NOTICE " in line for line in lines), lines
        print("PASS: registration, PING, JOIN, MODE, PRIVMSG, and error behavior unchanged")
    finally:
        caller.close()
        if observer is not None:
            observer.close()


if __name__ == "__main__":
    main()
