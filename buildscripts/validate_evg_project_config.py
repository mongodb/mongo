import os.path
import re
import subprocess
import sys

import structlog
import typer
from typing_extensions import Annotated

from buildscripts.ciconfig.evergreen import find_evergreen_binary

LOGGER = structlog.get_logger(__name__)

DEFAULT_LOCAL_EVG_AUTH_CONFIG = os.path.expanduser("~/.evergreen.yml")

DEFAULT_EVG_PROJECT_NAME = "mongodb-mongo-master"
DEFAULT_EVG_NIGHTLY_PROJECT_NAME = "mongodb-mongo-master-nightly"
DEFAULT_EVG_NIGHTLY_PROJECT_CONFIG = "etc/evergreen_nightly.yml"

ALLOWABLE_EVG_VALIDATE_MESSAGE_REGEXES = [
    re.compile(r".*buildvariant .+ has unmatched selector: .+"),
    re.compile(r".*buildvariant .+ has unmatched criteria: .+"),
]
ALLOWABLE_IF_NOT_IN_ALL_PROJECTS_EVG_VALIDATE_MESSAGE_REGEXES = [
    re.compile(r".*task .+ defined but not used by any variants; consider using or disabling.*"),
]

HORIZONTAL_LINE = "-" * 100


def main(
    evg_project_name: Annotated[
        str, typer.Option(help="Evergreen project name")
    ] = DEFAULT_EVG_PROJECT_NAME,
    evg_auth_config: Annotated[
        str, typer.Option(help="Evergreen auth config file")
    ] = DEFAULT_LOCAL_EVG_AUTH_CONFIG,
):
    evg_project_config_map = {evg_project_name: DEFAULT_EVG_NIGHTLY_PROJECT_CONFIG}
    if evg_project_name == DEFAULT_EVG_PROJECT_NAME:
        evg_project_config_map = {
            DEFAULT_EVG_NIGHTLY_PROJECT_NAME: DEFAULT_EVG_NIGHTLY_PROJECT_CONFIG,
        }

    evergreen_bin = find_evergreen_binary("evergreen")
    for _, project_config in evg_project_config_map.items():
        cmd = [
            evergreen_bin,
            "--config",
            evg_auth_config,
            "evaluate",
            "--path",
            project_config,
        ]
        LOGGER.info(f"Running command: {cmd}")
        subprocess.run(cmd, capture_output=True, text=True, check=True)

    sys.exit(0)


if __name__ == "__main__":
    typer.run(main)
