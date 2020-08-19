#!/usr/bin/env python3
"""Generate multiversion tests to run in evergreen in parallel."""

import datetime
import logging
import os
import re
import sys
import tempfile
from typing import Optional, List, Set
from collections import defaultdict

from subprocess import check_output

import requests
import click
import structlog

from evergreen.api import RetryingEvergreenApi, EvergreenApi
from shrub.v2 import ShrubProject, FunctionCall, Task, TaskDependency, BuildVariant, ExistingTask

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.multiversionconstants import (LAST_LTS_MONGO_BINARY, REQUIRES_FCV_TAG)
import buildscripts.resmokelib.parser
import buildscripts.util.taskname as taskname
from buildscripts.util.fileops import write_file_to_dir
import buildscripts.evergreen_generate_resmoke_tasks as generate_resmoke
from buildscripts.evergreen_generate_resmoke_tasks import Suite, ConfigOptions
import buildscripts.evergreen_gen_fuzzer_tests as gen_fuzzer
import buildscripts.ciconfig.tags as _tags

# pylint: disable=len-as-condition

LOGGER = structlog.getLogger(__name__)

REQUIRED_CONFIG_KEYS = {
    "build_variant", "fallback_num_sub_suites", "project", "task_id", "task_name",
    "use_multiversion"
}

DEFAULT_CONFIG_VALUES = generate_resmoke.DEFAULT_CONFIG_VALUES
CONFIG_DIR = DEFAULT_CONFIG_VALUES["generated_config_dir"]
DEFAULT_CONFIG_VALUES["is_jstestfuzz"] = False
TEST_SUITE_DIR = DEFAULT_CONFIG_VALUES["test_suites_dir"]
CONFIG_FILE = generate_resmoke.CONFIG_FILE
CONFIG_FORMAT_FN = generate_resmoke.CONFIG_FORMAT_FN
REPL_MIXED_VERSION_CONFIGS = ["new-old-new", "new-new-old", "old-new-new"]
SHARDED_MIXED_VERSION_CONFIGS = ["new-old-old-new"]

BURN_IN_TASK = "burn_in_tests_multiversion"
MULTIVERSION_CONFIG_KEY = "use_in_multiversion"
PASSTHROUGH_TAG = "multiversion_passthrough"
RANDOM_REPLSETS_TAG = "random_multiversion_ds"
BACKPORT_REQUIRED_TAG = "backport_required_multiversion"
EXCLUDE_TAGS = f"{REQUIRES_FCV_TAG},multiversion_incompatible,{BACKPORT_REQUIRED_TAG}"
EXCLUDE_TAGS_FILE = "multiversion_exclude_tags.yml"

# The directory in which BACKPORTS_REQUIRED_FILE resides.
ETC_DIR = "etc"
BACKPORTS_REQUIRED_FILE = "backports_required_for_multiversion_tests.yml"
BACKPORTS_REQUIRED_BASE_URL = "https://raw.githubusercontent.com/mongodb/mongo"


def enable_logging():
    """Enable INFO level logging."""
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=logging.INFO,
        stream=sys.stdout,
    )
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())


def is_suite_sharded(suite_dir: str, suite_name: str) -> bool:
    """Return true if a suite uses ShardedClusterFixture."""
    source_config = generate_resmoke.read_yaml(suite_dir, suite_name + ".yml")
    return source_config["executor"]["fixture"]["class"] == "ShardedClusterFixture"


def get_version_configs(is_sharded: bool) -> List[str]:
    """Get the version configurations to use."""
    if is_sharded:
        return SHARDED_MIXED_VERSION_CONFIGS
    return REPL_MIXED_VERSION_CONFIGS


def get_multiversion_resmoke_args(is_sharded: bool) -> str:
    """Return resmoke args used to configure a cluster for multiversion testing."""
    if is_sharded:
        return "--numShards=2 --numReplSetNodes=2 "
    return "--numReplSetNodes=3 --linearChain=on "


