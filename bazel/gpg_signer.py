#!/usr/bin/env python3
"""Action helper to sign a file with a provided private key using GPG.

This replaces the inline `_gpg_sign.sh` that `bazel/signing.bzl` used to generate.

Args (positional):
  1) GPG: path to the gpg binary to execute
  2) KEY: path to the ascii-armored private key file to import
  3) PASSPHRASE: optional path to a file containing the passphrase (or "" if none)
  4) OUT: output signature path
  5) INP: input file path to sign
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from typing import List, Optional

debug = False  # manually change to enable verbose output


def _debug(msg: str) -> None:
    if debug:
        print(msg, file=sys.stderr)


def _run(argv: List[str], *, capture_stdout: bool = False) -> subprocess.CompletedProcess:
    if capture_stdout:
        return subprocess.run(
            argv, check=True, text=True, stdout=subprocess.PIPE, stderr=sys.stderr
        )
    if not debug:
        with open(os.devnull, "wb") as null:
            return subprocess.run(argv, check=True, stdout=null, stderr=null)
    else:
        return subprocess.run(argv, check=True)


def _extract_fingerprint(colons_output: str) -> Optional[str]:
    # gpg --with-colons output: lines like "fpr:::::::::FINGERPRINT:"
    for line in colons_output.splitlines():
        if not line.startswith("fpr:"):
            continue
        parts = line.split(":")
        if len(parts) > 9 and parts[9]:
            return parts[9]
    return None


def main(argv: List[str]) -> int:
    _debug("Starting gpg_signer.py")

    if len(argv) != 6:
        print(
            "usage: gpg_signer.py <gpg> <key> <passphrase_file_or_empty> <out> <inp>",
            file=sys.stderr,
        )
        return 2

    gpg = argv[1]
    key = argv[2]
    passphrase_file = argv[3] or None
    out_path = argv[4]
    inp_path = argv[5]

    # Use helpers from the same bundle as `gpg` to avoid accidentally picking up system gpg-agent/gpgconf,
    # especially under remote execution.
    bindir = os.path.dirname(gpg)
    gpg_agent = os.path.join(bindir, "gpg-agent")
    gpgconf = os.path.join(bindir, "gpgconf")

    # Unique temp homedir for this action.
    base_tmp = os.environ.get("TMPDIR") or os.getcwd()
    gpgdir = tempfile.mkdtemp(prefix="gpg.", dir=base_tmp)
    os.chmod(gpgdir, 0o700)

    try:
        # Disable agent caching for this home directory.
        with open(os.path.join(gpgdir, "gpg-agent.conf"), "w", encoding="utf-8") as fh:
            fh.write(
                "default-cache-ttl 0\n"
                "max-cache-ttl 0\n"
                "ignore-cache-for-signing\n"
                "allow-loopback-pinentry\n"
            )

        _debug("Starting gpg-agent")
        # Inherit stdout/stderr so logs show up in action output (like the old shell script).
        _run([gpg_agent, "--homedir", gpgdir, "--daemon", "--verbose"])
        _debug("gpg-agent importing key to home dir")

        # Import the private key into the temp homedir.
        _run([gpg, "--homedir", gpgdir, "--batch", "--import", key])

        # Find fingerprint.
        cp = _run([gpg, "--homedir", gpgdir, "--list-keys", "--with-colons"], capture_stdout=True)
        fpr = _extract_fingerprint(cp.stdout)
        if not fpr:
            print(
                "Failed to determine key fingerprint from gpg --with-colons output", file=sys.stderr
            )
            return 1

        # Build passphrase options if provided.
        pass_opts: List[str] = []
        if passphrase_file:
            pass_opts = ["--pinentry-mode", "loopback", "--passphrase-file", passphrase_file]

        _run(
            [
                gpg,
                "--homedir",
                gpgdir,
                "--batch",
                "--yes",
                *pass_opts,
                "--detach-sign",
                "-u",
                fpr,
                "-o",
                out_path,
                inp_path,
            ]
        )
        return 0
    finally:
        # Cleanup.
        try:
            subprocess.run([gpgconf, "--homedir", gpgdir, "--kill", "gpg-agent"], check=False)
        finally:
            shutil.rmtree(gpgdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
