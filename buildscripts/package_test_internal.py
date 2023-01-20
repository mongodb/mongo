# This script needs to be compatible with odler versions of python since it runs on older versions of OSs when testing packaging
# For example ubuntu 1604 uses python3.5

#pylint: disable=redefined-outer-name,invalid-name

import grp
import logging
import os
import pathlib
import platform
import pwd
import re
import sys
import subprocess
import tarfile
import time
import traceback

from logging.handlers import WatchedFileHandler
from typing import Dict, List, Optional, Tuple, Union

root = logging.getLogger()
root.setLevel(logging.DEBUG)

stdout_handler = logging.StreamHandler(sys.stdout)
stdout_handler.setLevel(logging.DEBUG)
file_handler = WatchedFileHandler(sys.argv[1], mode='w')
file_handler.setLevel(logging.DEBUG)
formatter = logging.Formatter('[%(asctime)s]%(levelname)s:%(message)s')
stdout_handler.setFormatter(formatter)
file_handler.setFormatter(formatter)
root.addHandler(stdout_handler)
root.addHandler(file_handler)

DOCKER_SYSTEMCTL_REPO = "https://raw.githubusercontent.com/gdraheim/docker-systemctl-replacement"
SYSTEMCTL_URL = DOCKER_SYSTEMCTL_REPO + "/master/files/docker/systemctl3.py"
JOURNALCTL_URL = DOCKER_SYSTEMCTL_REPO + "/master/files/docker/journalctl3.py"

TestArgs = Dict[str, Union[str, int, List[str]]]


def run_and_log(cmd: str, end_on_error: bool = True) -> 'subprocess.CompletedProcess[bytes]':
    proc = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)  # pylint: disable=subprocess-run-check
    logging.debug(cmd)
    logging.debug(proc.stdout.decode("UTF-8").strip())
    if end_on_error and proc.returncode != 0:
        logging.error("Command %s failed, failing test\n", cmd)
        raise RuntimeError("Command failed")
    return proc


def download_extract_package(package: str) -> List[str]:
    # Use wget here because using urllib we get errors like the following
    # https://stackoverflow.com/questions/27835619/urllib-and-ssl-certificate-verify-failed-error
    run_and_log("wget -q \"{}\"".format(package))
    downloaded_file = package.split('/')[-1]
    if not package.endswith(".tgz"):
        return [downloaded_file]

    extracted_paths = []
    with tarfile.open(downloaded_file) as tf:
        for member in tf.getmembers():
            if member.name.endswith('.deb') or member.name.endswith('.rpm'):
                extracted_paths.append(member.name)
        tf.extractall()

    return extracted_paths


def download_extract_all_packages(package_urls: List[str]) -> List[str]:
    all_packages: List[str] = []
    for package_url in package_urls:
        package_names = download_extract_package(package_url)
        all_packages.extend(["./" + package_name for package_name in package_names])
    return all_packages


def run_apt_test(packages: List[str]):
    logging.info("Detected apt running test.")
    run_and_log("DEBIAN_FRONTEND=noninteractive apt-get install -y {}".format(' '.join(packages)))


def run_yum_test(packages: List[str]):
    logging.info("Detected yum running test.")
    run_and_log("yum install -y {}".format(' '.join(packages)))


def run_zypper_test(packages: List[str]):
    logging.info("Detected zypper running test.")
    run_and_log("zypper -n --no-gpg-checks install {}".format(' '.join(packages)))


def run_mongo_query(query: str, should_fail: bool = False, tries: int = 60,
                    interval: float = 1.0) -> Optional['subprocess.CompletedProcess[bytes]']:
    assert tries >= 1

    exec_result: Union[subprocess.CompletedProcess[bytes], None] = None
    current_try = 1

    command = f"mongosh --eval '{query}'"

    # Keep trying the query until we either get a successful return code or we
    # run out of tries.
    while current_try <= tries:
        try:
            logging.debug("Sending query: %s - try %s", query, current_try)
            exec_result = run_and_log(command, end_on_error=False)
        except (IOError, OSError) as exc:
            logging.error("Command execution failed: %s", command)
            traceback.print_exception(type(exc), exc, exc.__traceback__)
            raise
        else:
            if ((not should_fail and exec_result.returncode == 0)
                    or (should_fail and exec_result.returncode != 0)):
                return exec_result

            logging.error("Command failed with output: %s",
                          exec_result.stdout.decode('UTF-8').rstrip())

            current_try += 1

            if current_try < tries:
                time.sleep(interval)

    raise RuntimeError("Query retries exceeded, failing test.")


