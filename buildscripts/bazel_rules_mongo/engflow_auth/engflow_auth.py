"""
Install engflow_auth binary if not on system and authenticate if token is expired.
"""

import hashlib
import json
import os
import platform
import stat
import subprocess
import sys
import time
import urllib.request
from datetime import datetime

from retry import retry

NORMALIZED_ARCH = {"x86_64": "x64", "aarch64": "arm64", "arm64": "arm64", "AMD64": "x64"}

NORMALIZED_OS = {"Windows": "windows", "Darwin": "macos", "Linux": "linux"}

CHECKSUMS = {
    "engflow_auth_linux_arm64": "ad5ffee1e6db926f5066aa40ee35517b1993851d0063ac121dbf5b407c81e2bf",
    "engflow_auth_linux_x64": "b731bae21628b2be321c24b342854c6ed1ed0326010e62a2ecf0b5650a56cf1a",
    "engflow_auth_macos_arm64": "69057929b4515d41b1af861c9bfdbc47427cc5ce5a80c94d4776c8bef672292e",
    "engflow_auth_macos_x64": "a322373e41faa7750c34348f357c5a4a670a66cfd988e80b4343c72822d91292",
    "engflow_auth_windows_x64.exe": "cb9590ffcc6731389ded173250f604b37778417450b1dc92c6bafadeef342826",
}
GH_URL_PREFIX = "https://github.com/EngFlow/auth/releases/download/v0.0.13/"
CLUSTER = "sodalite.cluster.engflow.com"

LOGIN_TIMEOUT = 300
CLI_WAIT_TIMEOUT = 5


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


def check_hash(binary_path: str, tag: str) -> bool:
    with open(binary_path, "rb") as f:
        digest = hashlib.file_digest(f, "sha256")
    return digest.hexdigest() == CHECKSUMS[tag]


def install(verbose: bool) -> str:
    binary_directory = os.path.expanduser("~/.local/bin")
    os.makedirs(binary_directory, exist_ok=True)
    binary_filename = "engflow_auth"
    binary_path = os.path.join(binary_directory, binary_filename)
    tag = get_release_tag()
    if "windows" in tag:
        binary_path += ".exe"
    if os.path.exists(binary_path):
        if verbose:
            print(f"{binary_filename} already exists at {binary_path}, skipping download")
    else:
        url = GH_URL_PREFIX + tag
        print(f"Downloading {url}...")
        download(url, binary_path)
        os.chmod(binary_path, stat.S_IRWXU)
        if not check_hash(binary_path, tag):
            print("File hash doesn't match expected checksum. Removing...")
            print("Please raise the issue in #ask-devprod-build")
            os.remove(binary_path)
            raise Exception("Checksum failure")
        print(f"Downloaded {binary_path}")

    return binary_path


def update_bazelrc(binary_path: str, verbose: bool):
    norm_path = os.path.normpath(binary_path).replace("\\", "/")
    lines = []
    bazelrc_path = f"{os.path.expanduser('~')}/.bazelrc"
    if verbose:
        print(f"Updating {bazelrc_path}")
    if os.path.exists(bazelrc_path):
        with open(bazelrc_path, "r") as bazelrc:
            for line in bazelrc.readlines():
                if "--tls_client" in line or "--credential_helper" in line:
                    continue
                lines.append(line)
    lines.append(f"common --credential_helper={CLUSTER}={norm_path}")

    with open(bazelrc_path, "w+") as bazelrc:
        bazelrc.writelines(lines)


def authenticate(binary_path: str, verbose: bool) -> bool:
    need_login = False
    p = subprocess.run(f"{binary_path} export {CLUSTER}", shell=True, capture_output=True)
    if p.returncode != 0:
        need_login = True
    else:
        expiry_iso = json.loads(p.stdout)["token"]["expiry"][:23]
        if datetime.now() > datetime.fromisoformat(expiry_iso):
            need_login = True
    if not need_login:
        if verbose:
            print("Already authenticated. Skipping authentication.")
        return True

    try:
        print(
            "Attempting to authenticate with MongoDB remote build service. On any device, login via the following link to complete EngFlow authentication:\nGenerating link url and opening browser in 10 seconds:\n(use CTRL+C or COMMAND+C to skip if not an internal mongodb user)"
        )
        countdown = 10
        for i in reversed(range(countdown + 1)):
            if i == 9:
                sys.stdout.write("\b\b  \b\b")
            elif i < 9:
                sys.stdout.write("\b \b")
            if i == 0:
                break
            sys.stdout.write(str(i))
            sys.stdout.flush()
            time.sleep(1)

        p = subprocess.Popen(
            f"{binary_path} login -store=file {CLUSTER}",
            shell=True,
            stderr=subprocess.PIPE,
            universal_newlines=True,
        )

        login_url = None

        start_time = time.time()
        while not login_url and time.time() < start_time + CLI_WAIT_TIMEOUT:
            line = p.stderr.readline().strip()
            if line.startswith("https://" + CLUSTER):
                login_url = line
                break

        if not login_url:
            print("CLI had unexpected output.")
            p.kill()
            return False
        else:
            print(login_url)

        try:
            p.wait(timeout=LOGIN_TIMEOUT)
            print("Sucessfully authenticated with EngFlow!")

        except subprocess.TimeoutExpired:
            print(
                "Timed out waiting for login attempt. Failed to authenticate with EngFlow. Builds will be run locally..."
            )
            p.kill()
            return False

    except KeyboardInterrupt:
        print(
            "Skipping authentication, use '--config=local' to skip trying to authenticate in the future."
        )
        time.sleep(3)
        return False

    return True


def setup_auth(verbose: bool = True) -> bool:
    path = install(verbose)
    authenticated = authenticate(path, verbose)
    if not authenticated:
        return False
    update_bazelrc(path, verbose)
    return True


def main():
    return 0 if setup_auth() else 1


if __name__ == "__main__":
    main()
