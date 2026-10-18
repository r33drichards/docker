#!/usr/bin/env python3
"""Exercise ECHO against a running server using only the Python standard library."""

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
        self.send(f"USER {nick} 0 * :Echo test")
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
                    raise AssertionError("Server closed the connection")
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
    parser.add_argument("--expect-unloaded", action="store_true")
    args = parser.parse_args()
    caller = Client(args.host, args.port, "echocaller")
    observer = None
    try:
        if args.expect_unloaded:
            caller.send("ECHO :not enabled")
            caller.until(lambda line: " 421 echocaller ECHO " in line)
            print("PASS: ECHO is opt-in")
            return

        observer = Client(args.host, args.port, "echoobserver")
        caller.barrier("caller-ready")
        observer.barrier("observer-ready")
        cases = [
            ("ECHO hello", "hello"),
            ("ECHO one two three", "one two three"),
            ("ECHO :two  spaces :and a colon", "two  spaces :and a colon"),
            ("echo :lowercase command", "lowercase command"),
            ("ECHO :literal %s {braces} $(id); /OPER password", "literal %s {braces} $(id); /OPER password"),
            ("ECHO :Unicode café ☕", "Unicode café ☕"),
        ]
        for index, (command, expected) in enumerate(cases):
            caller.send(command)
            lines = caller.barrier(f"echo-{index}")
            notices = [line for line in lines if " NOTICE echocaller :" in line]
            assert len(notices) == 1, (command, lines)
            assert notices[0].split(" NOTICE echocaller :", 1)[1] == expected, notices

        for command in ("ECHO", "ECHO :"):
            caller.send(command)
            lines = caller.barrier("missing-argument")
            assert any(" 461 echocaller ECHO " in line for line in lines), lines
            assert not any(" NOTICE echocaller :" in line for line in lines), lines

        # The caller's barriers ensure ECHO processing has completed before this
        # observer barrier. A broadcast would therefore appear in these lines.
        lines = observer.barrier("observer-finished")
        assert not any(" NOTICE " in line or " PRIVMSG " in line for line in lines), lines
        print("PASS: ECHO arguments, literal text, missing text, and caller-only output")
    finally:
        caller.close()
        if observer is not None:
            observer.close()


if __name__ == "__main__":
    main()
