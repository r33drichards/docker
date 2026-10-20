#!/usr/bin/env python3
"""Compile/run bounded transport against owned loopback HTTP and bad-TLS fixtures."""

import http.server
import json
import os
import pathlib
import shlex
import ssl
import subprocess
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


class Receiver:
    def __init__(self, tls=None):
        self.posts = []
        self.condition = threading.Condition()
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def handle(self):
                try:
                    super().handle()
                except (BrokenPipeError, ConnectionResetError):
                    pass  # Expected when curl rejects the synthetic certificate.

            def log_message(self, *args):
                pass

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                with owner.condition:
                    owner.posts.append((self.path, body, dict(self.headers)))
                    owner.condition.notify_all()
                if self.path == "/slow":
                    time.sleep(3)
                self.send_response(
                    302 if self.path == "/redirect" else 503 if self.path == "/failure" else 200
                )
                if self.path == "/redirect":
                    self.send_header("Location", "/must-not-follow")
                self.end_headers()
                try:
                    self.wfile.write(b"x" * 70000 if self.path == "/large" else b"ok")
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        if tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(*tls)
            self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.url = f"{'https' if tls else 'http'}://127.0.0.1:{self.server.server_port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def wait(self, count, timeout=5):
        with self.condition:
            assert self.condition.wait_for(lambda: len(self.posts) >= count, timeout), self.posts
            return list(self.posts)


def main():
    with tempfile.TemporaryDirectory(prefix="js-webhooks-") as directory:
        directory = pathlib.Path(directory)
        binary = directory / "webhooks"
        cflags = shlex.split(os.environ.get("CURL_CXXFLAGS", ""))
        ldflags = shlex.split(os.environ.get("CURL_LDFLAGS", ""))
        if not cflags and not ldflags:
            cflags = shlex.split(
                subprocess.check_output(["pkg-config", "--cflags", "libcurl"], text=True)
            )
            ldflags = shlex.split(
                subprocess.check_output(["pkg-config", "--libs", "libcurl"], text=True)
            )
        subprocess.run(
            [
                os.environ.get("CXX", "g++"),
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                *shlex.split(os.environ.get("WEBHOOK_TEST_CXXFLAGS", "")),
                *cflags,
                str(ROOT / "tests/js_webhooks.cpp"),
                *ldflags,
                "-pthread",
                "-o",
                str(binary),
            ],
            check=True,
            timeout=120,
        )
        cert, key = directory / "certificate.pem", directory / "key.pem"
        subprocess.run(
            [
                os.environ.get("OPENSSL", "openssl"),
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-keyout",
                str(key),
                "-out",
                str(cert),
                "-days",
                "1",
                "-subj",
                "/CN=untrusted.invalid",
            ],
            check=True,
            timeout=15,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        receiver, tls = Receiver(), Receiver((str(cert), str(key)))
        try:
            # A bogus proxy must not intercept even loopback (NO_PROXY is also cleared).
            env = dict(
                os.environ,
                http_proxy="http://127.0.0.1:1",
                https_proxy="http://127.0.0.1:1",
                ALL_PROXY="http://127.0.0.1:1",
                NO_PROXY="",
                no_proxy="",
            )
            subprocess.run(
                [str(binary), receiver.url, tls.url + "/tls", str(directory)],
                check=True,
                timeout=20,
                env=env,
            )
            paths = [post[0] for post in receiver.posts]
            assert paths.count("/ok") == 1 and paths.count("/redirect") == 1, paths
            assert "/must-not-follow" not in paths and tls.posts == [], (paths, tls.posts)
            for name, body, headers in receiver.posts:
                headers = {key.lower(): value for key, value in headers.items()}
                assert headers["content-type"] == "application/json"
                assert headers.get("xc-token") == ("synthetic-token" if name == "/ok" else None)
                assert headers.get("authorization") == (
                    "synthetic-token" if name == "/redirect" else None
                )
                assert json.loads(body) == (
                    {} if body == b"{}" else {"event": "transport.test", "nick": "λ"}
                )
            print(
                "PASS: loopback receiver verified JSON, no retry/redirect/proxy and no unverified TLS POST"
            )
        finally:
            receiver.close()
            tls.close()


if __name__ == "__main__":
    main()
