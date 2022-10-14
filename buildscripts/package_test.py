import argparse
import dataclasses
import json
import logging
import os
import platform
import sys
import time
import traceback
import uuid

from concurrent import futures
from pathlib import Path
from typing import Any, Dict, Generator, List, Optional, Tuple, Set

import docker
import docker.errors
import requests

from simple_report import Result, Report

root = logging.getLogger()
root.setLevel(logging.DEBUG)

handler = logging.StreamHandler(sys.stdout)
handler.setLevel(logging.DEBUG)
formatter = logging.Formatter('[%(asctime)s]%(levelname)s:%(message)s')
handler.setFormatter(formatter)
root.addHandler(handler)

PACKAGE_MANAGER_COMMANDS = {
    "apt": {
        "update": "export DEBIAN_FRONTEND=noninteractive; apt-get update -y",
        "install": "export DEBIAN_FRONTEND=noninteractive; apt-get install -y {}",
    },
    "yum": {
        "update": "yum update -y",
        "install": "yum install -y {}",
    },
    "zypper": {
        "update": "zypper -n update",
        "install": "zypper -n install {}",
    },
}

DOCKER_SYSTEMCTL_REPO = "https://raw.githubusercontent.com/gdraheim/docker-systemctl-replacement"
SYSTEMCTL_URL = f"{DOCKER_SYSTEMCTL_REPO}/master/files/docker/systemctl3.py"
JOURNALCTL_URL = f"{DOCKER_SYSTEMCTL_REPO}/master/files/docker/journalctl3.py"

# Lookup table used when building and running containers
# os_name, Optional[(base_image, package_manager, frozenset(base_packages), python_command)]
OS_DOCKER_LOOKUP = {
    'amazon': None,
    'amzn64': None,
    # TODO(SERVER-69982) This can be reenabled when the ticket is fixed
    # 'amazon': ('amazonlinux:1', "yum", frozenset(["python38", "wget", "pkgconfig", "systemd"]), "python3"),
    # 'amzn64': ('amazonlinux:1', "yum", frozenset(["python38", "wget", "pkgconfig", "systemd"]), "python3"),
    'amazon2': ('amazonlinux:2', "yum", frozenset(["python3", "wget", "pkgconfig", "systemd"]),
                "python3"),
    'amazon2022': ('amazonlinux:2022', "yum", frozenset(["python3", "wget", "pkgconfig",
                                                         "systemd"]), "python3"),
    'debian10': ('debian:10-slim', "apt", frozenset(["python3", "wget", "pkg-config", "systemd"]),
                 "python3"),
    'debian11': ('debian:11-slim', "apt", frozenset(["python3", "wget", "pkg-config", "systemd"]),
                 "python3"),
    'debian71': ('debian:7-slim', "apt", frozenset(["python3", "wget", "pkg-config", "systemd"]),
                 "python3"),
    'debian81': ('debian:8-slim', "apt", frozenset(["python3", "wget", "pkg-config", "systemd"]),
                 "python3"),
    'debian92': ('debian:9-slim', "apt", frozenset(["python3", "wget", "pkg-config", "systemd"]),
                 "python3"),
    'linux_i686': None,
    'linux_x86_64': None,
    'macos': None,
    'osx': None,
    'osx-ssl': None,
    'rhel55': None,
    'rhel57': None,
    'rhel62': None,
    'rhel70': ('registry.access.redhat.com/ubi7/ubi:7.9', "yum",
               frozenset(["rh-python38.x86_64", "wget", "pkgconfig", "systemd"]),
               "/opt/rh/rh-python38/root/usr/bin/python3"),
    'rhel71': ('registry.access.redhat.com/ubi7/ubi:7.9', "yum",
               frozenset(["rh-python38.x86_64", "wget", "pkgconfig", "systemd"]),
               "/opt/rh/rh-python38/root/usr/bin/python3"),
    'rhel72': ('registry.access.redhat.com/ubi7/ubi:7.9', "yum",
               frozenset(["rh-python38.x86_64", "wget", "pkgconfig", "systemd"]),
               "/opt/rh/rh-python38/root/usr/bin/python3"),
    'rhel80': ('redhat/ubi8', "yum", frozenset(["python3", "wget", "pkgconfig", "systemd"]),
               "python3"),
    'rhel81': ('redhat/ubi8', "yum", frozenset(["python3", "wget", "pkgconfig", "systemd"]),
               "python3"),
    'rhel82': ('redhat/ubi8', "yum", frozenset(["python3", "wget", "pkgconfig", "systemd"]),
               "python3"),
    'rhel83': ('redhat/ubi8', "yum", frozenset(["python3", "wget", "pkgconfig", "systemd"]),
               "python3"),
    'rhel90': ('redhat/ubi9', "yum", frozenset(["python3", "wget", "pkgconfig", "systemd"]),
               "python3"),
    'sunos5': None,
    'suse11': None,
    'suse12': None,
    # ('registry.suse.com/suse/sles12sp5:latest', "zypper", frozenset(["python3", "wget", "pkg-config", "systemd"]), "python3"),
    # The offical repo fails with the following error
    # Problem retrieving the repository index file for service 'container-suseconnect-zypp':
    # [container-suseconnect-zypp|file:/usr/lib/zypp/plugins/services/container-suseconnect-zypp]
    'suse15': ('opensuse/leap:15', "zypper", frozenset(["python3", "wget", "pkg-config",
                                                        "systemd"]), "python3"),
    # 'suse15': ('registry.suse.com/suse/sle15:latest', "zypper", frozenset(["python3", "wget", "pkg-config", "systemd"]), "python3"),
    # Has the same error as above
    'ubuntu1204': None,
    'ubuntu1404': None,
    'ubuntu1604': ('ubuntu:16.04', "apt", frozenset(["python3", "wget", "pkg-config", "systemd"]),
                   "python3"),
    'ubuntu1804': ('ubuntu:18.04', "apt", frozenset(["python3", "wget", "pkg-config", "systemd"]),
                   "python3"),
    'ubuntu2004': ('ubuntu:20.04', "apt", frozenset(["python3", "wget", "pkg-config", "systemd"]),
                   "python3"),
    'ubuntu2204': ('ubuntu:22.04', "apt", frozenset(["python3", "wget", "pkg-config", "systemd"]),
                   "python3"),
    'windows': None,
    'windows_i686': None,
    'windows_x86_64': None,
    'windows_x86_64-2008plus': None,
    'windows_x86_64-2008plus-ssl': None,
    'windows_x86_64-2012plus': None,
}

