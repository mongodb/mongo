#!/usr/bin/env python3
"""Action helper to export a dearmored pgp public key in armored mode using GPG.
Args (positional):
  1) GPG: path to the gpg binary to execute
  2) KEY: path to the public key file to import
  3) PASSPHRASE: optional path to a file containing the passphrase (or "" if none)
  4) ARMORED_KEY_OUTPUT_FILE: output armored public key path
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

debug = False  # manually change to enable verbose output


def _debug(msg: str) -> None:
    if debug:
        print(msg, file=sys.stderr)


def _run(argv: list[str], *, capture_stdout: bool = False) -> subprocess.CompletedProcess:
    if capture_stdout:
        return subprocess.run(
            argv, check=True, text=True, stdout=subprocess.PIPE, stderr=sys.stderr
        )
    if not debug:
        with open(os.devnull, "wb") as null:
            return subprocess.run(argv, check=True, stdout=null, stderr=null)
    else:
        return subprocess.run(argv, check=True)


def _create_gpg_home() -> Path:
    """Create an isolated GPG home in the action's writable temporary directory."""
    gpgdir = tempfile.mkdtemp(prefix="mongo-gpg-", dir=os.environ.get("TMPDIR"))
    os.chmod(gpgdir, 0o700)
    return Path(gpgdir)


def _extract_fingerprint(colons_output: str) -> Optional[str]:
    # gpg --with-colons output: lines like "fpr:::::::::FINGERPRINT:"
    for line in colons_output.splitlines():
        if not line.startswith("fpr:"):
            continue
        parts = line.split(":")
        if len(parts) > 9 and parts[9]:
            return parts[9]
    return None


def main(argv: list[str]) -> int:
    _debug("Starting gpg_export_armored_key.py")

    if len(argv) != 5:
        print(
            "usage: gpg_export_armored_key.py <gpg> <key> <passphrase_file_or_empty> <armored_key_output_file>",
            file=sys.stderr,
        )
        return 2

    gpg = argv[1]
    key = argv[2]
    passphrase_file = argv[3] or None
    armored_key_output_file = argv[4]

    # Use helpers from the same bundle as `gpg` to avoid accidentally picking up system gpg-agent/gpgconf,
    # especially under remote execution.
    bindir = os.path.dirname(gpg)
    gpg_agent = os.path.join(bindir, "gpg-agent")
    gpgconf = os.path.join(bindir, "gpgconf")

    # TMPDIR is action-scoped and writable, including when the execution image has a
    # read-only /tmp. Keep the directory name short because gpg-agent creates sockets
    # beneath it.
    gpgdir = _create_gpg_home()

    try:
        # Disable agent caching for this home directory.
        with open(gpgdir / "gpg-agent.conf", "w", encoding="utf-8") as fh:
            fh.write(
                "default-cache-ttl 0\n"
                "max-cache-ttl 0\n"
                "ignore-cache-for-signing\n"
                "allow-loopback-pinentry\n"
                "disable-scdaemon\n"
            )

        _debug("Starting gpg-agent")
        # Inherit stdout/stderr so logs show up in action output (like the old shell script).
        _run([gpg_agent, "--homedir", str(gpgdir), "--daemon", "--verbose"])
        _debug("gpg-agent importing key to home dir")

        # Import the private key into the temp homedir.
        _run([gpg, "--homedir", str(gpgdir), "--batch", "--import", key])
        _debug("gpg-agent imported the key!")

        # Find fingerprint.
        cp = _run(
            [gpg, "--homedir", str(gpgdir), "--list-keys", "--with-colons"],
            capture_stdout=True,
        )
        fpr = _extract_fingerprint(cp.stdout)
        if not fpr:
            print(
                "Failed to determine key fingerprint from gpg --with-colons output", file=sys.stderr
            )
            return 1
        _debug("gpg-agent extracted fingerprint: " + fpr)

        # Build passphrase options if provided.
        pass_opts: list[str] = []
        if passphrase_file:
            pass_opts = ["--pinentry-mode", "loopback", "--passphrase-file", passphrase_file]

        cp = _run(
            [
                gpg,
                "--homedir",
                str(gpgdir),
                "--batch",
                *pass_opts,
                "--armor",
                "--export",
                fpr,
            ],
            capture_stdout=True,
        )
        with open(armored_key_output_file, "w+") as outf:
            outf.write(cp.stdout)

        return 0
    finally:
        # Cleanup.
        try:
            subprocess.run(
                [gpgconf, "--homedir", str(gpgdir), "--kill", "gpg-agent"],
                check=False,
            )
        finally:
            shutil.rmtree(gpgdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