def get_backports_required_last_lts_hash(task_path_suffix: str):
    """Parse the last-lts shell binary to get the commit hash."""
    last_lts_shell_exec = os.path.join(task_path_suffix, LAST_LTS_MONGO_BINARY)
    shell_version = check_output([last_lts_shell_exec, "--version"]).decode('utf-8')
    last_lts_commit_hash = ""
    for line in shell_version.splitlines():
        if "gitVersion" in line:
            version_line = line.split(':')[1]
            # We identify the commit hash as the string enclosed by double quotation marks.
            result = re.search(r'"(.*?)"', version_line)
            if result:
                commit_hash = result.group().strip('"')
                if not commit_hash.isalnum():
                    raise ValueError(f"Error parsing last-lts commit hash. Expected an "
                                     f"alpha-numeric string but got: {commit_hash}")
                return commit_hash
            else:
                break
    raise ValueError("Could not find a valid commit hash from the last-lts mongo binary.")


def get_last_lts_yaml(last_lts_commit_hash):
    """Download BACKPORTS_REQUIRED_FILE from the last LTS commit and return the yaml."""
    LOGGER.info(f"Downloading file from commit hash of last-lts branch {last_lts_commit_hash}")
    response = requests.get(
        f'{BACKPORTS_REQUIRED_BASE_URL}/{last_lts_commit_hash}/{ETC_DIR}/{BACKPORTS_REQUIRED_FILE}')
    # If the response was successful, no exception will be raised.
    response.raise_for_status()

    last_lts_file = f"{last_lts_commit_hash}_{BACKPORTS_REQUIRED_FILE}"
    temp_dir = tempfile.mkdtemp()
    with open(os.path.join(temp_dir, last_lts_file), "w") as fileh:
        fileh.write(response.text)

    backports_required_last_lts = generate_resmoke.read_yaml(temp_dir, last_lts_file)
    return backports_required_last_lts


def _generate_resmoke_args(suite_file: str, mixed_version_config: str, is_sharded: bool, options,
                           burn_in_test: Optional[str]) -> str:
    return (
        f"{options.resmoke_args} --suite={suite_file} --mixedBinVersions={mixed_version_config}"
        f" --excludeWithAnyTags={EXCLUDE_TAGS},{generate_resmoke.remove_gen_suffix(options.task)}_{BACKPORT_REQUIRED_TAG} --tagFile={os.path.join(CONFIG_DIR, EXCLUDE_TAGS_FILE)} --originSuite={options.suite} "
        f" {get_multiversion_resmoke_args(is_sharded)} {burn_in_test if burn_in_test else ''}")