VERSIONS_TO_SKIP = set(['3.0.15', '3.2.22', '3.4.24', '3.6.23', '4.0.28'])

# TODO(SERVER-70016) These can be deleted once these versions are no longer is current.json
DISABLED_TESTS = [("amazon2", "4.4.16"), ("amazon2", "4.4.17-rc2"), ("amazon2", "4.2.23-rc0"),
                  ("amazon2", "4.2.23-rc1"), ("amazon2", "4.2.22")]


@dataclasses.dataclass
class Test:
    """Class to track a single test."""

    os_name: str
    version: str
    base_image: str = dataclasses.field(default="", repr=False)
    package_manager: str = dataclasses.field(default="", repr=False)
    update_command: str = dataclasses.field(default="", repr=False)
    install_command: str = dataclasses.field(default="", repr=False)
    base_packages: str = dataclasses.field(default="", repr=False)
    python_command: str = dataclasses.field(default="", repr=False)
    packages_urls: List[str] = dataclasses.field(default_factory=list)
    packages_paths: List[Path] = dataclasses.field(default_factory=list)

    def __post_init__(self) -> None:
        assert OS_DOCKER_LOOKUP[self.os_name] is not None

        self.base_image = OS_DOCKER_LOOKUP[self.os_name][0]
        self.package_manager = OS_DOCKER_LOOKUP[self.os_name][1]
        self.base_packages = OS_DOCKER_LOOKUP[self.os_name][2]
        self.python_command = OS_DOCKER_LOOKUP[self.os_name][3]

        self.update_command = PACKAGE_MANAGER_COMMANDS[self.package_manager]["update"]
        self.install_command = PACKAGE_MANAGER_COMMANDS[self.package_manager]["install"]

    def __repr__(self) -> str:
        ret = f"\nTest:\n\tos: {self.os_name}\n"
        ret += f"image: {self.base_image}\n"
        ret += f"\tversion: {self.version}\n"
        ret += f"\tpackages_urls: {self.packages_urls}\n"
        ret += f"\tpackages_paths: {self.packages_paths}\n"
        return ret

    def name(self) -> str:
        return self.os_name + "-" + self.version


