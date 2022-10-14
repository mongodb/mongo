# This script needs to be compatible with odler versions of python since it runs on older versions of OSs when testing packaging
# For example ubuntu 1604 uses python3.5

import sys
import subprocess
import tarfile
import logging
from logging.handlers import WatchedFileHandler
from typing import List

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


def run_apt_test(packages: List[str]):
    logging.info("Detected apt running test.")
    install_together = ""
    for package in packages:
        deb_names = download_extract_package(package)
        for deb_name in deb_names:
            install_together += "./" + deb_name + " "
    run_and_log("DEBIAN_FRONTEND=noninteractive apt-get install -y {}".format(install_together))


def run_yum_test(packages: List[str]):
    logging.info("Detected yum running test.")
    install_together = ""
    for package in packages:
        rpm_names = download_extract_package(package)
        install_together += ' '.join(rpm_names) + " "
    run_and_log("yum install -y {}".format(install_together))


def run_zypper_test(packages: List[str]):
    logging.info("Detected zypper running test.")
    install_together = ""
    for package in packages:
        rpm_names = download_extract_package(package)
        install_together += ' '.join(rpm_names) + " "
    run_and_log("zypper -n --no-gpg-checks install {}".format(install_together))


def run_startup_test():
    logging.info("Starting mongod.")

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

    # Overwrite the installed systemd with the emulator script and set up
    # the required symlinks so it all works correctly.
    # TODO SERVER-70426: Remove these when we have a fake systemd package
    # that does all of this for us.
    run_and_log("ln -sf /usr/bin/systemctl3.py /bin/systemctl")
    run_and_log("ln -sf /usr/bin/systemctl3.py /usr/bin/systemctl")
    run_and_log("ln -sf /usr/bin/systemctl3.py /bin/systemd")
    run_and_log("ln -sf /usr/bin/systemctl3.py /usr/bin/systemd")
    run_and_log("ln -sf /usr/bin/journalctl3.py /bin/journalctl")
    run_and_log("ln -sf /usr/bin/journalctl3.py /usr/bin/journalctl")
    run_and_log("systemctl enable mongod.service")
    run_and_log("systemctl start mongod.service")


package_urls = sys.argv[2:]

if len(package_urls) == 0:
    logging.error("No packages to test... Failing test")
    sys.exit(1)

apt_proc = run_and_log("apt --help", end_on_error=False)
yum_proc = run_and_log("yum --help", end_on_error=False)
zypper_proc = run_and_log("zypper -n --help", end_on_error=False)
# zypper
if apt_proc.returncode == 0:
    run_apt_test(packages=package_urls)
elif yum_proc.returncode == 0:
    run_yum_test(packages=package_urls)
elif zypper_proc.returncode == 0:
    run_zypper_test(packages=package_urls)
else:
    logging.error("Found no supported package manager...Failing Test\n")
    sys.exit(1)

run_startup_test()

sys.exit(0)
