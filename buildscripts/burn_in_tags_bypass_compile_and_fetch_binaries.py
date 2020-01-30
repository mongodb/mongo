#!/usr/bin/env python3
"""Bypass compile and fetch binaries for burn_in_tags."""

import logging
import sys

import click
import structlog
from structlog.stdlib import LoggerFactory

from evergreen.api import RetryingEvergreenApi
from buildscripts.bypass_compile_and_fetch_binaries import TargetBuild, gather_artifacts_and_update_expansions

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.get_logger(__name__)

EVG_CONFIG_FILE = ".evergreen.yml"


@click.command()
@click.option("--project", required=True, help="The evergreen project.")
@click.option("--build-variant", required=True, help="Build variant where compile is running.")
@click.option("--revision", required=True, help="Base revision of the build.")
@click.option("--out-file", required=True, help="File to write macros expansions to.")
@click.option("--version-id", required=True, help="Evergreen version id of the current build.")
@click.option("--json-artifact", required=True,
              help="The JSON file to write out the metadata of files to attach to task.")
def main(  # pylint: disable=too-many-arguments,too-many-locals
        project, build_variant, revision, out_file, version_id, json_artifact):
    """
    Create a file with expansions that can be used to bypass compile.

    This is used for dynamically generated build variants that can use a base build variants
    compile artifacts to run against.
    \f

    :param project: The evergreen project.
    :param build_variant: The build variant whose artifacts we want to use.
    :param revision: The base revision being run against.
    :param out_file: File to write expansions to.
    :param version_id: Evergreen version id being run against.
    """
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=logging.DEBUG,
        stream=sys.stdout,
    )

    evg_api = RetryingEvergreenApi.get_api(config_file=EVG_CONFIG_FILE)

    version = evg_api.version_by_id(version_id)
    build = version.build_by_variant(build_variant)

    target = TargetBuild(project=project, revision=revision, build_variant=build_variant)
    gather_artifacts_and_update_expansions(build, target, json_artifact, out_file)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
