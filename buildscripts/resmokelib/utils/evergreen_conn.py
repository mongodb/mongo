"""Helper functions to interact with evergreen."""

import os
import pathlib
from collections import deque
from pathlib import Path
from typing import Deque, Iterator, List, Optional, Set, Union

import requests
import structlog
from requests import HTTPError

from buildscripts.resmokelib.setup_multiversion.config import SetupMultiversionConfig
from evergreen import Patch, RetryingEvergreenApi, Task, Version

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


def _find_evergreen_yaml_candidates() -> List[str]:
    # Common for machines in Evergreen
    candidates: List[Union[str, Path]] = [os.getcwd()]

    cwd = pathlib.Path(os.getcwd())
    # add every path that is the parent of CWD as well
    for parent in cwd.parents:
        candidates.append(parent)

    # Common for local machines
    candidates.append(os.path.expanduser(os.path.join("~", ".evergreen.yml")))

    out = []
    for path in candidates:
        file = os.path.join(path, ".evergreen.yml")
        if os.path.isfile(file):
            out.append(file)

    return out


def get_evergreen_api(evergreen_config=None):
    """Return evergreen API."""
    if evergreen_config:
        possible_configs = [evergreen_config]
    else:
        possible_configs = _find_evergreen_yaml_candidates()

    if not possible_configs:
        LOGGER.error("Could not find .evergreen.yml", candidates=possible_configs)
        raise RuntimeError("Could not find .evergreen.yml")

    last_ex = None
    for config in possible_configs:
        try:
            return RetryingEvergreenApi.get_api(config_file=config)
        #
        except Exception as ex:  # pylint: disable=broad-except
            last_ex = ex
            continue

    LOGGER.error(
        "Could not connect to Evergreen with any .evergreen.yml files available on this system",
        config_file_candidates=possible_configs,
    )
    raise last_ex


def get_buildvariant_name(
    config: SetupMultiversionConfig, edition, platform, architecture, major_minor_version
):
    """Return Evergreen buildvariant name."""

    buildvariant_name = ""
    evergreen_buildvariants = config.evergreen_buildvariants

    for buildvariant in evergreen_buildvariants:
        if (
            buildvariant.edition == edition
            and buildvariant.platform == platform
            and buildvariant.architecture == architecture
        ):
            versions = buildvariant.versions
            if major_minor_version in versions:
                buildvariant_name = buildvariant.name
                break
            elif not versions:
                buildvariant_name = buildvariant.name

    return buildvariant_name


# pylint: disable=protected-access
def get_patch_module_diffs(evg_api: RetryingEvergreenApi, version_id):
    """Get the raw git diffs for all modules."""
    evg_url = evg_api._create_url(f"/patches/{version_id}")
    try:
        res = evg_api._call_api(evg_url)
    except requests.exceptions.HTTPError as err:
        err_res = err.response
        if err_res.status_code == 400:
            LOGGER.debug(
                "Not a patch build task, skipping applying patch", version_id_of_task=version_id
            )
            return None
        else:
            raise

    patch = Patch(res.json(), evg_api)

    patch_module_diff = {}
    for module_code_change in patch.module_code_changes:
        git_diff_link = module_code_change.raw_link
        raw = evg_api._call_api(git_diff_link)
        diff = raw.text
        patch_module_diff[module_code_change.branch_name] = diff

    return patch_module_diff


def get_generic_buildvariant_name(config: SetupMultiversionConfig, major_minor_version):
    """Return Evergreen buildvariant name for generic platform."""

    LOGGER.info(
        "Falling back to generic architecture.",
        edition=GENERIC_EDITION,
        platform=GENERIC_PLATFORM,
        architecture=GENERIC_ARCHITECTURE,
    )

    generic_buildvariant_name = get_buildvariant_name(
        config=config,
        edition=GENERIC_EDITION,
        platform=GENERIC_PLATFORM,
        architecture=GENERIC_ARCHITECTURE,
        major_minor_version=major_minor_version,
    )

    if not generic_buildvariant_name:
        raise EvergreenConnError("Generic architecture buildvariant not found.")

    return generic_buildvariant_name


def get_evergreen_version(evg_api: RetryingEvergreenApi, evg_ref: str) -> Optional[Version]:
    """Return evergreen version by reference (commit_hash or evergreen_version_id)."""
    from buildscripts.resmokelib import multiversionconstants

    # Evergreen reference as evergreen_version_id
    evg_refs = [evg_ref]
    # Evergreen reference as {project_name}_{commit_hash}
    evg_refs.extend(
        f"{proj.replace('-', '_')}_{evg_ref}" for proj in multiversionconstants.EVERGREEN_PROJECTS
    )

    for ref in evg_refs:
        try:
            evg_version = evg_api.version_by_id(ref)
        except HTTPError:
            continue
        else:
            LOGGER.debug(
                "Found evergreen version.",
                evergreen_version=f"{EVERGREEN_HOST}/version/{evg_version.version_id}",
            )
            return evg_version

    return None


def get_evergreen_versions(evg_api: RetryingEvergreenApi, evg_project: str) -> Iterator[Version]:
    """Return the list of evergreen versions by evergreen project name."""
    return evg_api.versions_by_project(evg_project)


