import argparse
import dataclasses
import json
import logging
import os
import platform
import re
import sys
import time
import traceback
import uuid

from concurrent import futures
from pathlib import Path
from typing import Any, Dict, Generator, List, Optional, Set, Tuple

import docker
import docker.errors
import requests

from docker.client import DockerClient
from docker.models.containers import Container
from docker.models.images import Image

from simple_report import Report, Result

root = logging.getLogger()
root.setLevel(logging.DEBUG)

handler = logging.StreamHandler(sys.stdout)
handler.setLevel(logging.DEBUG)
formatter = logging.Formatter('[%(asctime)s]%(levelname)s:%(message)s')
handler.setFormatter(formatter)
root.addHandler(handler)

PACKAGE_MANAGER_COMMANDS = {
    "apt": {
        "update": "export DEBIAN_FRONTEND=noninteractive && apt-get update -y",
        "install": "export DEBIAN_FRONTEND=noninteractive && apt-get install -y {}",
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

# Lookup table used when building and running containers
# os_name, Optional[(base_image, package_manager, frozenset(base_packages), python_command)]
OS_DOCKER_LOOKUP = {
    'amazon': None,
    'amzn64': None,
    'amazon2': ('amazonlinux:2', "yum",
                frozenset(["python", "python3", "wget", "pkgconfig", "systemd", "procps", "file"]),
                "python3"),
    'amazon2022': ('amazonlinux:2022', "yum",
                   frozenset(
                       ["python", "python3", "wget", "pkgconfig", "systemd", "procps", "file"]),
                   "python3"),
    'debian10': ('debian:10-slim', "apt",
                 frozenset(["python", "python3", "wget", "pkg-config", "systemd", "procps",
                            "file"]), "python3"),
    'debian11': ('debian:11-slim', "apt",
                 frozenset([
                     "python3", "python-is-python3", "wget", "pkg-config", "systemd", "procps",
                     "file"
                 ]), "python3"),
    'debian71': ('debian:7-slim', "apt",
                 frozenset(["python", "python3", "wget", "pkg-config", "systemd", "procps",
                            "file"]), "python3"),
    'debian81': ('debian:8-slim', "apt",
                 frozenset(["python", "python3", "wget", "pkg-config", "systemd", "procps",
                            "file"]), "python3"),
    'debian92': ('debian:9-slim', "apt",
                 frozenset(["python", "python3", "wget", "pkg-config", "systemd", "procps",
                            "file"]), "python3"),
    'linux_i686': None,
    'linux_x86_64': None,
    'macos': None,
    'osx': None,
    'osx-ssl': None,
    'rhel55': None,
    'rhel57': None,
    'rhel62': None,
    'rhel70': ('centos:7', "yum",
               frozenset(["rh-python38.x86_64", "wget", "pkgconfig", "systemd", "procps", "file"]),
               "/opt/rh/rh-python38/root/usr/bin/python3"),
    'rhel71': ('centos:7', "yum",
               frozenset(["rh-python38.x86_64", "wget", "pkgconfig", "systemd", "procps", "file"]),
               "/opt/rh/rh-python38/root/usr/bin/python3"),
    'rhel72': ('centos:7', "yum",
               frozenset(["rh-python38.x86_64", "wget", "pkgconfig", "systemd", "procps", "file"]),
               "/opt/rh/rh-python38/root/usr/bin/python3"),
    'rhel80': ('almalinux:8', "yum",
               frozenset(["python3", "wget", "pkgconfig", "systemd", "procps", "file"]), "python3"),
    'rhel81': ('almalinux:8', "yum",
               frozenset(["python3", "wget", "pkgconfig", "systemd", "procps", "file"]), "python3"),
    'rhel82': ('almalinux:8', "yum",
               frozenset(["python3", "wget", "pkgconfig", "systemd", "procps", "file"]), "python3"),
    'rhel83': ('almalinux:8', "yum",
               frozenset(["python3", "wget", "pkgconfig", "systemd", "procps", "file"]), "python3"),
    'rhel90': ('almalinux:9', "yum",
               frozenset(["python3", "wget", "pkgconfig", "systemd", "procps", "file"]), "python3"),
    'sunos5': None,
    'suse11': None,
    'suse12': None,
    'suse15': ('registry.suse.com/suse/sle15:latest', "zypper",
               frozenset(["python3", "wget", "pkg-config", "systemd", "procps", "file"]),
               "python3"),
    # Has the same error as above
    'ubuntu1204': None,
    'ubuntu1404': None,
    'ubuntu1604': ('ubuntu:16.04', "apt",
                   frozenset([
                       "apt-utils", "python", "python3", "wget", "pkg-config", "systemd", "procps",
                       "file"
                   ]), "python3"),
    'ubuntu1804': ('ubuntu:18.04', "apt",
                   frozenset(
                       ["python", "python3", "wget", "pkg-config", "systemd", "procps", "file"]),
                   "python3"),
    'ubuntu2004': ('ubuntu:20.04', "apt",
                   frozenset([
                       "python3", "python-is-python3", "wget", "pkg-config", "systemd", "procps",
                       "file"
                   ]), "python3"),
    'ubuntu2204': ('ubuntu:22.04', "apt",
                   frozenset([
                       "python3", "python-is-python3", "wget", "pkg-config", "systemd", "procps",
                       "file"
                   ]), "python3"),
    'windows': None,
    'windows_i686': None,
    'windows_x86_64': None,
    'windows_x86_64-2008plus': None,
    'windows_x86_64-2008plus-ssl': None,
    'windows_x86_64-2012plus': None,
}

# These versions are marked "current" but in fact are EOL
VERSIONS_TO_SKIP: Set[str] = set(['3.0.15', '3.2.22', '3.4.24', '3.6.23', '4.0.28'])
DISABLED_TESTS: Set[Tuple[str, str]] = set()


@dataclasses.dataclass
class Test:
    """Class to track a single test."""

    os_name: str
    version: str
    edition: str
    base_image: str = dataclasses.field(default="", repr=False)
    package_manager: str = dataclasses.field(default="", repr=False)
    update_command: str = dataclasses.field(default="", repr=False)
    install_command: str = dataclasses.field(default="", repr=False)
    base_packages: List[str] = dataclasses.field(default_factory=list)
    python_command: str = dataclasses.field(default="", repr=False)
    packages_urls: List[str] = dataclasses.field(default_factory=list)
    packages_paths: List[Path] = dataclasses.field(default_factory=list)
    attempts: int = dataclasses.field(default=0)

    def __post_init__(self) -> None:
        assert OS_DOCKER_LOOKUP[self.os_name] is not None

        self.base_image = OS_DOCKER_LOOKUP[self.os_name][0]
        self.package_manager = OS_DOCKER_LOOKUP[self.os_name][1]
        self.base_packages = list(OS_DOCKER_LOOKUP[self.os_name][2])
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
        return self.os_name + "-" + self.edition + "-" + self.version


def get_image(test: Test, client: DockerClient) -> Image:
    tries = 1
    while True:
        try:
            logging.info("Pulling base image for %s: %s, try %s", test.os_name, test.base_image,
                         tries)
            base_image = client.images.pull(test.base_image)
        except docker.errors.ImageNotFound as exc:
            if tries >= 5:
                logging.error("Base image %s not found after %s tries", test.base_image, tries)
                raise exc
        else:
            return base_image
        finally:
            tries += 1
            time.sleep(1)


def join_commands(commands: List[str], sep: str = ' && ') -> str:
    return sep.join(commands)


def run_test(test: Test, client: DockerClient) -> Result:
    result = Result(status="pass", test_file=test.name(), start=time.time(), log_raw="")

    log_name = f"logs/{test.name()}_{test.version}_{uuid.uuid4().hex}.log"
    test_docker_root = Path("/mnt/package_test").resolve()
    log_docker_path = Path.joinpath(test_docker_root, log_name)
    test_external_root = Path(__file__).parent.resolve()
    logging.debug(test_external_root)
    log_external_path = Path.joinpath(test_external_root, log_name)

    commands: List[str] = ["export PYTHONIOENCODING=UTF-8"]

    if test.os_name.startswith('rhel'):
        if test.os_name.startswith('rhel7'):
            # RHEL 7 needs the SCL installed for Python 3
            commands += [
                "yum -y install centos-release-scl",
                "yum-config-manager --enable centos-sclo-rh",
            ]
        # RHEL distros need EPEL for Compass dependencies
        commands += [
            "yum -y install yum-utils epel-release",
            "yum-config-manager --enable epel",
        ]

    commands += [
        test.update_command,
        test.install_command.format(" ".join(test.base_packages)),
    ]

    if test.python_command != 'python3':
        commands.append(f"ln -s {test.python_command} /usr/bin/python3")

    os.makedirs(log_external_path.parent, exist_ok=True)
    commands.append(
        f"python3 /mnt/package_test/package_test_internal.py {log_docker_path} {' '.join(test.packages_urls)}"
    )
    logging.debug("Attempting to run the following docker commands:\n\t%s",
                  join_commands(commands, sep='\n\t'))

    image: Image | None = None
    container: Container | None = None
    try:
        image = get_image(test, client)
        container = client.containers.run(image, command=f"bash -c \"{join_commands(commands)}\"",
                                          auto_remove=True, detach=True,
                                          volumes=[f'{test_external_root}:{test_docker_root}'])
        for log in container.logs(stream=True):
            result["log_raw"] += log.decode('UTF-8')
            # This is pretty verbose, lets run this way for a while and we can delete this if it ends up being too much
            logging.debug(log.decode('UTF-8').strip())
        exit_code = container.wait()
        result["exit_code"] = exit_code['StatusCode']
    except docker.errors.APIError as exc:
        traceback.print_exception(type(exc), exc, exc.__traceback__)
        logging.error("Failed to start test")
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


logging.info("Attempting to download current mongo releases json")
r = requests.get('https://downloads.mongodb.org/current.json')
current_releases = r.json()

logging.info("Attempting to download current mongo tools releases json")
r = requests.get('https://downloads.mongodb.org/tools/db/release.json')
current_tools_releases = r.json()

logging.info("Attempting to download current mongosh releases json")
r = requests.get('https://s3.amazonaws.com/info-mongodb-com/com-download-center/mongosh.json')
mongosh_releases = r.json()


def iterate_over_downloads() -> Generator[Dict[str, Any], None, None]:
    # TODO: TOOLS-3204 - we need to sub the arch alias until package
    # rchitectures are named consistently with the server packages
    for version in current_releases["versions"]:
        for download in version["downloads"]:
            if download["edition"] == "source":
                continue
            if version["version"] in VERSIONS_TO_SKIP:
                continue
            download["version"] = version["version"]
            yield download


def get_tools_package(arch_name: str, os_name: str) -> Optional[str]:
    # TODO: MONGOSH-1308 - we need to sub the arch alias until package
    # architectures are named consistently with the server packages
    if arch_name == "aarch64" and os_name != "amazon2":
        arch_name = "arm64"
    for download in current_tools_releases["versions"][0]["downloads"]:
        if download["name"] == os_name and download["arch"] == arch_name:
            return download["package"]["url"]
    return None


def get_mongosh_package(arch_name: str, os_name: str) -> Optional[str]:
    if arch_name == "aarch64":
        arch_name = "arm64"
    if arch_name in ("x86_64", "amd64"):
        arch_name = "x64"
    pkg_ext = "rpm"
    if "debian" in os_name or "ubuntu" in os_name:
        pkg_ext = "deb"
    for platform_type in mongosh_releases["versions"][0]["platform"]:
        if platform_type["os"] == pkg_ext and platform_type["arch"] == arch_name:
            return platform_type["download_link"]
    return None


def get_arch_aliases(arch_name: str) -> List[str]:
    if arch_name in ('amd64', 'x86_64'):
        return ['amd64', 'x86_64']
    if arch_name in ('ppc64le', 'ppc64el'):
        return ['ppc64le', 'ppc64el']
    return [arch_name]


def get_edition_alias(edition_name: str) -> str:
    if edition_name in ("base", "targeted"):
        return "org"
    return edition_name


arches: Set[str] = set()
oses: Set[str] = set()
editions: Set[str] = set()
versions: Set[str] = set()

for dl in iterate_over_downloads():
    editions.add(get_edition_alias(dl["edition"]))
    arches.add(dl["arch"])
    oses.add(dl["target"])
    versions.add(dl["version"])

parser = argparse.ArgumentParser(
    description=
    'Test packages on various hosts. This will spin up docker containers and test the installs.')
parser.add_argument("--arch", type=str, help="Arch of packages to test",
                    choices=["auto"] + list(arches), default="auto")
parser.add_argument("-r", "--retries", type=int, help="Number of times to retry failed tests",
                    default=3)
subparsers = parser.add_subparsers(dest="command")
release_test_parser = subparsers.add_parser("release")
release_test_parser.add_argument(
    "--os", type=str, help=
    "OS of docker image to run test(s) on. All means run all os tests on this arch. None means run no os test on this arch (except for one specified in extra-packages.",
    choices=["all"] + list(oses), default="all")
release_test_parser.add_argument("-e", "--edition", help="Server edition to run tests for",
                                 choices=["all"] + list(editions), default="all")
release_test_parser.add_argument("-v", "--server-version", type=str,
                                 help="Version of MongoDB to run tests for",
                                 choices=["all"] + list(versions), default="all")
branch_test_parser = subparsers.add_parser("branch")
branch_test_parser.add_argument(
    "-t", "--test", type=str, help=
    "Space-separated tuple of (test_os, package_archive_path). For example: ubuntu2004 https://s3.amazonaws.com/mciuploads/${project}/${build_variant}/${revision}/artifacts/${build_id}-packages.tgz.",
    action='append', nargs=2, default=[])
branch_test_parser.add_argument("-e", "--edition", type=str, help="Server edition being tested",
                                required=True)
branch_test_parser.add_argument("-v", "--server-version", type=str,
                                help="Server version being tested", required=True)
args = parser.parse_args()

arch: str = args.arch
if arch == "auto":
    arch = platform.machine()

tests: List[Test] = []
urls: List[str] = []

if args.command == "branch":
    for test_pair in args.test:
        test_os = test_pair[0]
        urls = [test_pair[1]]
        if test_os not in OS_DOCKER_LOOKUP:
            logging.error("We have not seen this OS %s before, please add it to OS_DOCKER_LOOKUP",
                          test_os)
            sys.exit(1)

        if not OS_DOCKER_LOOKUP[test_os]:
            logging.info(
                "Skipping test on target because the OS has no associated container %s->???",
                test_os)
            continue

        tools_package = get_tools_package(arch, test_os)
        mongosh_package = get_mongosh_package(arch, test_os)
        if tools_package:
            urls.append(tools_package)
        else:
            logging.error("Could not find tools package for %s and %s", arch, test_os)
            sys.exit(1)

        if mongosh_package:
            urls.append(mongosh_package)
        else:
            logging.error("Could not find mongosh package for %s and %s", arch, test_os)
            sys.exit(1)

        tests.append(
            Test(os_name=test_os, edition=args.edition, version=args.server_version,
                 packages_urls=urls))

# If os is None we only want to do the tests specified in the arguments
if args.command == "release":

    for dl in iterate_over_downloads():
        if args.os not in ["all", dl["target"]]:
            continue

        if args.server_version not in ("all", dl["version"]):
            continue

        # "base" and "targeted" should both match "org"
        if args.edition not in ("all", get_edition_alias(dl["edition"])):
            continue

        # amd64 and x86_64 should be treated as aliases of each other
        if dl["arch"] not in get_arch_aliases(arch):
            continue

        if not OS_DOCKER_LOOKUP[dl["target"]]:
            logging.info(
                "Skipping test on target because the OS has no associated container %s->??? on mongo version %s",
                dl['target'], dl['version'])
            continue

        if "packages" not in dl:
            logging.info(
                "Skipping test on target because there are no packages %s->??? on mongo version %s",
                dl['target'], dl['version'])
            continue

        if (dl["target"], dl["version"]) in DISABLED_TESTS:
            continue

        test_os: str = dl["target"]
        urls: List[str] = dl["packages"]
        server_version: str = dl["version"]
        edition: str

        version_major, version_minor, _ = server_version.split(".", 2)
        version_major = int(version_major)
        version_minor = int(version_minor)

        # The feeds don't include metapackages, so we need to add them to our
        # URL list for a complete installation.
        repo_uri: str
        package: str
        repo_uri, package = urls[0].rsplit("/", 1)
        match = re.match(r'(\w+-(\w+(?:-unstable)?))-[^-_]+((?:-|_).*)', package)
        if match:
            urls.insert(0, f"{repo_uri}/{match.group(1)}{match.group(3)}")
            # The actual "edition" may be an unstable package release, so we
            # need to capture that.
            package_type = match.group(0).split(".")[-1]
            edition = match.group(2)

            if version_major > 4:
                urls.append(f"{repo_uri}/{match.group(1)}-database{match.group(3)}")

            if version_major > 4 or (version_major == 4 and version_minor >= 3):
                urls.append(f"{repo_uri}/{match.group(1)}-database-tools-extra{match.group(3)}")

            urls.append(f"{repo_uri}/{match.group(1)}-tools{match.group(3)}")
            urls.append(f"{repo_uri}/{match.group(1)}-mongos{match.group(3)}")

            if package_type == "deb":
                # We removed the built-in shell package for RPM, but for some reason
                # we never removed it from the DEB packages. It's just an empty package
                # we require users to install now...
                urls.append(f"{repo_uri}/{match.group(1)}-shell{match.group(3)}")
        else:
            logging.error("Failed to match package name: %s", package)
            sys.exit(1)

        if version_major > 4 or (version_major == 4 and version_minor >= 3):
            # The external `tools' package is only compatible with server
            # versions 4.3 and above. Before that, we used the `tools`
            # package built in the server repo.
            tools_package = get_tools_package(arch, test_os)
            if tools_package:
                urls.append(tools_package)
            else:
                logging.error("Could not find tools package for %s and %s", arch, test_os)
                sys.exit(1)

        mongosh_package = get_mongosh_package(arch, test_os)
        if mongosh_package:
            # The mongosh package doesn't work on Ubuntu 16.04
            if test_os != "ubuntu1604":
                urls.append(mongosh_package)
        else:
            logging.error("Could not find mongosh package for %s and %s", arch, test_os)
            sys.exit(1)

        tests.append(
            Test(os_name=test_os, packages_urls=urls, edition=edition, version=server_version))

docker_client = docker.client.from_env()
docker_username = os.environ.get('docker_username')
docker_password = os.environ.get('docker_password')
if all((docker_username, docker_password)):
    logging.info("Logging into docker.io")
    response = docker_client.login(username=docker_username, password=docker_password)
    logging.debug("Login response: %s", response)
else:
    logging.warning("Skipping docker login")

report = Report(results=[], failures=0)
with futures.ThreadPoolExecutor() as tpe:
    test_futures = {tpe.submit(run_test, test, docker_client): test for test in tests}
    completed_tests: int = 0
    retried_tests: int = 0
    total_tests: int = len(tests)
    while len(test_futures.keys()) > 0:
        finished_futures, active_futures = futures.wait(test_futures.keys(), timeout=None,
                                                        return_when="FIRST_COMPLETED")
        for f in finished_futures:
            completed_test = test_futures.pop(f)
            test_result = f.result()
            if test_result["exit_code"] != 0:
                if completed_test.attempts < args.retries:
                    retried_tests += 1
                    completed_test.attempts += 1
                    test_futures[tpe.submit(run_test, completed_test,
                                            docker_client)] = completed_test
                    continue
                report["failures"] += 1

            completed_tests += 1
            report["results"].append(test_result)

        logging.info(
            "Completed %s tests, retried %s tests, total %s tests, %s tests are in progress.",
            completed_tests, retried_tests, total_tests, len(test_futures))

        # We are printing here to help diagnose hangs
        # This adds a bit of logging so we are only going to log running tests after a test completes
        for active_test in test_futures.values():
            logging.info("Test in progress: %s", active_test)

with open("report.json", "w") as fh:
    json.dump(report, fh)

if report["failures"] == 0:
    logging.info("All %s tests passed :)", len(report['results']))
    sys.exit(0)
else:
    failed_tests = [
        test_result["test_file"] for test_result in report["results"]
        if test_result["exit_code"] != 0
    ]
    success_count = len(report['results']) - len(failed_tests)
    logging.info("%s/%s tests passed", success_count, len(report['results']))
    if len(failed_tests) > 0:
        logging.info("Failed tests:\n\t%s", "\n\t".join(failed_tests))
    sys.exit(1)
