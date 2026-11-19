"""Fail a CEdev build that links but cannot fit the calculator code region.

CEdev v15 documents a 64-KiB code/data/rodata window. The compressed .8xp
size does NOT give its execution footprint. This checks the linker map's
actual relocated payload, before anyone transfers an oversized executable.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

MAX_EXECUTABLE_BYTES = 64 * 1024


def read_link_address(map_text: str, name: str) -> int:
    pattern = rf"^\s*0x([0-9a-fA-F]+)\s+{re.escape(name)}\s*="
    match = re.search(pattern, map_text, re.MULTILINE)
    if match is None:
        raise ValueError(f"CEdev map is missing {name}")
    return int(match.group(1), 16)


def verify_memory(map_text: str) -> tuple[int, int]:
    load = read_link_address(map_text, "LOAD_ADDR")
    end = read_link_address(map_text, "___runprgm_reloc_end")
    bss_end = read_link_address(map_text, "___bss_high")
    bss_high = read_link_address(map_text, "BSSHEAP_HIGH")
    used = end - load
    if used <= 0 or used > MAX_EXECUTABLE_BYTES:
        raise ValueError(
            f"Uncompressed code/data is {used:,} bytes; "
            f"CEdev's 64-KiB execution budget is {MAX_EXECUTABLE_BYTES:,}. "
            "A compressed .8xp may still compile but fail on the calculator."
        )
    if bss_end > bss_high:
        raise ValueError("BSS/heap overlaps the CEdev memory ceiling")
    return used, MAX_EXECUTABLE_BYTES - used


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map", type=Path, help="CEdev linker map, e.g. bin/CINEMA.map")
    args = parser.parse_args()
    try:
        used, free = verify_memory(args.map.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        parser.exit(1, f"FAIL: calculator memory-budget check: {exc}\n")
    print(f"PASS: uncompressed executable {used:,} B, {free:,} B below 64 KiB limit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