def parse_os_release(path: str) -> Dict[str, str]:
    result: Dict[str, str] = {}
    with open(path, 'r', encoding='utf-8') as os_release:
        for line in os_release:
            try:
                key, value = line.rstrip().split('=', 1)
            except ValueError:
                continue
            value = value.strip('"')
            result[key] = value
    return result


def get_os_release() -> Tuple[str, int, int]:
    if os.path.exists('/etc/os-release'):
        release_info = parse_os_release('/etc/os-release')
    elif os.path.exists('/usr/lib/os-release'):
        release_info = parse_os_release('/etc/os-release')
    else:
        logging.error("SYSFAIL: Could not find os-release file")
        sys.exit(2)

    os_name = release_info['ID']
    os_version = release_info['VERSION_ID']

    try:
        os_version_major, os_version_minor = (int(text) for text in os_version.split('.'))
    except ValueError:
        os_version_major = int(os_version)
        os_version_minor = 0

    return os_name, os_version_major, os_version_minor


def parse_ulimits(pid: int) -> Dict[str, Tuple[int, int, Optional[str]]]:
    ulimit_line_re = re.compile(
        r'(?P<name>.*?)\s{2,}(?P<soft>\S+)\s+(?P<hard>\S+)(?:\s+(?P<units>\S+))?', re.MULTILINE)

    result: Dict[str, Tuple[int, int, Optional[str]]] = {}
    with open(f"/proc/{pid}/limits", 'r', encoding='utf-8') as ulimits_file:
        next(ulimits_file)
        for line in ulimits_file:
            limits = ulimit_line_re.match(line)
            if limits is None:
                continue
            try:
                soft_limit = int(limits['soft'])
            except ValueError:
                # unlimited
                soft_limit = -1
            try:
                hard_limit = int(limits['hard'])
            except ValueError:
                # unlimited
                hard_limit = -1

            result[limits['name']] = (soft_limit, hard_limit, limits['units'])

    return result


def get_test_args(package_manager: str, package_files: List[str]) -> TestArgs:
    # Set up data for later tests
    test_args: TestArgs = {}

    test_args['package_manager'] = package_manager
    test_args['package_files'] = package_files

    os_name, os_version_major, os_version_minor = get_os_release()
    test_args['os_name'] = os_name
    test_args['os_version_major'] = os_version_major
    test_args['os_version_minor'] = os_version_minor

    test_args['systemd_units_dir'] = run_and_log(
        "pkg-config systemd --variable=systemdsystemunitdir").stdout.decode('utf-8').strip()

    if package_manager in ('yum', 'zypper'):
        test_args['mongo_username'] = 'mongod'
        test_args['mongo_groupname'] = 'mongod'
        test_args['mongo_home_dir'] = '/var/lib/mongo'
        test_args['mongo_work_dir'] = '/var/lib/mongo'
        test_args['mongo_user_shell'] = '/bin/false'
    else:
        test_args['mongo_username'] = 'mongodb'
        test_args['mongo_groupname'] = 'mongodb'
        test_args['mongo_home_dir'] = '/home/mongodb'
        test_args['mongo_work_dir'] = '/var/lib/mongodb'

        if ((os_name == 'debian' and os_version_major >= 10)
                or (os_name == 'ubuntu' and os_version_major >= 18)):
            test_args['mongo_user_shell'] = '/usr/sbin/nologin'
        else:
            test_args['mongo_user_shell'] = '/bin/false'

    test_args['arch'] = platform.machine()

    deb_output_re = re.compile(r'(?<=Package: ).*$', re.MULTILINE)

    def get_package_name(package_file: str) -> str:
        if package_manager in ('yum', 'zypper'):
            result = run_and_log(f"rpm --nosignature -qp --queryformat '%{{NAME}}' {package_file}")
            return result.stdout.decode('utf-8').strip()
        else:
            result = run_and_log(f"dpkg -I {package_file}")
            match = deb_output_re.search(result.stdout.decode('utf-8'))
            if match is not None:
                return match[0]
            return ''

    package_names: List[str] = []
    for package in package_files:
        package_names.append(get_package_name(package))
    test_args['package_names'] = package_names

    return test_args


