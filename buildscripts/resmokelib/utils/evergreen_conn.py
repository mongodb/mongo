"""Helper functions to interact with evergreen."""
import os

import requests
import structlog
from requests import HTTPError

from evergreen import RetryingEvergreenApi, Patch

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


def get_evergreen_api(evergreen_config=None):
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


# pylint: disable=protected-access
def get_patch_module_diffs(evg_api, version_id):
    """Get the raw git diffs for all modules."""
    evg_url = evg_api._create_url(f"/patches/{version_id}")
    try:
        res = evg_api._call_api(evg_url)
    except requests.exceptions.HTTPError as err:
        err_res = err.response
        if err_res.status_code == 400:
            LOGGER.debug("Not a patch build task, skipping applying patch",
                         version_id_of_task=version_id)
            return None
        else:
            raise

    patch = Patch(res.json(), evg_api)

    res = {}
    for module_code_change in patch.module_code_changes:
        git_diff_link = module_code_change.raw_link
        raw = evg_api._call_api(git_diff_link)
        diff = raw.text
        res[module_code_change.branch_name] = diff

    return res


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


def get_evergreen_project_and_version(evg_api, commit_hash):
    """Return evergreen project and version by commit hash."""
    from buildscripts.resmokelib import multiversionconstants

    for evg_project in multiversionconstants.EVERGREEN_PROJECTS:
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


def get_evergreen_project(evg_api, evergreen_version_id):
    """Return evergreen project for a given Evergreen version."""
    from buildscripts.resmokelib import multiversionconstants

    for evg_project in multiversionconstants.EVERGREEN_PROJECTS:
        try:
            evg_version = evg_api.version_by_id(evergreen_version_id)
        except HTTPError:
            continue
        else:
            LOGGER.debug("Found evergreen version.",
                         evergreen_version=f"{EVERGREEN_HOST}/version/{evg_version.version_id}")
            return evg_project, evg_version

    raise EvergreenConnError(f"Evergreen version {evergreen_version_id} not found.")


def get_evergreen_versions(evg_api, evg_project):
    """Return the list of evergreen versions by evergreen project name."""
    return evg_api.versions_by_project(evg_project)


def get_compile_artifact_urls(evg_api, evg_version, buildvariant_name, ignore_failed_push=False):
    """Return compile urls from buildvariant in Evergreen version."""
    try:
        build_id = evg_version.build_variants_map[buildvariant_name]
    except KeyError:
        raise EvergreenConnError(f"Buildvariant {buildvariant_name} not found.")

    evg_build = evg_api.build_by_id(build_id)
    LOGGER.debug("Found evergreen build.", evergreen_build=f"{EVERGREEN_HOST}/build/{build_id}")
    evg_tasks = evg_build.get_tasks()
    tasks_wrapper = _filter_successful_tasks(evg_tasks)

    # Ignore push tasks if specified as such, else return no results if push does not exist.
    if ignore_failed_push:
        tasks_wrapper.push_task = None
    elif tasks_wrapper.push_task is None:
        return {}

    return _get_multiversion_urls(tasks_wrapper)


def _get_multiversion_urls(tasks_wrapper):
    compile_artifact_urls = {}

    binary = tasks_wrapper.binary_task
    push = tasks_wrapper.push_task
    symbols = tasks_wrapper.symbols_task

    required_tasks = [binary, push] if push is not None else [binary]

    if all(task and task.status == "success" for task in required_tasks):
        LOGGER.info("Required evergreen task(s) were successful.",
                    required_tasks=f"{required_tasks}",
                    task_id=f"{EVERGREEN_HOST}/task/{required_tasks[0].task_id}")
        evg_artifacts = binary.artifacts
        for artifact in evg_artifacts:
            compile_artifact_urls[artifact.name] = artifact.url

        if symbols and symbols.status == "success":
            for artifact in symbols.artifacts:
                compile_artifact_urls[artifact.name] = artifact.url
        elif symbols and symbols.task_id:
            LOGGER.warning("debug symbol archive was unsuccessful",
                           archive_symbols_task=f"{EVERGREEN_HOST}/task/{symbols.task_id}")

        # Tack on the project id for generating a friendly decompressed name for the artifacts.
        compile_artifact_urls["project_id"] = binary.project_id

    elif all(task for task in required_tasks):
        LOGGER.warning("Required Evergreen task(s) were not successful.",
                       required_tasks=f"{required_tasks}",
                       task_id=f"{EVERGREEN_HOST}/task/{required_tasks[0].task_id}")
    else:
        LOGGER.error("There are no `compile` and/or 'push' tasks in the evergreen build")

    return compile_artifact_urls


class _MultiversionTasks(object):
    """Tasks relevant for multiversion setup."""

    def __init__(self, symbols, binary, push):
        """Init function."""
        self.symbols_task = symbols
        self.binary_task = binary
        self.push_task = push


def _filter_successful_tasks(evg_tasks) -> _MultiversionTasks:
    compile_task = None
    archive_symbols_task = None
    push_task = None
    for evg_task in evg_tasks:
        # Only set the compile task if there isn't one already, otherwise
        # newer tasks like "archive_dist_test_debug" take precedence.
        if evg_task.display_name in ("compile", "archive_dist_test") and compile_task is None:
            compile_task = evg_task
        elif evg_task.display_name == "push":
            push_task = evg_task
        elif evg_task.display_name == "archive_dist_test_debug":
            archive_symbols_task = evg_task
        if compile_task and push_task and archive_symbols_task:
            break
    return _MultiversionTasks(symbols=archive_symbols_task, binary=compile_task, push=push_task)