def build_image(test: Test) -> str:
    commands: List[str] = [
        test.update_command,
        test.install_command.format(" ".join(test.base_packages)),
        "mkdir -p /run/systemd/system",
        "mkdir -p $(pkg-config systemd --variable=systemdsystempresetdir)",
        "echo 'disable *' > $(pkg-config systemd --variable=systemdsystempresetdir)/00-test.preset",
        f"wget -P /usr/bin {SYSTEMCTL_URL}",
        f"wget -P /usr/bin {JOURNALCTL_URL}",
        "chmod +x /usr/bin/systemctl3.py /usr/bin/journalctl3.py",
        "ln -sf /usr/bin/systemctl3.py /bin/systemd",
        "ln -sf /usr/bin/systemctl3.py /usr/bin/systemd",
    ]

    if test.python_command != 'python3':
        commands.append(f"ln -s {test.python_command} /usr/bin/python3")

    logging.info("Building base image for %s: %s", test.os_name, test.base_image)

    client = docker.from_env()
    container = client.containers.run(
        test.base_image, ["/bin/bash", "-x", "-c", " && ".join(commands)], detach=True, tty=True)

    # Wait for the container to finish and exit (timeout is in seconds)
    container.wait(timeout=120)
    logging.debug(container.logs().decode('utf-8'))
    return container.commit(repository=f"localhost/{test.os_name}", changes="CMD /bin/systemd")


def run_test(test: Test) -> Result:
    result = Result(status="pass", test_file=test.name(), start=time.time(), log_raw="")
    client = docker.from_env()

    log_name = f"logs/{test.os_name}_{test.version}_{uuid.uuid4().hex}.log"
    test_docker_root = Path("/mnt/package_test").resolve()
    log_docker_path = Path.joinpath(test_docker_root, log_name)
    test_external_root = Path(__file__).parent.resolve()
    logging.debug(test_external_root)
    log_external_path = Path.joinpath(test_external_root, log_name)

    os.makedirs(log_external_path.parent, exist_ok=True)
    command = f"bash -c \"{test.python_command} /mnt/package_test/package_test_internal.py {log_docker_path} {' '.join(test.packages_urls)}\""
    logging.debug("Attemtping to run the following docker command: %s", command)

    try:
        image = build_image(test)
        container: docker.Container = client.containers.run(
            image, command=command, auto_remove=True, detach=True,
            volumes=[f'{test_external_root}:{test_docker_root}'])
        for log in container.logs(stream=True):
            result["log_raw"] += log.decode('UTF-8')
            # This is pretty verbose, lets run this way for a while and we can delete this if it ends up being too much
            logging.debug(log.decode('UTF-8').strip())
        exit_code = container.wait()
        result["exit_code"] = exit_code['StatusCode']
    except docker.errors.APIError as exc:
        traceback.print_exception(type(exc), exc, exc.__traceback__)
        logging.error("Failed to start docker container")
        result["end"] = time.time()
        result['status'] = 'fail'
        result["exit_code"] = 1
        return result

    try:
        with open(log_external_path, 'r') as log_raw:
            result["log_raw"] += log_raw.read()
    except OSError as oserror:
        logging.error("Failed to open %s with error %s", log_external_path, oserror)

    if exit_code['StatusCode'] != 0:
        logging.error("Failed test %s with exit code %s", test, exit_code)
        result['status'] = 'fail'
    result["end"] = time.time()
    return result


logging.info("Attemping to download current mongo releases json")
r = requests.get('https://downloads.mongodb.org/current.json')
current_releases = r.json()

logging.info("Attemping to download current mongo tools releases json")
r = requests.get('https://downloads.mongodb.org/tools/db/release.json')
current_tools_releases = r.json()

logging.info("Attemping to download current mongosh releases json")
r = requests.get('https://s3.amazonaws.com/info-mongodb-com/com-download-center/mongosh.json')
mongosh_releases = r.json()


def iterate_over_downloads() -> Generator[Dict[str, Any], None, None]:
    for version in current_releases["versions"]:
        for download in version["downloads"]:
            if download["edition"] == "source":
                continue
            if version["version"] in VERSIONS_TO_SKIP:
                continue
            download["version"] = version["version"]
            yield download


def get_tools_package(arch_name: str, os_name: str) -> Optional[str]:
    for download in current_tools_releases["versions"][0]["downloads"]:
        if download["name"] == os_name and download["arch"] == arch_name:
            return download["package"]["url"]
    return None


