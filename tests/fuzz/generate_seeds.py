#!/usr/bin/env python3
"""Generate a reproducible binary corpus from readable seed specifications."""

import argparse
import json
import struct
from pathlib import Path


def value(spec):
    if isinstance(spec, str):
        return spec.encode("utf-8")
    if "hex" in spec:
        return bytes.fromhex(spec["hex"])
    return spec["repeat"].encode("utf-8") * spec["count"]


def sized(data):
    return struct.pack("<H", len(data)) + data


def command(data, parameters):
    return sized(data) + bytes([len(parameters)]) + b"".join(map(sized, parameters))


def loader(case):
    first, second = map(value, case["files"])
    name = value(case["wasm_name"])
    per_file, total, count = case["limits"]
    return (
        struct.pack(
            "<BBBHHB",
            case["source_case"],
            case["authority"],
            case["wasm_case"],
            per_file,
            total,
            count,
        )
        + sized(first)
        + sized(second)
        + bytes([len(name)])
        + name
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = json.loads(Path(__file__).with_name("seeds.json").read_text(encoding="utf-8"))
    targets = ("wire_validation", "parsed_command", "script_loader")
    for target in targets:
        (args.output / target).mkdir(parents=True, exist_ok=True)

    commands = {
        case["name"]: command(value(case["command"]), list(map(value, case["parameters"])))
        for case in source["commands"]
    }
    for count in (14, 15, 16, 17):
        commands[f"parameter-count-{count}"] = command(b"X", [b"p"] * count)
    commands["every-byte"] = command(bytes(range(256)), [bytes(range(256)), b""])
    commands["truncated-header"] = b"\xff"
    commands["truncated-parameter"] = b"\x01\x00A\x02\xff\xffx"
    commands["empty-input"] = b""
    for target in targets[:2]:
        for name, data in commands.items():
            (args.output / target / name).write_bytes(data)

    loaders = {case["name"]: loader(case) for case in source["loader"]}
    baseline = source["loader"][1]
    for scenario in range(12):
        for authority in range(3):
            loaders[f"source-{scenario}-authority-{authority}"] = loader(
                {**baseline, "source_case": scenario, "authority": authority}
            )
    for scenario in range(7):
        loaders[f"wasm-{scenario}"] = loader({**baseline, "wasm_case": scenario})
    for index, limits in enumerate(
        (
            [0, 1024, 4],
            [512, 0, 4],
            [512, 1024, 0],
            [4, 1024, 4],
            [5, 1024, 4],
            [6, 1024, 4],
            [512, 10, 4],
            [512, 11, 4],
            [512, 12, 4],
            [512, 1024, 1],
            [512, 1024, 2],
        )
    ):
        loaders[f"limit-{index}"] = loader({**baseline, "limits": limits})
    loaders["empty-input"] = b""
    for name, data in loaders.items():
        (args.output / "script_loader" / name).write_bytes(data)
    print(f"Generated {len(commands)} command and {len(loaders)} loader seeds in {args.output}")


if __name__ == "__main__":
    main()
