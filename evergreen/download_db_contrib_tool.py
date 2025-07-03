import gzip
import os
import pathlib
import platform
import re
import stat
import sys
from urllib.request import urlretrieve

import retry

mongo_path = pathlib.Path(__file__).parents[1]
sys.path.append(str(mongo_path))
from buildscripts.util.expansions import get_expansion

DB_CONTRIB_TOOL_VERSION = "v1.1.4"
RELEASE_URL = (
    f"https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/{DB_CONTRIB_TOOL_VERSION}/"
)


def get_binary_name() -> str:
    # Get the binary name in s3 for the current platform/architecture
    operating_system = platform.system().lower()
    machine = platform.machine().lower()
    if machine == "aarch64":
        machine = "arm64"

    if machine == "amd64" or machine == "x86_64":
        machine = "x64"

    # On ppc64le RHEL, pyinstaller builds are not forward compatible (At least not from RHEL 8 -> 9),
    # so we need to create and name each binary with the exact OS.
    if machine == "ppc64le":
        # Get the major version from the release string
        # like '4.18.0-513.5.1.el8_9.ppc64le' -> major release 8
        major_version_match = re.findall("el(\d+)", os.uname().release)
        assert len(major_version_match) == 1
        major_version = int(major_version_match[0])

        # db-contrib-tool 1.0.1 is only released up to RHEL 9 on PPC64LE
        major_version = min(major_version, 9)

        operating_system = f"rhel{major_version}"

    binary_name = f"db-contrib-tool_{DB_CONTRIB_TOOL_VERSION}_{operating_system}_{machine}"
    if operating_system == "windows":
        binary_name = f"{binary_name}.exe"

    return binary_name


@retry.retry(tries=3, delay=3)
def download_binary(url: str, path: str) -> None:
    urlretrieve(url, path)


def main() -> int:
    binary_name = get_binary_name()
    gz_name = f"{binary_name}.gz"
    binary_url = f"{RELEASE_URL}{gz_name}"
    download_binary(binary_url, gz_name)
    # extract the binary
    with gzip.open(gz_name, "rb") as fin:
        with open(binary_name, "wb") as fout:
            fout.write(fin.read())
    os.remove(gz_name)
    workdir = get_expansion("workdir")
    final_name = "db-contrib-tool"
    if platform.machine().lower() == "windows":
        final_name += ".exe"
    # we add ${workdir}/bin to the PATH in prelude.sh
    final_path = os.path.join(workdir, "bin", final_name)
    # put the binary at ${workdir}/bin/db-contrib-tool(.exe)
    os.rename(binary_name, final_path)
    os.chmod(final_path, os.stat(final_path).st_mode | stat.S_IEXEC)


if __name__ == "__main__":
    sys.exit(main())
