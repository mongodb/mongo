#!/opt/mongodbtoolchain/v4/bin/python3
"""Wraps clang tidy to include our custom checks."""

# TODO: if https://github.com/notskm/vscode-clang-tidy/pull/77#issuecomment-1422910143 is resolved then this script can be removed

import subprocess
import sys
import os

CHECKS_SO = "build/install/lib/libmongo_tidy_checks.so"


def main():
    clang_tidy_args = ["/opt/mongodbtoolchain/v4/bin/clang-tidy"]
    if os.path.isfile(CHECKS_SO):
        clang_tidy_args += [f"-load={CHECKS_SO}"]
    clang_tidy_args += sys.argv[1:]
    proc = subprocess.run(clang_tidy_args, capture_output=True)
    # Write to output buffer here because that is how to copy directly from stdin to stdout without making assumptions about encoding
    sys.stdout.buffer.write(proc.stdout)
    sys.stderr.buffer.write(proc.stderr)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
