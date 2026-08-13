#!/usr/bin/env python3

import os
import pathlib
import re
import shlex
import sys

# Pulled from bazel/utils.bzl
_DISTRO_PATTERN_MAP = {
    "Ubuntu 18*": "ubuntu18",
    "Ubuntu 20*": "ubuntu20",
    "Ubuntu 22*": "ubuntu22",
    "Pop!_OS 22*": "ubuntu22",
    "Ubuntu 24*": "ubuntu24",
    "Amazon Linux 2": "amazon_linux_2",
    "Amazon Linux 2023": "amazon_linux_2023",
    "Debian GNU/Linux 10": "debian10",
    "Debian GNU/Linux 12": "debian12",
    "Red Hat Enterprise Linux 8*": "rhel8",
    "Red Hat Enterprise Linux 9*": "rhel9",
    "SLES 15*": "suse15",
    "SLES 16*": "suse16",
}


def get_host_distro_major_version(
    os_release_path: pathlib.Path = pathlib.Path("/etc/os-release"),
    system_release_path: pathlib.Path = pathlib.Path("/etc/system-release"),
    *,
    platform: str | None = None,
) -> str:
    # This code in this function looks a little janky in python.
    # That's because it was pulled from starlark in bazel/utils.bzl to make sure the logic matches exactly.
    if (platform or sys.platform) != "linux":
        return "UNKNOWN"

    try:
        os_release = os_release_path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"Failed to determine system distro, reading os-release failed with the error: {exc}")
        return "UNKNOWN"

    release_values: dict[str, str] = {}
    for line in os_release.splitlines():
        key, separator, raw_value = line.partition("=")
        if not separator or key not in {"NAME", "VERSION_ID"}:
            continue
        try:
            parsed_value = shlex.split(raw_value, comments=True, posix=True)
        except ValueError:
            parsed_value = []
        if len(parsed_value) == 1:
            release_values[key] = parsed_value[0]

    if "NAME" not in release_values or "VERSION_ID" not in release_values:
        print(f"Failed to determine system distro, parsing os-release returned: {os_release}")
        return "UNKNOWN"

    distro_str = "{distro_name} {distro_version}".format(
        distro_name=release_values["NAME"],
        distro_version=release_values["VERSION_ID"],
    )

    for distro_pattern, simplified_name in _DISTRO_PATTERN_MAP.items():
        if "*" in distro_pattern:
            prefix_suffix = distro_pattern.split("*")
            if distro_str.startswith(prefix_suffix[0]) and distro_str.endswith(prefix_suffix[1]):
                return simplified_name
        elif distro_str == distro_pattern:
            if simplified_name == "amazon_linux_2023":
                try:
                    system_release = system_release_path.read_text(encoding="utf-8")
                except OSError:
                    system_release = ""
                minor_match = re.search(
                    r"Amazon Linux release 2023\.([0-9]+)\.",
                    system_release,
                )
                if minor_match and minor_match.group(1) == "3":
                    return "amazon_linux_2023_3"
            return simplified_name
    return "UNKNOWN"


def calculate_local_rbe_container_url() -> str:
    remote_execution_containers = {}
    container_file_path = os.path.join(
        pathlib.Path(__file__).parent.parent.resolve(),
        "bazel",
        "platforms",
        "remote_execution_containers.bzl",
    )
    with open(container_file_path, "r", encoding="utf-8") as f:
        code = compile(f.read(), container_file_path, "exec")
        exec(code, {}, remote_execution_containers)
    host_distro = get_host_distro_major_version()
    if host_distro == "UNKNOWN":
        print("Could not determine host distro, cannot determine local RBE container URL")
        return "UNKNOWN"
    if host_distro not in remote_execution_containers["REMOTE_EXECUTION_CONTAINERS"]:
        print(f"Host distro '{host_distro}' does not have a corresponding RBE container")
        return "UNKNOWN"
    return remote_execution_containers["REMOTE_EXECUTION_CONTAINERS"][host_distro][
        "container-url"
    ].replace("docker://", "")
