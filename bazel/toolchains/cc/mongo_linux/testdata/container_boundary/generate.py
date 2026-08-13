#!/usr/bin/env python3

from __future__ import annotations

import os
import pathlib
import sys


def main() -> None:
    if sentinel_value := os.environ.get("MONGO_CONTAINER_BOUNDARY_SENTINEL"):
        sentinel = pathlib.Path(sentinel_value)
        if sentinel.exists():
            raise RuntimeError(f"Python generator escaped the action container and saw {sentinel}")

    source = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    output.write_text(source.read_text(encoding="utf-8").upper(), encoding="utf-8")


if __name__ == "__main__":
    main()