def setup():
    logging.info("Setting up test environment.")

    # TODO SERVER-70425: We can remove these once we have figured out why
    # packager.py sometimes uses distro files from older revisions.
    # Remove the PIDFile, PermissionsStartOnly, and Type configurations from
    # the systemd service file because they are not needed for simple-type
    # (non-forking) services and confuse the systemd emulator script.
    run_and_log(
        "sed -Ei '/^PIDFile=|PermissionsStartOnly=|Type=/d' $(pkg-config systemd --variable=systemdsystemunitdir)/mongod.service"
    )
    # Remove the journal: line (and the next) from mongod.conf, which is a
    # removed configuration. The Debian version of the config never got updated.
    run_and_log("sed -i '/journal:/,+1d' /etc/mongod.conf")
    # Remove fork: and pidFilePath: from mongod.conf because we want mongod to be
    # a non-forking service under systemd.
    run_and_log("sed -Ei '/fork:|pidFilePath:/d' /etc/mongod.conf")

    # TODO SERVER-70426: Remove these when we have a fake systemd package
    # that does all of this for us.
    # Ensure that mongod doesn't start automatically so we can do it as a test.
    run_and_log("mkdir -p /run/systemd/system")
    run_and_log("mkdir -p $(pkg-config systemd --variable=systemdsystempresetdir)")
    run_and_log(
        "echo 'disable *' > $(pkg-config systemd --variable=systemdsystempresetdir)/00-test.preset")
    # Install a systemd emulator script so we can test services the way they are
    # actually installed on user systems.
    run_and_log("wget -P /usr/bin " + SYSTEMCTL_URL)
    run_and_log("wget -P /usr/bin " + JOURNALCTL_URL)
    run_and_log("chmod +x /usr/bin/systemctl3.py /usr/bin/journalctl3.py")
    # Overwrite the installed systemd with the emulator script and set up
    # the required symlinks so it all works correctly.
    run_and_log("ln -sf /usr/bin/systemctl3.py /bin/systemctl")
    run_and_log("ln -sf /usr/bin/systemctl3.py /usr/bin/systemctl")
    run_and_log("ln -sf /usr/bin/systemctl3.py /bin/systemd")
    run_and_log("ln -sf /usr/bin/systemctl3.py /usr/bin/systemd")
    run_and_log("ln -sf /usr/bin/journalctl3.py /bin/journalctl")
    run_and_log("ln -sf /usr/bin/journalctl3.py /usr/bin/journalctl")


def test_start():
    logging.info("Starting mongod.")

    run_and_log("systemctl enable mongod.service")
    run_and_log("systemctl start mongod.service")

    run_mongo_query("db.smoke.insertOne({answer: 42})")
    run_and_log("systemctl is-active mongod.service")


def test_install_is_complete(test_args: TestArgs):
    logging.info("Checking that the installation is complete.")

    required_files: List[pathlib.Path] = [
        pathlib.Path('/etc/mongod.conf'),
        pathlib.Path('/usr/bin/mongod'),
        pathlib.Path('/var/log/mongodb/mongod.log'),
        pathlib.Path(test_args['systemd_units_dir']) / "mongod.service",
    ]

    required_dirs: List[pathlib.Path] = [
        pathlib.Path(test_args['mongo_work_dir']),
    ]

    if test_args['package_manager'] in ('yum', 'zypper'):
        # Only RPM-based distros create the home directory. Debian/Ubuntu
        # distros use a non-existent directory in /home
        required_dirs.append(pathlib.Path(test_args['mongo_home_dir']))

    for path in required_files:
        if not (path.exists() and path.is_file()):
            raise RuntimeError(f"Required file missing: {path}")

    for path in required_dirs:
        if not (path.exists() and path.is_dir()):
            raise RuntimeError(f"Required directory missing: {path}")

    try:
        user_info = pwd.getpwnam(test_args['mongo_username'])
    except KeyError:
        raise RuntimeError(f"Required user missing: {test_args['mongo_username']}")

    try:
        grp.getgrnam(test_args['mongo_groupname'])
    except KeyError:
        raise RuntimeError(f"Required group missing: {test_args['mongo_username']}")

    # All of the supplemental groups (the .deb pattern)
    mongo_user_groups = [
        g.gr_name for g in grp.getgrall() if test_args['mongo_username'] in g.gr_mem
    ]

    # The user's primary group (the .rpm pattern)
    mongo_user_groups.append(grp.getgrgid(user_info.pw_gid).gr_name)

    if test_args['mongo_groupname'] not in mongo_user_groups:
        raise RuntimeError((f"Required group `{test_args['mongo_groupname']}' "
                            f"is not in configured groups: {mongo_user_groups}"))

    if user_info.pw_dir != test_args['mongo_home_dir']:
        raise RuntimeError((f"Configured home directory `{user_info.pw_dir}' "
                            f"does not match required path `{test_args['mongo_home_dir']}'"))

    if user_info.pw_shell != test_args['mongo_user_shell']:
        raise RuntimeError((f"Configured user shell `{user_info.pw_shell}' "
                            f"does not match required path `{test_args['mongo_user_shell']}'"))