class EvergreenMultiversionConfigGenerator(object):
    """Generate evergreen configurations for multiversion tests."""

    def __init__(self, evg_api: EvergreenApi, options):
        """Create new EvergreenMultiversionConfigGenerator object."""
        self.evg_api = evg_api
        self.options = options
        # Strip the "_gen" suffix appended to the name of tasks generated by evergreen.
        self.task = generate_resmoke.remove_gen_suffix(self.options.task)

    def _generate_sub_task(self, mixed_version_config: str, task: str, task_index: int, suite: str,
                           num_suites: int, is_sharded: bool,
                           burn_in_test: Optional[str] = None) -> Task:
        # pylint: disable=too-many-arguments
        """
        Generate a sub task to be run with the provided suite and  mixed version config.

        :param mixed_version_config: mixed version configuration.
        :param task: Name of task.
        :param task_index: Index of task to generate.
        :param suite: Name of suite being generated.
        :param num_suites: Number os suites being generated.
        :param is_sharded: If this is being generated for a sharded configuration.
        :param burn_in_test: If generation is for burn_in, burn_in options to use.
        :return: Shrub configuration for task specified.
        """
        # Create a sub task name appended with the task_index and build variant name.
        task_name = f"{task}_{mixed_version_config}"
        sub_task_name = taskname.name_generated_task(task_name, task_index, num_suites,
                                                     self.options.variant)
        gen_task_name = BURN_IN_TASK if burn_in_test is not None else self.task

        run_tests_vars = {
            "resmoke_args":
                _generate_resmoke_args(suite, mixed_version_config, is_sharded, self.options,
                                       burn_in_test),
            "task":
                gen_task_name,
        }

        commands = [
            FunctionCall("do setup"),
            # Fetch and download the proper mongod binaries before running multiversion tests.
            FunctionCall("do multiversion setup"),
            FunctionCall("run generated tests", run_tests_vars),
        ]

        return Task(sub_task_name, commands, {TaskDependency("compile")})

    def _generate_burn_in_execution_tasks(self, version_configs: List[str], suites: List[Suite],
                                          burn_in_test: str, burn_in_idx: int,
                                          is_sharded: bool) -> Set[Task]:
        """
        Generate shrub tasks for burn_in executions.

        :param version_configs: Version configs to generate for.
        :param suites: Suites to generate.
        :param burn_in_test: burn_in_test configuration.
        :param burn_in_idx: Index of burn_in task being generated.
        :param is_sharded: If configuration should be generated for sharding tests.
        :return: Set of generated shrub tasks.
        """
        # pylint: disable=too-many-arguments
        burn_in_prefix = "burn_in_multiversion"
        task = f"{burn_in_prefix}:{self.task}"

        # For burn in tasks, it doesn't matter which generated suite yml to use as all the
        # yaml configurations are the same.
        source_suite = os.path.join(CONFIG_DIR, suites[0].name + ".yml")
        tasks = {
            self._generate_sub_task(version_config, task, burn_in_idx, source_suite, 1, is_sharded,
                                    burn_in_test)
            for version_config in version_configs
        }

        return tasks

    def _get_fuzzer_options(self, version_config: str, is_sharded: bool) -> ConfigOptions:
        """
        Get options to generate fuzzer tasks.

        :param version_config: Version configuration to generate for.
        :param is_sharded: If configuration is for sharded tests.
        :return: Configuration options to generate fuzzer tasks.
        """
        fuzzer_config = ConfigOptions(self.options.config)
        fuzzer_config.name = f"{self.options.suite}_multiversion"
        fuzzer_config.num_files = int(self.options.num_files)
        fuzzer_config.num_tasks = int(self.options.num_tasks)
        add_resmoke_args = get_multiversion_resmoke_args(is_sharded)
        fuzzer_config.resmoke_args = f"{self.options.resmoke_args} "\
            f"--mixedBinVersions={version_config} {add_resmoke_args}"
        return fuzzer_config

    def _generate_fuzzer_tasks(self, build_variant: BuildVariant, version_configs: List[str],
                               is_sharded: bool) -> None:
        """
        Generate fuzzer tasks and add them to the given build variant.

        :param build_variant: Build variant to add tasks to.
        :param version_configs: Version configurations to generate.
        :param is_sharded: Should configuration be generated for sharding.
        """
        tasks = set()
        for version_config in version_configs:
            fuzzer_config = self._get_fuzzer_options(version_config, is_sharded)
            task_name = f"{fuzzer_config.name}_{version_config}"
            sub_tasks = gen_fuzzer.generate_fuzzer_sub_tasks(task_name, fuzzer_config)
            tasks = tasks.union(sub_tasks)

        existing_tasks = {ExistingTask(f"{self.options.suite}_multiversion_gen")}
        build_variant.display_task(self.task, tasks, execution_existing_tasks=existing_tasks)

    def generate_resmoke_suites(self) -> List[Suite]:
        """Generate the resmoke configuration files for this generator."""
        # Divide tests into suites based on run-time statistics for the last
        # LOOKBACK_DURATION_DAYS. Tests without enough run-time statistics will be placed
        # in the misc suite.
        gen_suites = generate_resmoke.GenerateSubSuites(self.evg_api, self.options)
        end_date = datetime.datetime.utcnow().replace(microsecond=0)
        start_date = end_date - datetime.timedelta(days=generate_resmoke.LOOKBACK_DURATION_DAYS)
        suites = gen_suites.calculate_suites(start_date, end_date)
        # Render the given suites into yml files that can be used by resmoke.py.
        config_file_dict = generate_resmoke.render_suite_files(suites, self.options.suite,
                                                               gen_suites.test_list, TEST_SUITE_DIR,
                                                               self.options.create_misc_suite)
        generate_resmoke.write_file_dict(CONFIG_DIR, config_file_dict)

        return suites

    def get_burn_in_tasks(self, burn_in_test: str, burn_in_idx: int) -> Set[Task]:
        """
        Get the burn_in tasks being generated.

        :param burn_in_test: Burn in test configuration.
        :param burn_in_idx: Index of burn_in configuration being generated.
        :return: Set of shrub tasks for the specified burn_in.
        """
        is_sharded = is_suite_sharded(TEST_SUITE_DIR, self.options.suite)
        version_configs = get_version_configs(is_sharded)
        suites = self.generate_resmoke_suites()

        # Generate the subtasks to run burn_in_test against the appropriate mixed version
        # configurations. The display task is defined later as part of generating the burn
        # in tests.
        tasks = self._generate_burn_in_execution_tasks(version_configs, suites, burn_in_test,
                                                       burn_in_idx, is_sharded)
        return tasks

    def generate_evg_tasks(self, build_variant: BuildVariant) -> None:
        # pylint: disable=too-many-locals
        """
        Generate evergreen tasks for multiversion tests.

        The number of tasks generated equals
        (the number of version configs) * (the number of generated suites).

        :param build_variant: Build variant to add generated configuration to.
        """
        is_sharded = is_suite_sharded(TEST_SUITE_DIR, self.options.suite)
        version_configs = get_version_configs(is_sharded)

        if self.options.is_jstestfuzz:
            self._generate_fuzzer_tasks(build_variant, version_configs, is_sharded)
            return

        suites = self.generate_resmoke_suites()
        sub_tasks = set()
        for version_config in version_configs:
            idx = 0
            for suite in suites:
                # Generate the newly divided test suites
                source_suite = os.path.join(CONFIG_DIR, suite.name + ".yml")
                sub_tasks.add(
                    self._generate_sub_task(version_config, self.task, idx, source_suite,
                                            len(suites), is_sharded))
                idx += 1

            # Also generate the misc task.
            misc_suite_name = "{0}_misc".format(self.options.suite)
            misc_suite = os.path.join(CONFIG_DIR, misc_suite_name + ".yml")
            sub_tasks.add(
                self._generate_sub_task(version_config, self.task, idx, misc_suite, 1, is_sharded))
            idx += 1

        build_variant.display_task(self.task, sub_tasks,
                                   execution_existing_tasks={ExistingTask(f"{self.task}_gen")})

    def run(self) -> None:
        """Generate multiversion suites that run within a specified target execution time."""
        if not generate_resmoke.should_tasks_be_generated(self.evg_api, self.options.task_id):
            LOGGER.info("Not generating configuration due to previous successful generation.")
            return

        build_variant = BuildVariant(self.options.variant)
        self.generate_evg_tasks(build_variant)

        shrub_project = ShrubProject.empty()
        shrub_project.add_build_variant(build_variant)
        write_file_to_dir(CONFIG_DIR, f"{self.task}.json", shrub_project.json())

        if len(os.listdir(CONFIG_DIR)) == 0:
            raise RuntimeError(
                f"Multiversion suite generator unexpectedly yielded no configuration in '{CONFIG_DIR}'"
            )


