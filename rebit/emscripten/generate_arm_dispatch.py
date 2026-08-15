#!/usr/bin/env python3
"""Generate the direct WebAssembly ARM interpreter dispatcher."""

from __future__ import annotations

import argparse
import re
from collections import OrderedDict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TABLE_PATH = ROOT / "src" / "ARM_InstrTable.h"
OUTPUT_PATH = ROOT / "src" / "ARM_InstrDispatch.h"


def parse_table(source: str, name: str, count: int) -> list[str]:
    match = re.search(
        rf"INSTRFUNC_PROTO\({re.escape(name)}\[{count}\]\)\s*=\s*\{{(.*?)\n\}};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise RuntimeError(f"Could not find {name}[{count}] in {TABLE_PATH}.")

    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", match.group(1), flags=re.DOTALL)
    entries = [entry.strip() for entry in body.split(",") if entry.strip()]
    if len(entries) != count:
        raise RuntimeError(f"Expected {count} {name} entries, found {len(entries)}.")
    return entries


def render_dispatch(name: str, entries: list[str]) -> str:
    groups: OrderedDict[str, list[int]] = OrderedDict()
    for index, function in enumerate(entries):
        groups.setdefault(function, []).append(index)

    lines = [
        f"void {name}(ARM* cpu, u32 code)",
        "{",
        "    switch (code)",
        "    {",
    ]
    for function, indices in groups.items():
        lines.extend(f"    case {index}:" for index in indices)
        lines.append(f"        return {function}(cpu);")
    lines.extend(
        [
            "    default:",
            "        __builtin_unreachable();",
            "    }",
            "}",
        ]
    )
    return "\n".join(lines)


def generate() -> str:
    source = TABLE_PATH.read_text(encoding="utf-8")
    arm = parse_table(source, "ARMInstrTable", 4096)
    thumb = parse_table(source, "THUMBInstrTable", 1024)
    return (
        "/* Generated from ARM_InstrTable.h. Do not edit manually. */\n"
        "#pragma once\n\n"
        "namespace melonDS::ARMInterpreter\n"
        "{\n"
        f"{render_dispatch('DispatchARM', arm)}\n\n"
        f"{render_dispatch('DispatchTHUMB', thumb)}\n\n"
        "}\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail instead of writing when ARM_InstrDispatch.h is stale",
    )
    arguments = parser.parse_args()
    generated = generate()

    if arguments.check:
        if not OUTPUT_PATH.is_file() or OUTPUT_PATH.read_text(encoding="utf-8") != generated:
            parser.error(f"{OUTPUT_PATH} is stale; rerun {Path(__file__).relative_to(ROOT)}")
        return 0

    OUTPUT_PATH.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
