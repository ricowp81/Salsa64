#!/usr/bin/env python3
"""Derive a reproducible CI build label from Salsa64 main's latest release."""
from __future__ import annotations
import argparse, re, sys
from pathlib import Path
RELEASE_HEADING = re.compile(r"^##\s+\[(\d+\.\d+\.\d+)\]")
def read_release_version(changelog: Path) -> str:
    try:
        lines = changelog.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise SystemExit(f"cannot read release changelog {changelog}: {exc}") from exc
    for line in lines:
        match = RELEASE_HEADING.match(line)
        if match:
            return match.group(1)
    raise SystemExit(f"no x.y.z release heading found in {changelog}")
def alpha_suffix(number: int) -> str:
    if number < 1:
        raise ValueError("build number must be at least 1")
    result: list[str] = []
    while number:
        number, remainder = divmod(number - 1, 26)
        result.append(chr(ord("a") + remainder))
    return "".join(reversed(result))
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_number", type=int, help="GitHub Actions workflow run number")
    parser.add_argument("--changelog", type=Path, default=Path(__file__).resolve().parents[1] / "CHANGELOG.md")
    args = parser.parse_args()
    try:
        suffix = alpha_suffix(args.run_number)
    except ValueError as exc:
        parser.error(str(exc))
    print(f"v{read_release_version(args.changelog)}{suffix}")
    return 0
if __name__ == "__main__":
    sys.exit(main())