@click.group()
def main():
    """Serve as an entry point for the 'run' and 'generate-exclude-tags' commands."""
    pass


@main.command("run")
@click.option("--expansion-file", type=str, required=True,
              help="Location of expansions file generated by evergreen.")
@click.option("--evergreen-config", type=str, default=CONFIG_FILE,
              help="Location of evergreen configuration file.")
def run_generate_tasks(expansion_file: str, evergreen_config: Optional[str] = None):
    """
    Create a configuration for generate tasks to create sub suites for the specified resmoke suite.

    Tests using ReplicaSetFixture will be generated to use 3 nodes and linear_chain=True.
    Tests using ShardedClusterFixture will be generated to use 2 shards with 2 nodes each.
    The different binary version configurations tested are stored in REPL_MIXED_VERSION_CONFIGS
    and SHARDED_MIXED_VERSION_CONFIGS.

    The `--expansion-file` should contain all the configuration needed to generate the tasks.
    \f
    :param expansion_file: Configuration file.
    :param evergreen_config: Evergreen configuration file.
    """
    evg_api = RetryingEvergreenApi.get_api(config_file=evergreen_config)
    config_options = generate_resmoke.ConfigOptions.from_file(
        expansion_file, REQUIRED_CONFIG_KEYS, DEFAULT_CONFIG_VALUES, CONFIG_FORMAT_FN)
    config_generator = EvergreenMultiversionConfigGenerator(evg_api, config_options)
    config_generator.run()


