"""Package-manager commands used to prepare package-test containers."""

# apt-get update can exit successfully after a transient download failure and leave the container
# without a complete package index. Retry each metadata and package download within apt itself.
APT_RETRIES = 3
APT_GET = f"apt-get -o Acquire::Retries={APT_RETRIES}"
DEBIAN11_APT_UPDATE_OPTION = "-o Acquire::Check-Valid-Until=false"
DEBIAN11_SECURITY_SNAPSHOT = "20260831T033337Z"
DEBIAN11_ARCHIVE_SECURITY_REPOSITORY = "archive.debian.org/debian-security"
DEBIAN11_SNAPSHOT_SECURITY_REPOSITORY = (
    f"snapshot.debian.org/archive/debian-security/{DEBIAN11_SECURITY_SNAPSHOT}"
)

PACKAGE_MANAGER_COMMANDS: dict[str, dict[str, str]] = {
    "apt": {
        "update": f"export DEBIAN_FRONTEND=noninteractive && {APT_GET} update -y",
        "install": f"export DEBIAN_FRONTEND=noninteractive && {APT_GET} install -y {{}}",
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


def build_update_command(package_manager: str, os_name: str) -> str:
    """Build the package-index update command for an operating system."""

    update_command = PACKAGE_MANAGER_COMMANDS[package_manager]["update"]
    if package_manager == "apt" and os_name.startswith("debian11"):
        # Debian 11 reached end-of-life, so its release metadata is expired.
        update_command = update_command.replace(
            " update", f" {DEBIAN11_APT_UPDATE_OPTION} update", 1
        )
    return update_command


def build_os_setup_commands(os_name: str) -> list[str]:
    """Build commands needed to prepare an OS's package repositories."""

    if os_name.startswith("debian11"):
        # Debian 11's security repository is partially pruned after EOL, so its index
        # can point to packages that no longer exist. Prefer the archive repository
        # once it is populated, and otherwise use a complete pre-EOL snapshot rather
        # than removing security entirely and leaving the base image's dependencies
        # inconsistent with bullseye main/updates.
        return [
            "sed -i 's|deb.debian.org/debian-security|"
            f"{DEBIAN11_ARCHIVE_SECURITY_REPOSITORY}|g' /etc/apt/sources.list",
            f"apt-get {DEBIAN11_APT_UPDATE_OPTION} update || true",
            "if ! ls /var/lib/apt/lists/*archive.debian.org_debian-security_dists_"
            "bullseye-security* > /dev/null 2>&1; then "
            "sed -i 's|"
            f"{DEBIAN11_ARCHIVE_SECURITY_REPOSITORY}|"
            f"{DEBIAN11_SNAPSHOT_SECURITY_REPOSITORY}"
            "|g' /etc/apt/sources.list; fi",
        ]
    return []


def build_python_setup_commands(python_command: str) -> list[str]:
    """Build commands that validate and expose the Python interpreter to the test."""

    validate_python_command = (
        f"if ! {python_command} --version; then "
        f"echo 'Required Python interpreter {python_command} is unavailable' >&2; "
        "exit 1; "
        "fi"
    )
    commands = [validate_python_command]
    if python_command != "python3":
        commands.append(f"ln -s {python_command} /usr/bin/python3")
    return commands


def build_package_test_internal_args(
    edition: str,
    platform: str,
    package_urls: list[str],
    skip_system_library_check: bool = False,
) -> list[str]:
    """Build the arguments passed to package_test_internal.py."""

    args = ["--edition", edition, "--platform", platform]
    if skip_system_library_check:
        args.append("--skip-system-library-check")
    return args + package_urls
