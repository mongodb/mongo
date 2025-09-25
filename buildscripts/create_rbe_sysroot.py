#!/usr/bin/env python3

import getpass
import os
import shutil
import subprocess
import sys

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.local_rbe_container_url import calculate_local_rbe_container_url


def main():
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))
    container_url = calculate_local_rbe_container_url()
    if container_url == "UNKNOWN":
        print("Could not determine local RBE container URL, cannot create rbe sysroot")
        return 1

    print(f"Using local RBE container URL: {container_url}")

    container_cli = shutil.which("docker") or shutil.which("podman")
    if not container_cli:
        print("Error: Neither docker nor podman is installed.", file=sys.stderr)
        sys.exit(1)

    cid = subprocess.check_output([container_cli, "create", container_url]).decode().strip()

    os.makedirs("./rbe_sysroot", exist_ok=True)

    subprocess.run(["sudo", container_cli, "cp", f"{cid}:/", "./rbe_sysroot/"], check=True)

    user = getpass.getuser()
    subprocess.run(["sudo", "chown", "-R", f"{user}:{user}", "./rbe_sysroot"], check=True)
    subprocess.run([container_cli, "rm", cid], check=True)

    return 0


if __name__ == "__main__":
    exit(main())
