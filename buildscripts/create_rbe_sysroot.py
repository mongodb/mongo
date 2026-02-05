#!/usr/bin/env python3

import getpass
import os
import shutil
import subprocess
import sys

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.local_rbe_container_url import calculate_local_rbe_container_url


def create_rbe_sysroot(dir) -> bool:
    container_url = calculate_local_rbe_container_url()
    if container_url == "UNKNOWN":
        print("Could not determine local RBE container URL, cannot create rbe sysroot")
        return False

    print(f"Using local RBE container URL: {container_url}")

    container_cli = shutil.which("docker") or shutil.which("podman")
    if not container_cli:
        print("Error: Neither docker nor podman is installed.", file=sys.stderr)
        return False

    cid = subprocess.check_output([container_cli, "create", container_url]).decode().strip()

    os.makedirs(dir, exist_ok=True)

    subprocess.run(["sudo", container_cli, "cp", f"{cid}:/", dir], check=True)

    user = getpass.getuser()
    subprocess.run(["sudo", "chown", "-R", f"{user}:{user}", dir], check=True)
    subprocess.run([container_cli, "rm", cid], check=True)

    return True


def main():
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    success = create_rbe_sysroot("./rbe_sysroot")
    return not success


if __name__ == "__main__":
    sys.exit(main())
