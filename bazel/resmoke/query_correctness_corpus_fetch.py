#!/usr/bin/env python3
"""Fetches a 10gen/query-correctness-tests-N corpus for the query_correctness suites."""

import argparse
import netrc
import os
import pwd
import shutil
import subprocess
import sys
import time
from pathlib import Path

RETRIES = 5
RETRY_SLEEP_SEC = 5

HOME = Path(os.environ.get("HOME") or pwd.getpwuid(os.getuid()).pw_dir)


def read_pin(conf: Path, repo: str) -> str:
    """Returns the commit pinned for `repo` in test_repos.conf."""
    for line in conf.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(":")
        if len(parts) != 2 or not parts[0] or not parts[1]:
            sys.exit(f"error: malformed {conf} line (want <repo>:<commit>): {line}")
        if parts[0] == repo:
            commit = parts[1]
            if len(commit) != 40:
                sys.exit(f"error: {repo} pin must be a full 40-char commit hash, got: {commit}")
            return commit
    sys.exit(f"error: {repo} not found in {conf}")


def github_token() -> str:
    """A GitHub token from the environment, ~/.netrc, or the gh CLI."""
    for var in ("GITHUB_TOKEN", "github_token"):
        if os.environ.get(var):
            return os.environ[var]

    netrc_path = Path(os.environ.get("NETRC", HOME / ".netrc"))
    if netrc_path.is_file():
        try:
            hosts = netrc.netrc(netrc_path)
        except (netrc.NetrcParseError, OSError):
            hosts = None
        if hosts:
            for machine in ("api.github.com", "github.com"):
                auth = hosts.authenticators(machine)
                if auth and auth[2]:
                    return auth[2]

    if shutil.which("gh"):
        # gh reads its credentials from $HOME, so it needs HOME put back.
        result = subprocess.run(
            ["gh", "auth", "token"],
            capture_output=True,
            text=True,
            env={**os.environ, "HOME": str(HOME)},
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()

    sys.exit(
        "error: no GitHub token found for the private query-correctness-tests repos. "
        "Set $GITHUB_TOKEN, add a github.com entry to ~/.netrc, or run `gh auth login`."
    )


def download(repo: str, commit: str, dest: Path) -> None:
    """Extracts 10gen/<repo>@<commit>'s generated_tests/ tree into `dest`."""

    url = f"https://api.github.com/repos/10gen/{repo}/tarball/{commit}"

    for attempt in range(1, RETRIES + 1):
        if dest.exists():
            shutil.rmtree(dest)
        dest.mkdir(parents=True)

        print(f"downloading {url} (attempt {attempt}/{RETRIES})", flush=True)
        curl = subprocess.Popen(
            ["curl", "-fsSL", "-H", f"Authorization: token {github_token()}", url],
            stdout=subprocess.PIPE,
        )
        # Only generated_tests/ is consumed; skipping the siblings saves multiple
        # GB of disk. The tarball's root directory is named after the repo and
        # commit, so it is matched by wildcard and stripped along with
        # generated_tests/ itself.
        tar = subprocess.Popen(
            [
                "tar",
                "-xz",
                "-C",
                str(dest),
                "--strip-components=2",
                "--wildcards",
                "*/generated_tests/*",
            ],
            stdin=curl.stdout,
        )
        curl.stdout.close()
        tar_rc = tar.wait()
        curl_rc = curl.wait()
        if tar_rc == 0 and curl_rc == 0:
            return

        if attempt == RETRIES:
            sys.exit(f"error: failed to download {url} (curl={curl_rc}, tar={tar_rc})")
        print(f"download failed (curl={curl_rc}, tar={tar_rc}), retrying...", file=sys.stderr)
        time.sleep(RETRY_SLEEP_SEC)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, help="e.g. query-correctness-tests-1")
    parser.add_argument("--conf", required=True, type=Path, help="Path to test_repos.conf.")
    parser.add_argument(
        "--out",
        required=True,
        type=Path,
        help="Output directory to populate with generated_tests/.",
    )
    args = parser.parse_args()

    download(args.repo, read_pin(args.conf, args.repo), args.out / "generated_tests")


if __name__ == "__main__":
    main()
