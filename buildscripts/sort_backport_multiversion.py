#!/usr/bin/env python3
"""Sort the 'all' lists in etc/backports_required_for_multiversion_tests.yml by test_file and ticket."""

import argparse
import sys
from pathlib import Path

import yaml

BACKPORTS_FILE = (
    Path(__file__).parent.parent / "etc" / "backports_required_for_multiversion_tests.yml"
)


class _IndentedDumper(yaml.Dumper):
    """Dumper that indents sequence items relative to their parent key.

    By default PyYAML emits sequences "indentless" (the dash aligns with the
    parent mapping key).  Overriding increase_indent with indentless=False
    gives the 4-space-indented style used in the backports file:

        last-continuous:
          all:
            - test_file: ...
              ticket: ...
    """

    def increase_indent(self, flow: bool = False, indentless: bool = False) -> None:
        return super().increase_indent(flow=flow, indentless=False)


def _extract_header(str_data: str) -> str:
    """Extract lines from the file starting with '#'."""
    lines = str_data.splitlines(keepends=True)
    header_lines = []
    for line in lines:
        if line.startswith("#"):
            header_lines.append(line)
        else:
            break
    return "".join(header_lines)


def _sort_all_entries(yml_data: dict) -> None:
    """Sort the entries in-place by test_file and ticket."""
    for section in ("last-continuous", "last-lts"):
        entries = (yml_data.get(section) or {}).get("all")
        if entries is None:
            continue
        entries.sort(key=lambda e: (e["test_file"], e["ticket"]))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fix",
        action="store_true",
        default=False,
        help="Sort the file in place (default).",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        default=False,
        help="Return an error if the file is not sorted.",
    )
    args = parser.parse_args()

    if args.fix and args.check:
        parser.error("--fix and --check are mutually exclusive.")

    backports_file_str = BACKPORTS_FILE.read_text()
    header = _extract_header(backports_file_str)
    data = yaml.safe_load(backports_file_str)
    _sort_all_entries(data)
    sorted_content = header + yaml.dump(
        data,
        Dumper=_IndentedDumper,
        default_flow_style=False,
        sort_keys=False,
        indent=2,
        allow_unicode=True,
    )

    if args.check:
        if backports_file_str != sorted_content:
            print(
                "etc/backports_required_for_multiversion_tests.yml is not sorted.\n"
                "Run buildscripts/sort_backport_multiversion.py --fix to fix it."
            )
            sys.exit(1)
    else:
        BACKPORTS_FILE.write_text(sorted_content)


if __name__ == "__main__":
    main()
