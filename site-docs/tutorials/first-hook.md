# Your first command hook

In this lesson you will run a disposable loopback-only server, rewrite one PING,
and observe a notice after VERSION. Use a machine with Docker, Git, and an IRC
client that supports raw commands. Do not use a production server. Port 16667
must be free; allow time and disk space for a native V8/InspIRCd build.

## 1. Get the lesson files and build

Run from a fresh checkout (skip cloning if already in this repository):


~~~sh
git clone --branch feat/local-echo-command https://github.com/r33drichards/docker.git
cd docker
docker build -f Dockerfile.javascript -t inspircd:javascript .
~~~

The optional image contains the module but will load it only because our lesson
configuration explicitly requests it. A build must finish successfully before continuing.
If a pinned Alpine dependency is unavailable, stop and consult the maintainer rather
than substituting mismatched headers and libraries.

## 2. Start your isolated server


~~~sh
docker run -d --name js-lesson --workdir /inspircd   -p 127.0.0.1:16667:6667   -v "$PWD/examples/javascript/tutorial:/lab:ro"   --entrypoint /inspircd/bin/inspircd inspircd:javascript   --nofork --config /lab/inspircd.conf
~~~

The mount is read-only. The container runs as UID 10000, so the example directory
must be traversable and its files readable by that UID. Only host loopback port
16667 is published; no TLS or public networking is set up in this lesson.
Follow startup with `docker logs -f js-lesson`. Wait for normal startup; if the
process exits, read its configuration/module error before continuing.

## 3. Observe the rewritten PING

Connect your IRC client to **127.0.0.1:16667**, without TLS, using any new nickname.
After registration send a raw IRC command (many clients use `/quote`):

~~~text
/quote PING :lesson
~~~

Expect a server PONG ending in `hello-from-javascript` instead of `lesson`.
The first callback changed the parameters used by the native command parser.
An ordinary `/quote PING :unchanged` still receives a PONG containing `unchanged`.

## 4. Observe the post-command hook

~~~text
/quote VERSION
~~~

Expect the ordinary VERSION reply and a server NOTICE containing
`VERSION handler returned SUCCESS`. These are separate observations: a native
handler result does not prove delivery to another client.
Open `examples/javascript/tutorial/hooks.js` to match each result to its callback.

## 5. Clean up

~~~sh
docker rm -f js-lesson
~~~

You have registered synchronous callbacks, rewritten a command, and queued a
notice. Next, learn to [reload scripts safely](../how-to/reload.md) or look up
the [command event shape](../reference/commands.md).
