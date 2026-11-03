"""Helper intentionally supplied as data rather than as a Python source."""

import pathlib


def message() -> str:
    return pathlib.Path(__file__).with_name("message.txt").read_text(encoding="utf-8")
