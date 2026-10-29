#!/usr/bin/env python3
"""Filter multiversion-downloads.json down to the binaries a task actually needs.

`select_multiversion_binaries` runs once per build variant, resolves every version listed in
`evergreen/multiversion_selection.sh`, and writes them all to `multiversion-downloads.json`. Every
multiversion task on that variant then hands that file to `db-contrib-tool setup-repro-env`, which
downloads *every* entry -- passing the tool a `.json` path makes it one VERSIONS_FILE request that
it expands wholesale, with no way to ask for a subset.

So a task testing only against last-LTS also downloads 7.0, 8.0 and 8.0.16, and nothing is cached
between tasks because `multiversion_setup.sh` removes the install and link dirs first.

There are exactly two cases:

1. The task generator passed the versions this task needs, in `multiversion_setup_versions`. It
   knows them because it created the sub-task for a specific old version. Keep those entries, drop
   the rest.
2. It passed nothing -- an older generator that predates the variable, or a task that tests against
   several versions and has no single old one. Leave the file alone, which is the behaviour before
   this script existed.

Deliberately no inference: nothing here reads suite configs or derives a suite from a task name. If
the generator did not say, we do not guess.

MATCHING. The generator speaks in aliases (`last_lts`) while the downloads file records resolved
versions (`bin_suffix: "9.0"`), because db-contrib-tool resolves aliases at selection time. Aliases
are mapped with resmoke's own `MultiversionService.get_binary_name_for_version`, which is what
names the binary on disk (`mongod-9.0`), so this cannot disagree with what the fixture looks for.
Anything that is not a known alias is taken literally, so an exact pin like `8.0.16` needs no
special handling.

FAILURE MODES. This runs on the critical path of every multiversion task, so it fails open:

  * no versions passed -> pass the file through untouched.
  * a required version has no matching entry -> drift between `multiversion_selection.sh` and what
    the task declared. Warn and pass everything through; `--strict` makes it an error instead,
    which is what you want once this is rolled out, since the alternative is a missing binary
    surfacing later as a confusing test failure.
  * anything unexpected -> pass through.
"""

import argparse
import json
import os.path
import sys

# Match resmoke.py: make the repo root importable so this runs as a script from anywhere.
if __name__ == "__main__" and __package__ is None:
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.resmokelib import multiversionconstants

# Aliases that resolve to a concrete version. Anything else is treated as an exact version.
KNOWN_ALIASES = ("last_lts", "last_continuous", "last_patch")


def suffixes_from_versions(versions: str) -> set[str]:
    """Resolve a space-delimited version list to the `bin_suffix` values the file uses.

    Elements are either an alias (`last_lts`, or `last-lts` -- MultiversionOptions uses
    underscores) or an exact version (`7.0`, `8.0.16`).

    Aliases go through the helper that names the binary on disk, with the base name stripped,
    rather than reading the FCV constants directly -- if the naming scheme changes, this follows it.
    """
    service = multiversionconstants.multiversion_service
    suffixes = set()
    for element in versions.split():
        alias = element.replace("-", "_")
        if alias in KNOWN_ALIASES:
            suffixes.add(
                service.get_binary_name_for_version(alias, "mongod").removeprefix("mongod-")
            )
        else:
            suffixes.add(element)
    return suffixes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Path to multiversion-downloads.json.")
    parser.add_argument("--output", help="Where to write the filtered file. Defaults to --input.")
    parser.add_argument(
        "--versions",
        default="",
        help="Space-delimited versions this task needs, from the task generator's "
        "${multiversion_setup_versions}. Empty means download everything.",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Fail if a required version has no entry in the downloads file.",
    )
    args = parser.parse_args()
    output = args.output or args.input

    if not args.versions.strip():
        print(
            "multiversion filter: no versions from the task generator; downloading all binaries",
            file=sys.stderr,
        )
        return 0

    with open(args.input) as fh:
        downloads = json.load(fh)

    if not isinstance(downloads, list):
        print(
            f"multiversion filter: expected a JSON list in {args.input}, leaving it alone",
            file=sys.stderr,
        )
        return 0

    try:
        required = suffixes_from_versions(args.versions)
    except Exception as exc:
        print(
            f"multiversion filter: could not resolve {args.versions!r} ({exc}); "
            "downloading all binaries",
            file=sys.stderr,
        )
        return 0

    available = {entry.get("bin_suffix") for entry in downloads}
    missing = required - available
    if missing:
        message = (
            f"multiversion filter: {args.versions!r} needs {sorted(missing)}, but {args.input} "
            f"only offers {sorted(v for v in available if v)}. Check that "
            "evergreen/multiversion_selection.sh resolves every version tasks ask for."
        )
        if args.strict:
            print(message, file=sys.stderr)
            return 1
        print(f"{message} Downloading all binaries instead.", file=sys.stderr)
        return 0

    kept = [entry for entry in downloads if entry.get("bin_suffix") in required]
    dropped = sorted(v for v in available - required if v)

    with open(output, "w") as fh:
        json.dump(kept, fh, indent=2)

    print(
        f"multiversion filter: {args.versions!r} keeping {sorted(required)}"
        + (f", skipping download of {dropped}" if dropped else ""),
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
