"""Helper functions to interact with evergreen."""
import os

import structlog
from requests import HTTPError

from evergreen import RetryingEvergreenApi

EVERGREEN_HOST = "https://evergreen.mongodb.com"
EVERGREEN_CONFIG_LOCATIONS = (
    # Common for machines in Evergreen
    os.path.join(os.getcwd(), ".evergreen.yml"),
    # Common for local machines
    os.path.expanduser(os.path.join("~", ".evergreen.yml")),
)

GENERIC_EDITION = "base"
GENERIC_PLATFORM = "linux_x86_64"
GENERIC_ARCHITECTURE = "x86_64"

LOGGER = structlog.getLogger(__name__)


class EvergreenConnError(Exception):
    """Errors in evergreen_conn.py."""

    pass


def get_evergreen_api(evergreen_config):
    """Return evergreen API."""
    config_to_pass = evergreen_config
    if not config_to_pass:
        # Pickup the first config file found in common locations.
        for file in EVERGREEN_CONFIG_LOCATIONS:
            if os.path.isfile(file):
                config_to_pass = file
                break
    try:
        evg_api = RetryingEvergreenApi.get_api(config_file=config_to_pass)
    except Exception as ex:
        LOGGER.error("Most likely something is wrong with evergreen config file.",
                     config_file=config_to_pass)
        raise ex
    else:
        return evg_api


def get_buildvariant_name(config, edition, platform, architecture, major_minor_version):
    """Return Evergreen buildvariant name."""

    buildvariant_name = ""
    evergreen_buildvariants = config.evergreen_buildvariants

    for buildvariant in evergreen_buildvariants:
        if (buildvariant.edition == edition and buildvariant.platform == platform
                and buildvariant.architecture == architecture):
            versions = buildvariant.versions
            if major_minor_version in versions:
                buildvariant_name = buildvariant.name
                break
            elif not versions:
                buildvariant_name = buildvariant.name

    return buildvariant_name


def get_generic_buildvariant_name(config, major_minor_version):
    """Return Evergreen buildvariant name for generic platform."""

    LOGGER.info("Falling back to generic architecture.", edition=GENERIC_EDITION,
                platform=GENERIC_PLATFORM, architecture=GENERIC_ARCHITECTURE)

    generic_buildvariant_name = get_buildvariant_name(
        config=config, edition=GENERIC_EDITION, platform=GENERIC_PLATFORM,
        architecture=GENERIC_ARCHITECTURE, major_minor_version=major_minor_version)

    if not generic_buildvariant_name:
        raise EvergreenConnError("Generic architecture buildvariant not found.")

    return generic_buildvariant_name


def get_evergreen_project_and_version(config, evg_api, commit_hash):
    """Return evergreen project and version by commit hash."""

    for evg_project in config.evergreen_projects:
        try:
            version_id = evg_project.replace("-", "_") + "_" + commit_hash
            evg_version = evg_api.version_by_id(version_id)
        except HTTPError:
            continue
        else:
            LOGGER.debug("Found evergreen version.",
                         evergreen_version=f"{EVERGREEN_HOST}/version/{evg_version.version_id}")
            return evg_project, evg_version

    raise EvergreenConnError(f"Evergreen version for commit hash {commit_hash} not found.")


def get_evergreen_versions(evg_api, evg_project):
    """Return the list of evergreen versions by evergreen project name."""
    return evg_api.versions_by_project(evg_project)


def get_compile_artifact_urls(evg_api, evg_version, buildvariant_name):
    """Return compile urls from buildvariant in Evergreen version."""
    compile_artifact_urls = {}

    try:
        build_id = evg_version.build_variants_map[buildvariant_name]
    except KeyError:
        raise EvergreenConnError(f"Buildvariant {buildvariant_name} not found.")
    else:
        evg_build = evg_api.build_by_id(build_id)
        LOGGER.debug("Found evergreen build.", evergreen_build=f"{EVERGREEN_HOST}/build/{build_id}")
        evg_tasks = evg_build.get_tasks()
        compile_task = None
        push_task = None

        for evg_task in evg_tasks:
            # Only set the compile task if there isn't one already, otherwise
            # newer tasks like "archive_dist_test_debug" take precedence.
            if evg_task.display_name in ("compile", "archive_dist_test") and compile_task is None:
                compile_task = evg_task
            elif evg_task.display_name == "push":
                push_task = evg_task
            if compile_task and push_task:
                break

        if compile_task and push_task and compile_task.status == push_task.status == "success":
            LOGGER.info("Found successful evergreen tasks.",
                        compile_task=f"{EVERGREEN_HOST}/task/{compile_task.task_id}",
                        push_task=f"{EVERGREEN_HOST}/task/{push_task.task_id}")
            evg_artifacts = compile_task.artifacts
            for artifact in evg_artifacts:
                compile_artifact_urls[artifact.name] = artifact.url

            # Tack on the project id for generating a friendly decompressed name for the artifacts.
            compile_artifact_urls["project_id"] = compile_task.project_id

        elif compile_task and push_task:
            LOGGER.warning("Found evergreen tasks, but they are not both successful.",
                           compile_task=f"{EVERGREEN_HOST}/task/{compile_task.task_id}",
                           push_task=f"{EVERGREEN_HOST}/task/{push_task.task_id}")
        else:
            LOGGER.error("There are no `compile` and/or 'push' tasks in the evergreen build.",
                         evergreen_build=f"{EVERGREEN_HOST}/build/{build_id}")

    return compile_artifact_urls
