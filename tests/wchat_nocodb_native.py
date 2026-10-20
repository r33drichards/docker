"""Owned-loopback test for the opt-in NocoDB adapter; no real NocoDB calls."""

import json
import pathlib
import time


def exercise_wchat_nocodb(caller, other, reload, receiver, directory):
    root = pathlib.Path(__file__).resolve().parent.parent
    secret = directory / "adapter-secret"
    secret.write_text("synthetic-adapter-token")
    secret.chmod(0o600)
    config = (
        f'<javascriptwebhook name="wchat_events" url="{receiver.url}/nocodb" '
        'loopbackhttp="yes">\n'
        f'<javascriptwebhookheader endpoint="wchat_events" name="xc-token" '
        f'valuefile="{secret}">\n'
    )
    reload((root / "examples/javascript/wchat-nocodb.js").read_text(), "admin", config)
    before = len(receiver.posts)
    caller.send("JOIN #wchat")
    caller.barrier("adapter-first-join")
    other.send("JOIN #wchat")
    other.barrier("adapter-second-join")
    receiver.wait(before + 2)
    message = "exact café λ  two spaces"
    for _ in range(2):
        caller.send(f"PRIVMSG #wchat :{message}")
        other.until(lambda line: " PRIVMSG #wchat :" in line and line.endswith(message))
    receiver.wait(before + 4)

    caller.send(f"PRIVMSG {other.nick} :PRIVATE-DM-MARKER")
    caller.send("JOIN #adapter-private")
    caller.send("PRIVMSG #adapter-private :PRIVATE-CHANNEL-MARKER")
    caller.send("NOTICE #wchat :NOTICE-MARKER")
    caller.send("PRIVMSG @#wchat :STATUS-MARKER")
    caller.send("MODE #wchat +m")
    caller.barrier("adapter-mode-ready")
    other.send("PRIVMSG #wchat :BLOCKED-MARKER")
    other.until(lambda line: " 404 " in line and "#wchat" in line)
    other.send("PART #wchat :PRIVATE-PART-REASON")
    other.barrier("adapter-second-part")
    caller.send("PART #wchat :PRIVATE-PART-REASON")
    caller.send("JOIN #wchat")
    caller.barrier("adapter-rejoin")
    receiver.wait(before + 7)
    time.sleep(0.2)
    posts = receiver.posts[before:]
    assert len(posts) == 7
    rows = []
    for route, body, headers in posts:
        assert route == "/nocodb"
        headers = {key.lower(): value for key, value in headers.items()}
        assert headers["xc-token"] == "synthetic-adapter-token"
        assert len(body) <= 16384
        row = json.loads(body)
        assert set(row) == {"key", "data"}
        assert row["key"].startswith("wchat-")
        data = row["data"]
        expected = {"version", "event", "user", "channel", "observedAt"}
        if data["event"] == "irc.channel.message":
            expected |= {"messageType", "text"}
            assert data["messageType"] == "PRIVMSG" and data["text"] == message
            assert data["user"]["nick"] == caller.nick
        assert set(data) == expected
        assert data["channel"] == "#wchat" and data["version"] == 1
        assert set(data["user"]) == {"id", "nick"}
        assert len(data["user"]["id"]) == 9
        rows.append(row)
    assert len({row["key"] for row in rows}) == 7
    events = [row["data"]["event"] for row in rows]
    assert events.count("irc.channel.join") == 3
    assert events.count("irc.channel.part") == 2
    assert events.count("irc.channel.message") == 2
    serialized = json.dumps(rows)
    for marker in (
        "PRIVATE-",
        "NOTICE-MARKER",
        "STATUS-MARKER",
        "BLOCKED-MARKER",
        "synthetic-adapter-token",
        "127.0.0.1",
    ):
        assert marker not in serialized
    print(
        "PASS: NocoDB envelope, JOIN/PART/rejoin, final repeated text, sender, routing/privacy filters"
    )