def get_compile_artifact_urls(
    evg_api: RetryingEvergreenApi, evg_version: Version, buildvariant_name, ignore_failed_push=False
):
    """Return compile urls from buildvariant in Evergreen version."""
    try:
        build_id = evg_version.build_variants_map[buildvariant_name]
    except KeyError:
        raise EvergreenConnError(f"Buildvariant {buildvariant_name} not found.")

    evg_build = evg_api.build_by_id(build_id)
    LOGGER.debug("Found evergreen build.", evergreen_build=f"{EVERGREEN_HOST}/build/{build_id}")
    evg_tasks: Deque[Union[Task, str]] = deque(evg_build.get_tasks())
    tasks_wrapper = _filter_successful_tasks(evg_api, evg_tasks)
    LOGGER.info(
        "Found the following multiversion tasks",
        symbols_task=tasks_wrapper.symbols_task,
        binary_task=tasks_wrapper.binary_task,
        push_task=tasks_wrapper.push_task,
    )

    # Ignore push tasks if specified as such, else return no results if push does not exist.
    if ignore_failed_push:
        tasks_wrapper.push_task = None
    elif tasks_wrapper.push_task is None:
        return {}

    return _get_multiversion_urls(tasks_wrapper)


class _MultiversionTasks(object):
    """Tasks relevant for multiversion setup."""

    def __init__(
        self, symbols: Union[Task, None], binary: Union[Task, None], push: Union[Task, None]
    ):
        """Init function."""
        self.symbols_task = symbols
        self.binary_task = binary
        self.push_task = push


def _get_multiversion_urls(tasks_wrapper: _MultiversionTasks):
    compile_artifact_urls = {}

    binary = tasks_wrapper.binary_task
    push = tasks_wrapper.push_task
    symbols = tasks_wrapper.symbols_task

    required_tasks = [binary, push] if push is not None else [binary]

    if all(task and task.status == "success" for task in required_tasks):
        LOGGER.info(
            "Required evergreen task(s) were successful.",
            required_tasks=f"{required_tasks}",
            task_id=f"{EVERGREEN_HOST}/task/{required_tasks[0].task_id}",
        )
        evg_artifacts = binary.artifacts
        for artifact in evg_artifacts:
            compile_artifact_urls[artifact.name] = artifact.url

        if symbols and symbols.status == "success":
            for artifact in symbols.artifacts:
                compile_artifact_urls[artifact.name] = artifact.url
        elif symbols and symbols.task_id:
            LOGGER.warning(
                "debug symbol archive was unsuccessful",
                archive_symbols_task=f"{EVERGREEN_HOST}/task/{symbols.task_id}",
            )

        # Tack on the project id for generating a friendly decompressed name for the artifacts.
        compile_artifact_urls["project_identifier"] = binary.project_identifier

    elif all(task for task in required_tasks):
        LOGGER.warning(
            "Required Evergreen task(s) were not successful.",
            required_tasks=f"{required_tasks}",
            task_id=f"{EVERGREEN_HOST}/task/{required_tasks[0].task_id}",
        )
    else:
        LOGGER.error("There are no `compile` and/or 'push' tasks in the evergreen build")

    return compile_artifact_urls


def _filter_successful_tasks(
    evg_api: RetryingEvergreenApi, evg_tasks: Deque[Union[Task, str]]
) -> _MultiversionTasks:
    """
    We want to filter successful tasks in order by variant then by dependent tasks to find the compile tasks.

    evg_tasks: A queue of Tasks or task_ids (str)
    """
    compile_task = None
    archive_symbols_task = None
    push_task = None
    seen_task_ids: Set[str] = set()
    while evg_tasks:
        evg_task = evg_tasks.popleft()

        # If we have checked this task before skip it
        task_id = evg_task if isinstance(evg_task, str) else evg_task.task_id
        if task_id in seen_task_ids:
            continue
        seen_task_ids.add(task_id)

        if isinstance(evg_task, str):
            evg_task = evg_api.task_by_id(evg_task)

        # Only set the compile task if there isn't one already, otherwise
        # newer tasks like "archive_dist_test_debug" take precedence.
        # Use `get_execution_or_self` to prevent grabbing an unfinished restarted executed task.
        if (
            evg_task.display_name
            in ("compile", "archive_dist_test", "archive_dist_test_future_git_tag_multiversion")
            and compile_task is None
        ):
            compile_task = evg_task.get_execution_or_self(0)
            # archive_dist_test_debug might not be in the dep chain
            # it should always be in the same build variant as the compile task
            evg_tasks.extend(evg_api.tasks_by_build(compile_task.build_id))

        elif evg_task.display_name == "push":
            push_task = evg_task.get_execution_or_self(0)
        elif (
            evg_task.display_name
            in ("archive_dist_test_debug", "archive_dist_test_debug_future_git_tag_multiversion")
            and archive_symbols_task is None
        ):
            archive_symbols_task = evg_task.get_execution_or_self(0)
        if compile_task and push_task and archive_symbols_task:
            break

        dependent_tasks = evg_task.depends_on if evg_task.depends_on else []
        for dep_task in dependent_tasks:
            evg_tasks.append(dep_task["id"])
    return _MultiversionTasks(symbols=archive_symbols_task, binary=compile_task, push=push_task)
