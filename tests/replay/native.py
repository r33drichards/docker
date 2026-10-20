# SPDX-License-Identifier: MIT
"""Real IRC replay plus heap assertions inside the embedded script realm.

No production eval endpoint is added. Trusted test assertions are baked into the
script at load time; a PING token selects only a predeclared assertion.
"""

import re


class NativeReplay:
    def __init__(self, client, reload_script, source, assertions, prefix="heap-replay"):
        if not re.fullmatch(r"[a-zA-Z0-9-]+", prefix):
            raise ValueError("invalid replay prefix")
        self.client = client
        self.prefix = prefix
        self.assertions = dict(assertions)
        cases = []
        for index, (_name, expression) in enumerate(self.assertions.items()):
            # Test source, not IRC-supplied JavaScript. Lexical Maps remain live.
            cases.append(
                "case " + str(index) + ": passed = ((" + expression + ") === true); break;"
            )
        probe = r"""
ircd.on('postCommand', e => {
  if (e.command !== 'PING' || e.params.length !== 1) return;
  const match = /^PREFIX-assert-(\d+)$/.exec(e.params[0]);
  if (!match) return;
  const index = Number(match[1]);
  let passed = false;
  try { switch (index) { CASES } } catch (_) { passed = false; }
  ircd.notice(e.userId, 'PREFIX-result-' + index + '-' + (passed ? 'pass' : 'fail'));
});
""".replace("PREFIX", prefix).replace("CASES", "\n".join(cases))
        reload_script(source + "\n" + probe, "admin")

    def send(self, command):
        """Replay a real IRC command; do not interpret arbitrary JS from IRC."""
        if "\r" in command or "\n" in command:
            raise ValueError("one IRC command per step")
        self.client.send(command)
        self.client.barrier(self.prefix + "-barrier")

    def assert_heap(self, name):
        index = list(self.assertions).index(name)
        token = f"{self.prefix}-result-{index}-"
        self.client.send(f"PING :{self.prefix}-assert-{index}")
        lines = self.client.until(lambda line: token in line)
        result = [line for line in lines if token in line]
        if len(result) != 1 or not result[0].endswith(token + "pass"):
            raise AssertionError(f"native heap assertion {name!r} failed: {result!r}")

    def run(self, steps):
        for step in steps:
            if set(step) == {"send"}:
                self.send(step["send"])
            elif set(step) == {"assert"}:
                self.assert_heap(step["assert"])
            else:
                raise ValueError("each native step needs exactly send or assert")


def exercise_heap_replay(client, reload_script):
    """Reusable actual-V8 Map/closure fixture for native suite integration."""
    source = """
const replayState = new Map();
let replayTotal = 0;
const accumulate = (() => {
  let closed = 0;
  return value => { closed += value; return closed; };
})();
ircd.on('postCommand', e => {
  if (e.command !== 'PING') return;
  if (e.params[0] === 'heap-add-a') {
    replayState.set('a', (replayState.get('a') || 0) + 1);
    replayTotal = accumulate(2);
  }
  if (e.params[0] === 'heap-remove-a') replayState.delete('a');
});
"""
    replay = NativeReplay(
        client,
        reload_script,
        source,
        {
            "empty Map": "replayState instanceof Map && replayState.size === 0",
            "first event": "replayState.get('a') === 1 && replayTotal === 2",
            "second event closure": "replayState.get('a') === 2 && replayTotal === 4",
            "removed": "replayState.size === 0 && replayTotal === 4",
        },
    )
    replay.run(
        [
            {"assert": "empty Map"},
            {"send": "PING :heap-add-a"},
            {"assert": "first event"},
            {"send": "PING :heap-add-a"},
            {"assert": "second event closure"},
            {"send": "PING :heap-remove-a"},
            {"assert": "removed"},
        ]
    )
