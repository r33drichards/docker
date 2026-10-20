#!/usr/bin/env python3
"""Build exact-rule tests and verify injected headers on isolated loopback requests.

All credentials are synthetic. The fixture-only compiler switch does not relax
production fetch policy; a separate production-mode binary checks that boundary.
"""

import http.server
import json
import os
import pathlib
import shlex
import subprocess
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parent.parent


class Receiver:
    def __init__(self):
        self.requests = []
        self.condition = threading.Condition()
        self.release = threading.Event()
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.receive()

            def do_POST(self):
                self.receive()

            def receive(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                headers = {key.lower(): value for key, value in self.headers.items()}
                with owner.condition:
                    owner.requests.append((self.command, self.path, body, headers))
                    owner.condition.notify_all()
                if self.path.startswith("/wait/"):
                    target = self.path.removeprefix("/wait")
                    with owner.condition:
                        ready = owner.condition.wait_for(
                            lambda: any(item[1] == target for item in owner.requests), 5
                        )
                    status, response = (200, b"ready") if ready else (504, b"not observed")
                else:
                    if self.path == "/owner-control":
                        with owner.condition:
                            owner.condition.wait_for(
                                lambda: any(item[1] == "/release-owner" for item in owner.requests),
                                5,
                            )
                    if self.path in ("/cancel", "/retired"):
                        owner.release.wait(15)
                    status = (
                        302 if self.path == "/redirect" else 503 if self.path == "/failure" else 200
                    )
                    response = body or b"ok"
                try:
                    self.send_response(status)
                    if self.path == "/redirect":
                        self.send_header("Location", "/must-not-follow")
                    self.send_header("Content-Length", str(len(response)))
                    self.end_headers()
                    self.wfile.write(response)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # Cancellation can close an already transmitted request.

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def verify(self):
        paths = [item[1] for item in self.requests]
        expected = {
            "/post": 3,
            "/post/child": 1,
            "/post?extra=1": 1,
            "/redirect": 1,
            "/cancel": 1,
            "/wait/cancel": 1,
            "/retired": 1,
            "/wait/retired": 1,
            "/discard": 1,
            "/failure": 1,
            "/legacy": 2,
            "/owner-control": 1,
            "/wait/owner-control": 1,
            "/release-owner": 1,
        }
        assert set(paths) == set(expected), paths  # No redirect or cancelled-before-commit I/O.
        assert {path: paths.count(path) for path in paths} == expected, paths
        for method, path, body, headers in self.requests:
            if path.startswith("/wait/") or path == "/release-owner":
                assert method == "GET"
                token = None
            elif (
                path in ("/post/child", "/post?extra=1", "/owner-control")
                or (path == "/post" and body == b"{}")
                or (path == "/legacy" and body == b'{"wrongscript":true}')
            ):
                assert method == "POST"
                token = None
            else:
                assert method == "POST"
                token = (
                    "synthetic-rotated-token"
                    if body == b'{"rotated":true}'
                    else "synthetic-owner-token"
                )
            assert headers.get("xc-token") == token, (path, headers)
            assert headers.get("authorization") == (
                "Bearer synthetic-test-only" if token else None
            ), (path, headers)
            assert "attempted-js-override" not in headers.values(), (path, headers)
            assert "mutated-after-stage" not in headers.values(), (path, headers)
            assert headers.get("x-legacy") == ("synthetic-legacy" if path == "/legacy" else None), (
                path,
                headers,
            )
            if b"attempted-js-override" in body:
                assert headers["content-type"] == "application/json"
                assert json.loads(body)["headers"]["xc-token"] == "attempted-js-override"


def main():
    cflags = shlex.split(os.environ.get("CURL_CXXFLAGS", ""))
    ldflags = shlex.split(os.environ.get("CURL_LDFLAGS", ""))
    if not cflags and not ldflags:
        cflags = shlex.split(
            subprocess.check_output(["pkg-config", "--cflags", "libcurl"], text=True)
        )
        ldflags = shlex.split(
            subprocess.check_output(["pkg-config", "--libs", "libcurl"], text=True)
        )
    with tempfile.TemporaryDirectory(prefix="js-fetch-headers-") as directory:
        binaries = []
        for fixture in (False, True):
            binary = pathlib.Path(directory) / ("fixture" if fixture else "production")
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
                    *(["-DJS_ASYNC_HTTP_TEST_LOOPBACK"] if fixture else []),
                    str(ROOT / "tests/js_fetch_headers.cpp"),
                    *ldflags,
                    "-pthread",
                    "-o",
                    str(binary),
                ],
                check=True,
                timeout=120,
            )
            binaries.append(binary)
        subprocess.run([str(binaries[0])], check=True, timeout=10)
        receiver = Receiver()
        try:
            # A bogus proxy must not intercept a host-injected credential.
            env = dict(
                os.environ,
                http_proxy="http://127.0.0.1:1",
                https_proxy="http://127.0.0.1:1",
                ALL_PROXY="http://127.0.0.1:1",
                NO_PROXY="",
                no_proxy="",
            )
            subprocess.run([str(binaries[1]), receiver.url], check=True, timeout=25, env=env)
            receiver.verify()
            print(
                "PASS: loopback verified exact-scope POST injection, no override, redirect or proxy leakage"
            )
        finally:
            receiver.close()


if __name__ == "__main__":
    main()
