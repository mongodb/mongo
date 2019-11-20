#!/usr/bin/env python3
"""Generate multiversion tests to run in evergreen in parallel."""

import datetime
from datetime import timedelta
import logging
import os
import sys
import tempfile

from collections import namedtuple
from subprocess import check_output

import requests
import yaml
import click
import structlog

from evergreen.api import RetryingEvergreenApi
from git import Repo
from shrub.config import Configuration
from shrub.command import CommandDefinition
from shrub.task import TaskDependency
from shrub.variant import DisplayTaskDefinition
from shrub.variant import TaskSpec

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.multiversionconstants import (LAST_STABLE_MONGO_BINARY,
                                                           REQUIRES_FCV_TAG)
import buildscripts.resmokelib.parser
import buildscripts.util.read_config as read_config
import buildscripts.util.taskname as taskname
import buildscripts.evergreen_generate_resmoke_tasks as generate_resmoke
import buildscripts.evergreen_gen_fuzzer_tests as gen_fuzzer

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
BURN_IN_CONFIG_KEY = "use_in_multiversion_burn_in_tests"
PASSTHROUGH_TAG = "multiversion_passthrough"
EXCLUDE_TAGS = f"{REQUIRES_FCV_TAG},multiversion_incompatible"

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


def prepare_directory_for_suite(directory):
    """Ensure that directory exists."""
    if not os.path.exists(directory):
        os.makedirs(directory)


def is_suite_sharded(suite_dir, suite_name):
    """Return true if a suite uses ShardedClusterFixture."""
    source_config = generate_resmoke.read_yaml(suite_dir, suite_name + ".yml")
    return source_config["executor"]["fixture"]["class"] == "ShardedClusterFixture"


def update_suite_config_for_multiversion_replset(suite_config):
    """Update suite_config with arguments for multiversion tests using ReplicaSetFixture."""
    suite_config["executor"]["fixture"]["num_nodes"] = 3
    suite_config["executor"]["fixture"]["linear_chain"] = True


def update_suite_config_for_multiversion_sharded(suite_config):
    """Update suite_config with arguments for multiversion tests using ShardedClusterFixture."""

    fixture_config = suite_config["executor"]["fixture"]
    default_shards = "default_shards"
    default_num_nodes = "default_nodes"
    base_num_shards = (default_shards if "num_shards" not in fixture_config
                       or not fixture_config["num_shards"] else fixture_config["num_shards"])
    base_num_rs_nodes_per_shard = (default_num_nodes
                                   if "num_rs_nodes_per_shard" not in fixture_config
                                   or not fixture_config["num_rs_nodes_per_shard"] else
                                   fixture_config["num_rs_nodes_per_shard"])

    if base_num_shards is not default_shards or base_num_rs_nodes_per_shard is not default_num_nodes:
        num_shard_num_nodes_pair = "{}-{}".format(base_num_shards, base_num_rs_nodes_per_shard)
        assert num_shard_num_nodes_pair in {"default_shards-2", "2-default_nodes", "2-3"}, \
               "The multiversion suite runs sharded clusters with 2 shards and 2 nodes per shard. "\
               " acceptable, please add '{}' to this assert.".format(num_shard_num_nodes_pair)

    suite_config["executor"]["fixture"]["num_shards"] = 2
    suite_config["executor"]["fixture"]["num_rs_nodes_per_shard"] = 2


def get_backports_required_last_stable_hash(task_path_suffix):
    """Parse the last-stable shell binary to get the commit hash."""
    last_stable_shell_exec = os.path.join(task_path_suffix, LAST_STABLE_MONGO_BINARY)
    shell_version = check_output([last_stable_shell_exec, "--version"]).decode('utf-8')
    last_stable_commit_hash = ""
    for line in shell_version.splitlines():
        if "git version" in line:
            last_stable_commit_hash = line.split(':')[1].strip()
            break
    if not last_stable_commit_hash:
        raise ValueError("Could not find a valid commit hash from the last-stable mongo binary.")
    return last_stable_commit_hash


