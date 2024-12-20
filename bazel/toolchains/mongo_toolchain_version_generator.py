# Use to update mongo_toolchain_version.bzl with hashes of a new toolchain from the toolchain-builder project.

import argparse
import hashlib
import os
import pathlib
import tempfile

import requests
from requests.adapters import HTTPAdapter, Retry

TOOLCHAIN_URL_FORMAT = (
    "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/"
    "bazel_{version}_toolchain_builder_"
    "{underscore_platform_name}_{patch_build_id}_{patch_build_date}"
    ".tar.gz"
)
PLATFORM_NAME_MAP = {
    "amazon_linux_2023_aarch64": "amazon2023-arm64",
    "amazon_linux_2023_x86_64": "amazon2023",
    "amazon_linux_2_aarch64": "amazon2-arm64",
    "amazon_linux_2_x86_64": "amazon2",
    "debian10_x86_64": "debian10",
    "debian12_x86_64": "debian12",
    "rhel8_aarch64": "rhel82-arm64",
    "rhel8_ppc64le": "rhel81-ppc64le",
    "rhel8_s390x": "rhel80-zseries",
    "rhel8_x86_64": "rhel80",
    "rhel9_aarch64": "rhel90-arm64",
    "rhel9_x86_64": "rhel90",
    "suse15_x86_64": "suse15",
    "ubuntu18_x86_64": "ubuntu1804",
    "ubuntu20_aarch64": "ubuntu2004-arm64",
    "ubuntu20_x86_64": "ubuntu2004",
    "ubuntu22_aarch64": "ubuntu2204-arm64",
    "ubuntu22_x86_64": "ubuntu2204",
    "ubuntu24_aarch64": "ubuntu2404-arm64",
    "ubuntu24_x86_64": "ubuntu2404",
}

REQUESTS_SESSION = requests.Session()
REQUESTS_SESSION.mount(
    "https://",
    HTTPAdapter(max_retries=Retry(total=5, backoff_factor=1, status_forcelist=[502, 503, 504])),
)


def download_toolchain(toolchain_url: str, local_path: str) -> bool:
    print(f"Downloading {toolchain_url}...")

    response = REQUESTS_SESSION.get(toolchain_url)
    if response.status_code != requests.codes.ok:
        print(f"WARNING: HTTP {response.status_code} status downloading {toolchain_url}")
        return False

    with open(local_path, "wb") as f:
        f.write(response.content)

    return True


def sha256_file(filename: str) -> str:
    sha256_hash = hashlib.sha256()
    with open(filename, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(block)
        return sha256_hash.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("patch_build_id", help="Patch build id from toolchain-builder project.")
    parser.add_argument(
        "patch_build_date_string",
        help="Patch build date string from toolchain-builder project, get this at the task URL, ex the date is 24_01_09_16_10_07 for https://spruce.mongodb.com/task/toolchain_builder_amazon2023_compile_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07/",
    )
    parser.add_argument("toolchain_version", help="Toolchain version e.g., v4")

    args = parser.parse_args()

    version_str = args.toolchain_version
    mongo_toolchain_version = {}
    version_file_path = os.path.join(
        pathlib.Path(__file__).parent.resolve(), f"mongo_toolchain_version_{version_str}.bzl"
    )

    for toolchain_name, platform_name in PLATFORM_NAME_MAP.items():
        underscore_platform_name = platform_name.replace("-", "_")

        toolchain_url = TOOLCHAIN_URL_FORMAT.format(
            version=version_str,
            platform_name=platform_name,
            underscore_platform_name=underscore_platform_name,
            patch_build_id=args.patch_build_id,
            patch_build_date=args.patch_build_date_string,
        )

        temp_dir = tempfile.gettempdir()
        local_tarball_path = os.path.join(
            temp_dir,
            f"bazel_{version_str}_toolchain_builder_{underscore_platform_name}_{args.patch_build_id}.tar.gz",
        )

        if not download_toolchain(toolchain_url, local_tarball_path):
            print(f"Toolchain {toolchain_name} for {platform_name} not available, skipping")
            continue
        sha = sha256_file(local_tarball_path)
        os.remove(local_tarball_path)

        mongo_toolchain_version[toolchain_name] = {
            "platform_name": platform_name,
            "sha": sha,
            "url": toolchain_url,
        }

    with open(version_file_path, "w") as f:
        print(f"Writing toolchain map to {version_file_path}...")
        print(
            "# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.\n",
            file=f,
        )
        print(f'TOOLCHAIN_PATCH_BUILD_ID = "{args.patch_build_id}"', file=f)
        print(f'TOOLCHAIN_PATCH_BUILD_DATE = "{args.patch_build_date_string}"', file=f)
        print(f"TOOLCHAIN_MAP_{version_str.upper()} = {{", file=f)
        for key, value in sorted(mongo_toolchain_version.items(), key=lambda x: x[0]):
            print(f'    "{key}": {{', file=f)
            for subkey, subvalue in sorted(value.items(), key=lambda x: x[0]):
                print(f'        "{subkey}": "{subvalue}",', file=f)
            print("    },", file=f)
        print("}", file=f)

    with open(version_file_path, "r") as f:
        print(f"Finished writing to {version_file_path}:")
        print(f.read())


if __name__ == "__main__":
    main()
