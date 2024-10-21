#!/usr/bin/env python3
"""Generate network diagnostics information and generate a .txt file."""

import pathlib
import shutil
import subprocess


def generate_netstat():
    if shutil.which("netstat") is None:
        print('Command not found: netstat. Skipping "generate and upload network diagnostics".')
        return

    with open("network_diagnostics.txt", "w") as outfile:
        subprocess.run(["netstat"], stdout=outfile, stderr=subprocess.STDOUT, check=True)


if not pathlib.Path("resmoke_error_code").is_file():
    print('resmoke_error_code not found. Skipping "generate and upload network diagnostics".')
else:
    generate_netstat()
