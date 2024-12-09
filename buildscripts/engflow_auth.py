"""
Install engflow_auth binary if not on system and authenticate if token is expired.
"""

import json
import os
import platform
import stat
import subprocess
import urllib.request
from datetime import datetime

from retry import retry

NORMALIZED_ARCH = {
    "x86_64": "x64",
    "aarch64": "arm64",
}

NORMALIZED_OS = {"Windows": "windows", "Darwin": "macos", "Linux": "linux"}

GH_URL_PREFIX = "https://github.com/EngFlow/auth/releases/latest/download/"
CLUSTER = "sodalite.cluster.engflow.com"


@retry(tries=3, delay=1)
def download(url: str, path: str):
    urllib.request.urlretrieve(url, filename=path)


def get_release_tag():
    tag = "engflow_auth"
    sys = platform.system()
    os_sys = NORMALIZED_OS.get(sys, None)
    machine = platform.machine()
    arch = NORMALIZED_ARCH.get(machine, None)
    if not os_sys or not arch:
        raise Exception(f"{sys} {machine} not supported")

    if os_sys == "windows":
        return tag + "_windows_x64.exe"
    return tag + f"_{os_sys}_{arch}"


def install() -> str:
    binary_directory = os.path.expanduser("~/.local/bin")
    binary_filename = "engflow_auth"
    binary_path = os.path.join(binary_directory, binary_filename)
    tag = get_release_tag()
    if os.path.exists(binary_path):
        print(f"{binary_filename} already exists at {binary_path}, skipping download")
    else:
        url = GH_URL_PREFIX + tag
        print(f"Downloading {url}...")
        download(url, binary_path)
        os.chmod(binary_path, stat.S_IEXEC)
        print(f"Downloaded {binary_path}")

    return binary_path


def update_bazelrc(binary_path: str):
    with open(f"{os.path.expanduser('~')}/.bazelrc", "a+") as bazelrc:
        lines = []
        for line in bazelrc.readlines():
            if "--tls_client" in line or "--credential_helper" in line:
                pass
            lines.append(line)
        lines.append(f"build --credential_helper={CLUSTER}={binary_path}")
        bazelrc.writelines(lines)


def authenticate(binary_path: str):
    need_login = False
    p = subprocess.run(f"{binary_path} export {CLUSTER}", shell=True, capture_output=True)
    if p.returncode != 0:
        need_login = True
    else:
        expiry_iso = json.loads(p.stdout)["token"]["expiry"][:23]
        if datetime.now() > datetime.fromisoformat(expiry_iso):
            need_login = True
    if need_login:
        p = subprocess.run(
            f"{binary_path} login -store=file {CLUSTER}",
            check=True,
            shell=True,
            capture_output=True,
        )


def main():
    path = install()
    authenticate(path)
    update_bazelrc(path)


if __name__ == "__main__":
    main()
