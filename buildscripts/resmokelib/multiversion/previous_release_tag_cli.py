"""CLI entrypoint for :mod:`previous_release_tag`.

Kept separate from the library module so that ``multiversion_service`` can
import ``find_previous_release_tag`` eagerly without ``runpy`` warning that
``previous_release_tag`` is already in ``sys.modules`` when invoked as
``python -m ...previous_release_tag_cli``.
"""

from __future__ import annotations

import argparse
import sys
from typing import Optional

from buildscripts.resmokelib.multiversion.previous_release_tag import (
    DEFAULT_TAG_PATTERN,
    find_previous_release_tag,
)
from buildscripts.util import cmdutils


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="previous_release_tag",
        description=(
            "Find the previous release tag relative to a target commit -- "
            "i.e. the release tag closest in history to the target, never "
            "one that points at or descends from the target."
        ),
    )
    parser.add_argument(
        "target_commit",
        nargs="?",
        default="HEAD",
        help="Commit, branch, or hash to compare against (default: HEAD).",
    )
    parser.add_argument(
        "--tag-pattern",
        default=DEFAULT_TAG_PATTERN,
        help=(
            "Git tag glob to restrict the search "
            f"(default: {DEFAULT_TAG_PATTERN}). Examples: r9.*.*, r9.0.*"
        ),
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Enable debug logging to stderr (shows per-tag evaluation).",
    )
    return parser


def _cli(argv: Optional[list[str]] = None) -> None:
    args = _build_parser().parse_args(argv)

    cmdutils.enable_logging(verbose=args.debug)

    tag = find_previous_release_tag(args.target_commit, tag_pattern=args.tag_pattern)

    if tag is None:
        sys.exit(f"No tags matching pattern {args.tag_pattern} found in the repository.")

    print(tag)


if __name__ == "__main__":
    _cli()
