#!/usr/bin/env python3

import os
import pathlib
import subprocess
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
}


def get_host_distro_major_version():
    # This code in this function looks a little janky in python.
    # That's because it was pulled from starlark in bazel/utils.bzl to make sure the logic matches exactly.
    if sys.platform != "linux":
        return "UNKNOWN"

    result = subprocess.run(
        [
            "sed",
            "-n",
            '/^\\(NAME\\|VERSION_ID\\)=/{s/[^=]*=//;s/"//g;p}',
            "/etc/os-release",
        ],
        check=True,
        capture_output=True,
        text=True,
    )

    if result.returncode != 0:
        print(
            f"Failed to determine system distro, parsing os-release failed with the error: {result.stderr}"
        )
        return "UNKNOWN"

    distro_seq = result.stdout.splitlines()
    if len(distro_seq) != 2:
        print(f"Failed to determine system distro, parsing os-release returned: {result.stdout}")
        return "UNKNOWN"

    distro_str = "{distro_name} {distro_version}".format(
        distro_name=distro_seq[0],
        distro_version=distro_seq[1],
    )

    for distro_pattern, simplified_name in _DISTRO_PATTERN_MAP.items():
        if "*" in distro_pattern:
            prefix_suffix = distro_pattern.split("*")
            if distro_str.startswith(prefix_suffix[0]) and distro_str.endswith(prefix_suffix[1]):
                return simplified_name
        elif distro_str == distro_pattern:
            return simplified_name
    return "UNKNOWN"


def calculate_local_rbe_container_url():
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
