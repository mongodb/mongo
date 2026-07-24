#!/usr/bin/env python3
"""Verify uv.lock is in sync with pyproject.toml.

Returns non-zero if either:
  1. `uv lock --check` would modify uv.lock (pyproject.toml drifted from lock), or
  2. pyproject.toml's `uv==X.Y.Z` pin in the `export` group has drifted from
     the canonical version in `buildscripts/uv_version.txt`.

Note: under the rules_pycross migration there is no longer a
`bazel/uv/requirements.bazel.lock` sidecar to keep in sync (pycross reads
uv.lock directly), so the third drift check that lived here on the earlier
`zac/uv-poc` branch has been dropped.

Resolves the `uv` binary at runtime rather than going through `python -m uv`.
Under rules_python/pip.parse the wheel launcher had trouble finding the
`bin/uv` binary; even though we no longer use pip.parse, keeping the direct
resolution logic lets this script work when invoked via `bazel run` (where
`uv` may live somewhere unusual in runfiles) as well as directly from an
activated venv.
"""

import os
import pathlib
import shutil
import subprocess
import sys

REPO_ROOT = os.environ.get("BUILD_WORKSPACE_DIRECTORY", ".")


def _find_uv():
    """Locate a usable `uv` binary.

    Search order:
      1. `shutil.which("uv")` — covers developers running this script with a
         venv activated (which is the canonical way to invoke it).
      2. `python3-venv/bin/uv` — the workstation venv populated by
         buildscripts/uv_sync.sh.
      3. Bazel runfiles walk — if invoked via `bazel run`, look for the
         wheel-shipped `bin/uv` alongside pycross-installed site-packages.
    """
    # 1. PATH lookup — most common case.
    found = shutil.which("uv")
    if found:
        return found

    # 2. Workstation venv fallback.
    candidate = pathlib.Path(REPO_ROOT) / "python3-venv" / "bin" / "uv"
    if candidate.exists():
        return str(candidate)

    # 3. Bazel runfiles: pycross-installed wheels expose their contents via
    # sys.path entries whose parent contains a `bin/uv`.
    for entry in sys.path:
        if entry.rstrip("/").endswith("/site-packages"):
            candidate = pathlib.Path(entry).parent / "bin" / "uv"
            if candidate.exists():
                return str(candidate)

    sys.exit(
        "ERROR: could not locate the `uv` binary. Run buildscripts/uv_sync.sh "
        "first, or install uv via pipx."
    )


UV = _find_uv()


def _run(cmd, **kwargs):
    """Echo and run a subprocess command."""
    print("+", " ".join(cmd), file=sys.stderr)
    return subprocess.run(cmd, cwd=REPO_ROOT, **kwargs)


def _check_uv_lock():
    """Fail if `uv lock --check` would rewrite uv.lock."""
    proc = _run([UV, "lock", "--check"])
    if proc.returncode != 0:
        sys.exit(
            "uv.lock is out of sync with pyproject.toml.\n"
            "Run `uv lock` from the repo root and commit the result."
        )


def _check_uv_version_pin_parity():
    """Verify pyproject.toml's `uv==X.Y.Z` pin in the `export` group matches
    the canonical version in `buildscripts/uv_version.txt`.

    Every other uv installer in the repo (shell scripts, Dockerfiles,
    powercycle bootstrap) reads uv_version.txt directly, but TOML can't
    do command substitution — so the pyproject copy is checked here.
    """
    version_file = pathlib.Path(REPO_ROOT) / "buildscripts" / "uv_version.txt"
    if not version_file.exists():
        sys.exit(f"ERROR: {version_file} is missing.")
    canonical = version_file.read_text().strip()

    import re

    pyproject = pathlib.Path(REPO_ROOT) / "pyproject.toml"
    content = pyproject.read_text()
    match = re.search(r'"uv==([0-9]+\.[0-9]+\.[0-9]+)"', content)
    if not match:
        sys.exit(f"ERROR: could not find `uv==<version>` pin in {pyproject}.")
    pinned = match.group(1)

    if pinned != canonical:
        sys.exit(
            f"uv version pin drift: pyproject.toml has 'uv=={pinned}' but "
            f"buildscripts/uv_version.txt says '{canonical}'. Update one to "
            "match the other (uv_version.txt is the canonical source)."
        )


def main():
    _check_uv_lock()
    _check_uv_version_pin_parity()
    print("uv.lock is in sync with pyproject.toml.")


if __name__ == "__main__":
    main()