@main.command("generate-exclude-tags")
@click.option("--task-path-suffix", type=str, required=True,
              help="The directory in which multiversion binaries are stored.")
@click.option("--output", type=str, default=os.path.join(CONFIG_DIR, EXCLUDE_TAGS_FILE),
              show_default=True, help="Where to output the generated tags.")
def generate_exclude_yaml(task_path_suffix: str, output: str) -> None:
    # pylint: disable=too-many-locals
    """
    Create a tag file associating multiversion tests to tags for exclusion.

    Compares the BACKPORTS_REQUIRED_FILE on the current branch with the same file on the
    last-lts branch to determine which tests should be blacklisted.
    """

    enable_logging()

    location, _ = os.path.split(os.path.abspath(output))
    if not os.path.isdir(location):
        LOGGER.info(f"Cannot write to {output}. Not generating tag file.")
        return

    backports_required_latest = generate_resmoke.read_yaml(ETC_DIR, BACKPORTS_REQUIRED_FILE)

    # Get the state of the backports_required_for_multiversion_tests.yml file for the last-lts
    # binary we are running tests against. We do this by using the commit hash from the last-lts
    # mongo shell executable.
    last_lts_commit_hash = get_backports_required_last_lts_hash(task_path_suffix)

    # Get the yaml contents from the last-lts commit.
    backports_required_last_lts = get_last_lts_yaml(last_lts_commit_hash)

    def diff(list1, list2):
        return [elem for elem in (list1 or []) if elem not in (list2 or [])]

    suites_latest = backports_required_latest["suites"] or {}
    # Check if the changed syntax for etc/backports_required_multiversion.yml has been backported.
    # This variable and all branches where it's not set can be deleted after backporting the change.
    change_backported = "all" in backports_required_last_lts.keys()
    if change_backported:
        always_exclude = diff(backports_required_latest["all"], backports_required_last_lts["all"])
        suites_last_lts: defaultdict = defaultdict(list, backports_required_last_lts["suites"])
    else:
        always_exclude = backports_required_latest["all"] or []
        suites_last_lts = defaultdict(list, backports_required_last_lts)
        for suite in suites_latest.keys():
            for elem in suites_last_lts[suite] or []:
                if elem in always_exclude:
                    always_exclude.remove(elem)

    tags = _tags.TagsConfig()

    # Tag tests that are excluded from every suite.
    for elem in always_exclude:
        tags.add_tag("js_test", elem["test_file"], BACKPORT_REQUIRED_TAG)

    # Tag tests that are excluded on a suite-by-suite basis.
    for suite in suites_latest.keys():
        test_set = set()
        for elem in diff(suites_latest[suite], suites_last_lts[suite]):
            test_set.add(elem["test_file"])
        for test in test_set:
            tags.add_tag("js_test", test, f"{suite}_{BACKPORT_REQUIRED_TAG}")

    LOGGER.info(f"Writing exclude tags to {output}.")
    tags.write_file(filename=output,
                    preamble="Tag file that specifies exclusions from multiversion suites.")


if __name__ == '__main__':
    main()  # pylint: disable=no-value-for-parameter
