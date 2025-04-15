# Use to update mongo_toolchain_version.bzl with hashes of a new toolchain from the toolchain-builder project.

import argparse
import hashlib
import os
import pathlib
import tempfile

import requests
from requests.adapters import HTTPAdapter, Retry

PATCH_TOOLCHAIN_URL_FORMAT = (
    "https://mciuploads.s3.amazonaws.com/toolchain-builder/"
    "{platform_name}/79c9b62fe59b85252bd716333ebea111b4d03a12/{component}_builder_{underscore_platform_name}_{build_id}.tar.gz"
)
TOOLCHAIN_URL_FORMAT = "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/{component}-{platform_name}-{build_id}.tar.gz"
COMPONENT_MAP = {
    "compiler": "bazel_{version}_toolchain",
    "gdb": "bazel_{version}_gdb",
}

COMPONENT_FILE_MAP = {
    "compiler": "mongo_toolchain_version_{version}.bzl",
    "gdb": "mongo_gdb_version_{version}.bzl",
}

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
    "rhel9_ppc64le": "rhel90-ppc64le",
    "rhel9_s390x": "rhel90-zseries",
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
    toolchain_versions = ["v4", "v5"]
    toolchain_components = ["compiler", "gdb"]

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "build_id",
        help="The build id, this should be the toolchain revision (githash), or the evergreen task id (version and date) if it is a --patch toolchain.",
    )
    parser.add_argument(
        "toolchain_version", choices=toolchain_versions + ["all"], help="Toolchain version"
    )
    parser.add_argument(
        "toolchain_component", choices=toolchain_components + ["all"], help="Toolchain component"
    )
    parser.add_argument(
        "--patch_toolchain",
        action="store_true",
        help="With the path URL should be used instead of the production URL.",
    )
    args = parser.parse_args()

    if args.toolchain_version == "all":
        toolchain_versions_to_run = toolchain_versions
    else:
        toolchain_versions_to_run = [args.toolchain_version]

    if args.toolchain_component == "all":
        toolchain_components_to_run = toolchain_components
    else:
        toolchain_components_to_run = [args.toolchain_component]

    for version_str in toolchain_versions_to_run:
        for component in toolchain_components_to_run:
            mongo_toolchain_version = {}
            version_file_path = os.path.join(
                pathlib.Path(__file__).parent.resolve(),
                COMPONENT_FILE_MAP[component].format(version=version_str),
            )
            toolchain_found = False
            for toolchain_name, platform_name in PLATFORM_NAME_MAP.items():
                underscore_platform_name = platform_name.replace("-", "_")
                if args.patch_toolchain:
                    toolchain_url_format = PATCH_TOOLCHAIN_URL_FORMAT
                else:
                    toolchain_url_format = TOOLCHAIN_URL_FORMAT

                toolchain_url = toolchain_url_format.format(
                    version=version_str,
                    platform_name=platform_name,
                    underscore_platform_name=underscore_platform_name,
                    build_id=args.build_id,
                    component=COMPONENT_MAP[component].format(version=version_str),
                )

                temp_dir = tempfile.gettempdir()
                local_tarball_path = os.path.join(
                    temp_dir,
                    f"{COMPONENT_MAP[component].format(version=version_str)}_{underscore_platform_name}_{args.build_id}.tar.gz",
                )

                if not download_toolchain(toolchain_url, local_tarball_path):
                    print(
                        f"Toolchain {version_str}_{component} on {toolchain_name} for {platform_name} not available, skipping"
                    )
                    continue
                toolchain_found = True
                sha = sha256_file(local_tarball_path)
                os.remove(local_tarball_path)

                mongo_toolchain_version[toolchain_name] = {
                    "platform_name": platform_name,
                    "sha": sha,
                    "url": toolchain_url,
                }
            if toolchain_found:
                with open(version_file_path, "w") as f:
                    print(f"Writing toolchain map to {version_file_path}...")
                    print(
                        "# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.\n",
                        file=f,
                    )
                    print(f'TOOLCHAIN_ID = "{args.build_id}"', file=f)
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
