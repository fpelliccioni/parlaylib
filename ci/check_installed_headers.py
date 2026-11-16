#!/usr/bin/env python3
"""Verify the installed header tree of a CMake or Conan package without modifying it."""
import argparse
from pathlib import Path

REQUIRED = ("parallel.h", "primitives.h", "sequence.h", "internal/work_stealing_job.h")


def files(directory):
    return {p.relative_to(directory).as_posix() for p in directory.rglob("*") if p.is_file()}


def check(root, source=None):
    owned = root / "parlay"
    if not owned.is_dir():
        raise SystemExit(f"Missing packaged header root: {owned}")
    for header in REQUIRED:
        if not (owned / header).is_file():
            raise SystemExit(f"Missing required header: parlay/{header}")
    if source is not None:
        # The installation carries the source header tree: nothing missing, nothing added.
        expected, actual = files(source / "parlay"), files(owned)
        if actual != expected:
            raise SystemExit(f"Installed headers differ from the source tree: "
                             f"missing {sorted(expected - actual)!r}, unexpected {sorted(actual - expected)!r}")
    print(f"PASS installed header layout: {owned}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("include_root", type=Path)
    parser.add_argument("--source", type=Path, help="source include directory to compare with")
    arguments = parser.parse_args()
    check(arguments.include_root, arguments.source)
