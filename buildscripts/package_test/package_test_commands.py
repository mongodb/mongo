"""Package-manager commands used to prepare package-test containers."""

# apt-get update can exit successfully after a transient download failure and leave the container
# without a complete package index. Retry each metadata and package download within apt itself.
APT_RETRIES = 3
APT_GET = f"apt-get -o Acquire::Retries={APT_RETRIES}"

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