def get_last_stable_yaml(last_stable_commit_hash, suite_name):
    """Download BACKPORTS_REQUIRED_FILE from the last stable commit and return the yaml."""
    LOGGER.info(
        f"Downloading file from commit hash of last-stable branch {last_stable_commit_hash}")
    response = requests.get(
        f'{BACKPORTS_REQUIRED_BASE_URL}/{last_stable_commit_hash}/{ETC_DIR}/{BACKPORTS_REQUIRED_FILE}'
    )
    # If the response was successful, no exception will be raised.
    response.raise_for_status()

    last_stable_file = f"{last_stable_commit_hash}_{BACKPORTS_REQUIRED_FILE}"
    temp_dir = tempfile.mkdtemp()
    with open(os.path.join(temp_dir, last_stable_file), "w") as fileh:
        fileh.write(response.text)

    backports_required_last_stable = generate_resmoke.read_yaml(temp_dir, last_stable_file)
    return backports_required_last_stable[suite_name]


class MultiversionConfig(object):
    """An object containing the configurations to generate and run the multiversion tests with."""

    def __init__(self, update_yaml, version_configs):
        """Create new MultiversionConfig object."""
        self.update_yaml = update_yaml
        self.version_configs = version_configs


class EvergreenConfigGenerator(object):
    """Generate evergreen configurations for multiversion tests."""

    def __init__(self, evg_api, evg_config, options):
        """Create new EvergreenConfigGenerator object."""
        self.evg_api = evg_api
        self.evg_config = evg_config
        self.options = options
        self.task_names = []
        self.task_specs = []
        # Strip the "_gen" suffix appended to the name of tasks generated by evergreen.
        self.task = generate_resmoke.remove_gen_suffix(self.options.task)

    def _generate_sub_task(self, mixed_version_config, task, task_index, suite, num_suites,
                           burn_in_test=None):
        # pylint: disable=too-many-arguments
        """Generate a sub task to be run with the provided suite and  mixed version config."""

        # Create a sub task name appended with the task_index and build variant name.
        task_name = "{0}_{1}".format(task, mixed_version_config)
        sub_task_name = taskname.name_generated_task(task_name, task_index, num_suites,
                                                     self.options.variant)
        self.task_names.append(sub_task_name)
        self.task_specs.append(TaskSpec(sub_task_name))
        task = self.evg_config.task(sub_task_name)

        gen_task_name = BURN_IN_TASK if burn_in_test is not None else self.task

        commands = [
            CommandDefinition().function("do setup"),
            # Fetch and download the proper mongod binaries before running multiversion tests.
            CommandDefinition().function("do multiversion setup")
        ]
        run_tests_vars = {
            "resmoke_args":
                "{0} --suite={1} --mixedBinVersions={2} --excludeWithAnyTags={3} ".format(
                    self.options.resmoke_args, suite, mixed_version_config, EXCLUDE_TAGS),
            "task":
                gen_task_name,
        }
        if burn_in_test is not None:
            run_tests_vars["resmoke_args"] += burn_in_test

        commands.append(CommandDefinition().function("run generated tests").vars(run_tests_vars))
        task.dependency(TaskDependency("compile")).commands(commands)

    def _write_evergreen_config_to_file(self, task_name):
        """Save evergreen config to file."""
        if not os.path.exists(CONFIG_DIR):
            os.makedirs(CONFIG_DIR)

        with open(os.path.join(CONFIG_DIR, task_name + ".json"), "w") as file_handle:
            file_handle.write(self.evg_config.to_json())

    def create_display_task(self, task_name, task_specs, task_list):
        """Create the display task definition for the MultiversionConfig object."""
        dt = DisplayTaskDefinition(task_name).execution_tasks(task_list)\
            .execution_task("{0}_gen".format(task_name))
        self.evg_config.variant(self.options.variant).tasks(task_specs).display_task(dt)

    def _generate_burn_in_execution_tasks(self, config, suites, burn_in_test, burn_in_idx):
        burn_in_prefix = "burn_in_multiversion"
        task = "{0}:{1}".format(burn_in_prefix, self.task)

        for version_config in config.version_configs:
            # For burn in tasks, it doesn't matter which generated suite yml to use as all the
            # yaml configurations are the same.
            source_suite = os.path.join(CONFIG_DIR, suites[0].name + ".yml")
            self._generate_sub_task(version_config, task, burn_in_idx, source_suite, 1,
                                    burn_in_test)
        return self.evg_config

    def _get_fuzzer_options(self, version_config, suite_file):
        fuzzer_config = generate_resmoke.ConfigOptions(self.options.config)
        fuzzer_config.name = f"{self.options.suite}_multiversion"
        fuzzer_config.num_files = int(self.options.num_files)
        fuzzer_config.num_tasks = int(self.options.num_tasks)
        fuzzer_config.resmoke_args = f"{self.options.resmoke_args} "\
            f"--mixedBinVersions={version_config} --excludeWithAnyTags={EXCLUDE_TAGS}"\
            f" --suites={CONFIG_DIR}/{suite_file}"
        return fuzzer_config

    def _generate_fuzzer_tasks(self, config):
        suite_file = self.options.suite + ".yml"
        # Update the jstestfuzz yml suite with the proper multiversion configurations.
        source_config = generate_resmoke.read_yaml(TEST_SUITE_DIR, suite_file)
        config.update_yaml(source_config)
        updated_yml = generate_resmoke.generate_resmoke_suite_config(source_config, suite_file)
        file_dict = {f"{self.options.suite}.yml": updated_yml}
        dt = DisplayTaskDefinition(self.task)

        for version_config in config.version_configs:
            fuzzer_config = self._get_fuzzer_options(version_config, suite_file)
            gen_fuzzer.generate_evg_tasks(fuzzer_config, self.evg_config,
                                          task_name_suffix=version_config, display_task=dt)
        generate_resmoke.write_file_dict(CONFIG_DIR, file_dict)
        dt.execution_task(f"{fuzzer_config.name}_gen")
        self.evg_config.variant(self.options.variant).display_task(dt)
        return self.evg_config

    def generate_evg_tasks(self, burn_in_test=None, burn_in_idx=0):
        # pylint: disable=too-many-locals
        """
        Generate evergreen tasks for multiversion tests.

        The number of tasks generated equals
        (the number of version configs) * (the number of generated suites).

        :param burn_in_test: The test to be run as part of the burn in multiversion suite.
        """
        if is_suite_sharded(TEST_SUITE_DIR, self.options.suite):
            config = MultiversionConfig(update_suite_config_for_multiversion_sharded,
                                        SHARDED_MIXED_VERSION_CONFIGS)
        else:
            config = MultiversionConfig(update_suite_config_for_multiversion_replset,
                                        REPL_MIXED_VERSION_CONFIGS)

        if self.options.is_jstestfuzz:
            return self._generate_fuzzer_tasks(config)

        # Divide tests into suites based on run-time statistics for the last
        # LOOKBACK_DURATION_DAYS. Tests without enough run-time statistics will be placed
        # in the misc suite.
        gen_suites = generate_resmoke.GenerateSubSuites(self.evg_api, self.options)
        end_date = datetime.datetime.utcnow().replace(microsecond=0)
        start_date = end_date - datetime.timedelta(days=generate_resmoke.LOOKBACK_DURATION_DAYS)
        suites = gen_suites.calculate_suites(start_date, end_date)
        # Update the base suite names to the multiversion task names.
        for suite in suites:
            suite.source_name = self.task
        # Render the given suites into yml files that can be used by resmoke.py.
        config_file_dict = generate_resmoke.render_suite_files(
            suites, self.options.suite, gen_suites.test_list, TEST_SUITE_DIR, config.update_yaml)
        # Update the base misc suite name to the multiversion name.
        base_misc_file = f"{self.options.suite}_misc.yml"
        misc_suite_name = f"{self.task}_misc.yml"
        misc_suite = os.path.join(CONFIG_DIR, misc_suite_name)
        config_file_dict[misc_suite_name] = config_file_dict.pop(base_misc_file)
        generate_resmoke.write_file_dict(CONFIG_DIR, config_file_dict)

        if burn_in_test is not None:
            # Generate the subtasks to run burn_in_test against the appropriate mixed version
            # configurations. The display task is defined later as part of generating the burn
            # in tests.
            self._generate_burn_in_execution_tasks(config, suites, burn_in_test, burn_in_idx)
            return self.evg_config

        for version_config in config.version_configs:
            idx = 0
            for suite in suites:
                # Generate the newly divided test suites
                source_suite = os.path.join(CONFIG_DIR, suite.name + ".yml")
                self._generate_sub_task(version_config, self.task, idx, source_suite, len(suites))
                idx += 1

            # Also generate the misc task.
            misc_suite_name = "{0}_misc".format(self.options.suite)
            self._generate_sub_task(version_config, self.task, idx, misc_suite, 1)
            idx += 1
        self.create_display_task(self.task, self.task_specs, self.task_names)
        return self.evg_config

    def run(self):
        """Generate and run multiversion suites that run within a specified target execution time."""
        if not generate_resmoke.should_tasks_be_generated(self.evg_api, self.options.task_id):
            LOGGER.info("Not generating configuration due to previous successful generation.")
            return

        self.generate_evg_tasks()
        self._write_evergreen_config_to_file(self.task)