def get_mongosh_package(arch_name: str, os_name: str) -> Optional[str]:
    if arch_name == "x86_64":
        arch_name = "x64"
    pkg_ext = "rpm"
    if "debian" in os_name or "ubuntu" in os_name:
        pkg_ext = "deb"
    for platform_type in mongosh_releases["versions"][0]["platform"]:
        if platform_type["os"] == pkg_ext and platform_type["arch"] == arch_name:
            return platform_type["download_link"]
    return None


arches: Set[str] = set()
oses: Set[str] = set()
editions: Set[str] = set()
versions: Set[str] = set()

for dl in iterate_over_downloads():
    editions.add(dl["edition"])
    arches.add(dl["arch"])
    oses.add(dl["target"])
    versions.add(dl["version"])

parser = argparse.ArgumentParser(
    description=
    'Test packages on various hosts. This will spin up docker containers and test the installs.')

parser.add_argument("--arch", type=str, help="Arch of host machine to use",
                    choices=["auto"] + list(arches), default="auto")
parser.add_argument(
    "--os", type=str, help=
    "OS of docker image to run test(s) on. All means run all os tests on this arch. None means run no os test on this arch (except for one specified in extra-packages.",
    choices=["all", "none"] + list(oses), default="all")
parser.add_argument(
    "-e", "--extra-test", type=str, help=
    "Space-separated tuple of (test_os, package_archive_path). For example ubuntu2004,https://s3.amazonaws.com/mciuploads/${project}/${build_variant}/${revision}/artifacts/${build_id}-packages.tgz.",
    action='append', nargs=2, default=[])
args = parser.parse_args()

mongo_os: str = args.os
extra_tests: List[Tuple[str, str]] = args.extra_test
arch: str = args.arch
if arch == "auto":
    arch = platform.machine()

tests: List[Test] = []
for extra_test in extra_tests:
    test_os = extra_test[0]
    logging.info(extra_test[1])
    urls: List[str] = [extra_test[1]]
    if test_os not in OS_DOCKER_LOOKUP:
        logging.error("We have not seen this OS %s before, please add it to OS_DOCKER_LOOKUP",
                      test_os)
        sys.exit(1)

    if not OS_DOCKER_LOOKUP[test_os]:
        logging.info("Skipping test on target because the OS has no associated container %s->???",
                     test_os)
        continue

    tools_package = get_tools_package(arch, test_os)
    mongosh_package = get_mongosh_package(arch, test_os)
    if tools_package:
        urls.append(tools_package)
    else:
        logging.warning("Could not find tools package for %s and %s", arch, test_os)

    if mongosh_package:
        urls.append(mongosh_package)
    else:
        logging.warning("Could not find mongosh package for %s and %s", arch, test_os)

    tests.append(Test(os_name=test_os, version="custom", packages_urls=urls))

# If os is None we only want to do the tests specified in the arguments
if mongo_os != "none":
    for dl in iterate_over_downloads():
        if mongo_os not in ["all", dl["target"]]:
            continue
        if dl["arch"] != arch:
            continue

        if not OS_DOCKER_LOOKUP[dl["target"]]:
            logging.info(
                "Skipping test on target because the OS has no associated container %s->??? on mongo version %s",
                dl['target'], dl['version'])
            continue

        if not "packages" in dl:
            logging.info(
                "Skipping test on target because there are no packages %s->??? on mongo version %s",
                dl['target'], dl['version'])
            continue

        if (dl["target"], dl["version"]) in DISABLED_TESTS:
            continue

        tests.append(
            Test(os_name=dl["target"], packages_urls=dl["packages"], version=dl["version"]))

report = Report(results=[], failures=0)
with futures.ThreadPoolExecutor() as tpe:
    test_futures = [tpe.submit(run_test, test) for test in tests]
    completed_tests = 0  # pylint: disable=invalid-name
    for f in futures.as_completed(test_futures):
        completed_tests += 1
        test_result = f.result()
        if test_result["exit_code"] != 0:
            report["failures"] += 1

        report["results"].append(test_result)
        logging.info("Completed %s/%s tests", completed_tests, len(test_futures))

with open("report.json", "w") as fh:
    json.dump(report, fh)

if report["failures"] == 0:
    logging.info("All %s tests passed :)", len(report['results']))
    sys.exit(0)
else:
    success_count = sum([1 for test_result in report["results"] if test_result["exit_code"] == 0])
    logging.info("%s/%s tests passed", success_count, len(report['results']))
    sys.exit(1)