def test_ulimits_correct():
    logging.info("Checking that mongod process limits are correct.")

    exec_result = run_and_log("pgrep '^mongod$'")
    mongod_pid = int(exec_result.stdout.decode('utf-8').strip())

    ulimits = parse_ulimits(mongod_pid)

    if ulimits['Max file size'][0] != -1:
        raise RuntimeError(f"RLMIT_FSIZE != unlimited: {ulimits['Max file size']}")

    if ulimits['Max cpu time'][0] != -1:
        raise RuntimeError(f"RLMIT_CPU != unlimited: {ulimits['Max cpu time']}")

    if ulimits['Max address space'][0] != -1:
        raise RuntimeError(f"RLMIT_AS != unlimited: {ulimits['Max address space']}")

    if ulimits['Max open files'][0] != -1 and ulimits['Max open files'][0] < 64000:
        raise RuntimeError(f"RLMIT_NOFILE < 64000: {ulimits['Max open files']}")

    if ulimits['Max resident set'][0] != -1:
        raise RuntimeError(f"RLMIT_RSS != unlimited: {ulimits['Max resident set']}")

    if ulimits['Max processes'][0] != -1 and ulimits['Max processes'][0] < 64000:
        raise RuntimeError(f"RLMIT_NPROC < 64000: {ulimits['Max processes']}")


def test_restart():
    logging.info("Restarting mongod.")

    run_and_log("systemctl restart mongod.service")

    logging.debug("Waiting up to 60 seconds for mongod to restart...")
    run_mongo_query("db.smoke.insertOne({answer: 42})")

    run_and_log("systemctl is-active mongod.service")


def test_stop():
    logging.info("Stopping mongod.")

    run_and_log("systemctl stop mongod.service")

    logging.debug("Waiting up to 60 seconds for mongod to finish shutting down...")
    run_mongo_query("db.smoke.insertOne({answer: 42})", should_fail=True)

    run_and_log("systemctl is-active mongod.service", end_on_error=False)


def test_install_compass(test_args: TestArgs):
    logging.info("Installing Compass.")

    exec_result = run_and_log("install_compass", end_on_error=False)

    if exec_result.returncode != 0:
        if test_args['arch'] == 'x86_64' and test_args['package_manager'] != 'zypper':
            # install-compass does not work on platforms other than x86_64 and
            # currently cannot use zypper to install packages.
            raise RuntimeError("Failed to install compass")


def test_uninstall(test_args: TestArgs):
    logging.info("Uninstalling packages:\n\t%s", '\n\t'.join(test_args['package_names']))

    command: str
    if test_args['package_manager'] == 'apt':
        command = 'apt-get remove -y {}'
    elif test_args['package_manager'] == 'yum':
        command = 'yum remove -y {}'
    elif test_args['package_manager'] == 'zypper':
        command = 'zypper -n remove {}'
    else:
        raise RuntimeError(
            "Don't know how to uninstall with package manager: {test_args['package_manager']}")

    run_and_log(command.format(' '.join(test_args['package_names'])))


def test_uninstall_is_complete(test_args: TestArgs):
    logging.info("Checking that the uninstallation is complete.")

    leftover_files = [
        pathlib.Path('/usr/bin/mongod'),
        pathlib.Path(test_args['systemd_units_dir']) / 'mongod.service',
    ]

    for path in leftover_files:
        if path.exists():
            raise RuntimeError(f"Failed to uninstall cleanly, found: {path}")


package_urls = sys.argv[2:]

if len(package_urls) == 0:
    logging.error("No packages to test... Failing test")
    sys.exit(1)

package_files = download_extract_all_packages(package_urls)

package_manager: str
apt_proc = run_and_log("apt --help", end_on_error=False)
yum_proc = run_and_log("yum --help", end_on_error=False)
zypper_proc = run_and_log("zypper -n --help", end_on_error=False)
# zypper
if apt_proc.returncode == 0:
    run_apt_test(packages=package_files)
    package_manager = 'apt'
elif yum_proc.returncode == 0:
    run_yum_test(packages=package_files)
    package_manager = 'yum'
elif zypper_proc.returncode == 0:
    run_zypper_test(packages=package_files)
    package_manager = 'zypper'
else:
    logging.error("Found no supported package manager...Failing Test\n")
    sys.exit(1)

test_args = get_test_args(package_manager, package_files)
setup()

test_start()
test_install_is_complete(test_args)
test_ulimits_correct()
test_restart()
test_stop()
test_install_compass(test_args)
test_uninstall(test_args)
test_uninstall_is_complete(test_args)

sys.exit(0)