@click.group()
def main():
    """Serve as an entry point for the 'run' and 'generate-exclude-files' commands."""
    pass


@main.command("run")
@click.option("--expansion-file", type=str, required=True,
              help="Location of expansions file generated by evergreen.")
@click.option("--evergreen-config", type=str, default=CONFIG_FILE,
              help="Location of evergreen configuration file.")
def run_generate_tasks(expansion_file, evergreen_config=None):
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
    prepare_directory_for_suite(CONFIG_DIR)
    evg_config = Configuration()
    config_options = generate_resmoke.ConfigOptions.from_file(
        expansion_file, REQUIRED_CONFIG_KEYS, DEFAULT_CONFIG_VALUES, CONFIG_FORMAT_FN)
    config_generator = EvergreenConfigGenerator(evg_api, evg_config, config_options)
    config_generator.run()


@main.command("generate-exclude-files")
@click.option("--suite", type=str, required=True,
              help="The multiversion suite to generate the exclude_files yaml for.")
@click.option("--task-path-suffix", type=str, required=True,
              help="The directory in which multiversion binaries are stored.")
@click.option("--is-generated-suite", type=bool, required=True,
              help="Indicates whether the suite yaml to update is generated or static.")
def generate_exclude_yaml(suite, task_path_suffix, is_generated_suite):
    """
    Update the given multiversion suite configuration yaml to exclude tests.

    Compares the BACKPORTS_REQUIRED_FILE on the current branch with the same file on the
    last-stable branch to determine which tests should be blacklisted.
    """

    enable_logging()

    suite_name = generate_resmoke.remove_gen_suffix(suite)

    # Get the backports_required_for_multiversion_tests.yml on the current version branch.
    backports_required_latest = generate_resmoke.read_yaml(ETC_DIR, BACKPORTS_REQUIRED_FILE)
    if suite_name not in backports_required_latest:
        LOGGER.info(f"Generating exclude files not supported for '{suite_name}''.")
        return

    latest_suite_yaml = backports_required_latest[suite_name]

    if not latest_suite_yaml:
        LOGGER.info(f"No tests need to be excluded from suite '{suite_name}'.")
        return

    # Get the state of the backports_required_for_multiversion_tests.yml file for the last-stable
    # binary we are running tests against. We do this by using the commit hash from the last-stable
    # mongo shell executable.
    last_stable_commit_hash = get_backports_required_last_stable_hash(task_path_suffix)

    # Get the yaml contents under the 'suite_name' key from the last-stable commit.
    last_stable_suite_yaml = get_last_stable_yaml(last_stable_commit_hash, suite_name)
    if last_stable_suite_yaml is None:
        files_to_exclude = set(elem["test_file"] for elem in latest_suite_yaml)
    else:
        files_to_exclude = set(
            elem["test_file"] for elem in latest_suite_yaml if elem not in last_stable_suite_yaml)

    if not files_to_exclude:
        LOGGER.info(f"No tests need to be excluded from suite '{suite_name}'.")
        return

    suite_yaml_dict = {}

    if not is_generated_suite:
        # Populate the config values to get the resmoke config directory.
        buildscripts.resmokelib.parser.set_options()
        suites_dir = os.path.join(_config.CONFIG_DIR, "suites")

        # Update the static suite config with the excluded files and write to disk.
        file_name = f"{suite_name}.yml"
        suite_config = generate_resmoke.read_yaml(suites_dir, file_name)
        suite_yaml_dict[file_name] = generate_resmoke.generate_resmoke_suite_config(
            suite_config, file_name, excludes=list(files_to_exclude))
    else:
        # We expect the generated suites to already have been generated in the generated config
        # directory.
        for file_name in os.listdir(CONFIG_DIR):
            suites_dir = CONFIG_DIR
            # Update the 'exclude_files' for each of the appropriate generated suites.
            if suite_name in file_name and file_name.endswith('.yml'):
                suite_config = generate_resmoke.read_yaml(CONFIG_DIR, file_name)
                selected_files = suite_config["selector"]["roots"]
                # Only exclude the files that we want to exclude in the first place and have been
                # selected to run as part of the generated suite yml.
                intersection = [test for test in selected_files if test in files_to_exclude]
                if not intersection:
                    continue
                suite_yaml_dict[file_name] = generate_resmoke.generate_resmoke_suite_config(
                    suite_config, file_name, excludes=list(intersection))
    generate_resmoke.write_file_dict(suites_dir, suite_yaml_dict)


if __name__ == '__main__':
    main()  # pylint: disable=no-value-for-parameter
