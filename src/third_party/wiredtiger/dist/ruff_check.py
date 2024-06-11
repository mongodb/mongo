#!/usr/bin/env python3

import pathlib
import re
from shutil import which
import subprocess
import sys

# Always use this script's folder as the working directory.
current_dir = pathlib.Path(__file__).parent.resolve()
ruff_config = f"{current_dir}/ruff.toml"


def run(cmd):
    try:
        output = subprocess.check_output(cmd, cwd=current_dir).decode().strip()
        if output == "All checks passed!":
            return True
        else:
            print(output)
            return False
    except subprocess.CalledProcessError as cpe:
        print("The command [%s] failed:\n%s" % (" ".join(cmd), cpe.output.decode("utf-8")))
        return False


if not which("ruff"):
    doc_link = "https://docs.astral.sh/ruff/installation/"
    lines = []
    ruff_version = None
    with open(ruff_config) as file:
        for line in file:
            m = re.search(r'required-version = "==(\d.\d.\d)"', line.strip())
            if m:
                ruff_version = m.group(1)
                break
    print("Ruff is not installed!")
    print(f"Please install Ruff using the official documentation: {doc_link}\n")
    print("Suggested steps using a virtual environment:")
    print(f"virtualenv -p python3 {current_dir.parent}/venv")
    print(f"source {current_dir.parent}/venv/bin/activate")
    print(f"python3 -m pip install ruff=={ruff_version}\n")
    sys.exit(1)

cmd = ["ruff", "check", "--fix", "../.", ".", "--config", ruff_config]

if not run(cmd):
    sys.exit(1)
