#!/usr/bin/env python3
"""Require a current ci:heavy label applied by a repository write/maintain/admin user.

Uses the read-only automatic GitHub token. No contributor data is evaluated as code.
The label authorizes heavy work on later commits until it is removed.
"""

import json
import os
import urllib.error
import urllib.parse
import urllib.request


def main():
    with open(os.environ["GITHUB_EVENT_PATH"], encoding="utf-8") as handle:
        event = json.load(handle)
    pr = event["pull_request"]
    repo = os.environ["GITHUB_REPOSITORY"]
    token = os.environ["GH_TOKEN"]

    def get(path):
        request = urllib.request.Request(
            "https://api.github.com/repos/" + repo + path,
            headers={
                "Authorization": "Bearer " + token,
                "Accept": "application/vnd.github+json",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        with urllib.request.urlopen(request, timeout=20) as response:
            return json.load(response)

    current = get("/pulls/" + str(pr["number"]))
    enabled = False
    if current["head"]["sha"] == pr["head"]["sha"] and any(
        label["name"] == "ci:heavy" for label in current["labels"]
    ):
        actor = None
        for page in range(1, 11):
            events = get("/issues/" + str(pr["number"]) + "/events?per_page=100&page=" + str(page))
            for item in events:
                if (
                    item.get("event") == "labeled"
                    and item.get("label", {}).get("name") == "ci:heavy"
                ):
                    actor = item["actor"]["login"]
            if len(events) < 100:
                break
        else:
            raise RuntimeError("Too many issue events; remove and reapply ci:heavy on a fresh PR")
        if actor is None:
            raise RuntimeError("Could not verify who applied ci:heavy")
        permission = get("/collaborators/" + urllib.parse.quote(actor, safe="") + "/permission")[
            "permission"
        ]
        if permission not in ("admin", "maintain", "write"):
            raise RuntimeError(
                "ci:heavy must be applied by a user with repository write/maintain/admin permission"
            )
        enabled = True
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write("enabled=" + str(enabled).lower() + "\n")
    print(
        "Heavy tests requested and authorized"
        if enabled
        else "Heavy tests not requested for this current head"
    )


if __name__ == "__main__":
    main()
