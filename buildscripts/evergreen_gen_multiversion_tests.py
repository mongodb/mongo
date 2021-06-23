#!/usr/bin/env python3
"""Generate multiversion tests to run in evergreen in parallel."""

from datetime import datetime, timedelta
import os
import re
import tempfile
from typing import Optional, List
from collections import defaultdict
from sys import platform

from subprocess import check_output

import inject
import requests
import click
import structlog
from pydantic import BaseModel

from shrub.v2 import ExistingTask
from evergreen.api import RetryingEvergreenApi, EvergreenApi

from buildscripts.resmokelib.multiversionconstants import (
    LAST_LTS_MONGO_BINARY, LAST_CONTINUOUS_MONGO_BINARY, REQUIRES_FCV_TAG)
from buildscripts.task_generation.evg_config_builder import EvgConfigBuilder
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.generated_config import GeneratedConfiguration
from buildscripts.task_generation.multiversion_util import MultiversionUtilService
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyConfig
from buildscripts.task_generation.suite_split import SuiteSplitConfig, SuiteSplitParameters
from buildscripts.task_generation.suite_split_strategies import SplitStrategy, FallbackStrategy, \
    greedy_division, round_robin_fallback
from buildscripts.task_generation.task_types.fuzzer_tasks import FuzzerGenTaskParams
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions
from buildscripts.task_generation.task_types.multiversion_tasks import MultiversionGenTaskParams
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.fileops import read_yaml_file
import buildscripts.evergreen_generate_resmoke_tasks as generate_resmoke
import buildscripts.evergreen_gen_fuzzer_tests as gen_fuzzer
import buildscripts.ciconfig.tags as _tags

# pylint: disable=len-as-condition
from buildscripts.util.taskname import remove_gen_suffix

LOGGER = structlog.getLogger(__name__)

DEFAULT_CONFIG_DIR = "generated_resmoke_config"
CONFIG_DIR = DEFAULT_CONFIG_DIR
DEFAULT_TEST_SUITE_DIR = os.path.join("buildscripts", "resmokeconfig", "suites")
LOOKBACK_DURATION_DAYS = 14
CONFIG_FILE = generate_resmoke.EVG_CONFIG_FILE
REPL_MIXED_VERSION_CONFIGS = ["new-old-new", "new-new-old", "old-new-new"]
SHARDED_MIXED_VERSION_CONFIGS = ["new-old-old-new"]

BURN_IN_TASK = "burn_in_tests_multiversion"
MULTIVERSION_CONFIG_KEY = "use_in_multiversion"
PASSTHROUGH_TAG = "multiversion_passthrough"
BACKPORT_REQUIRED_TAG = "backport_required_multiversion"
EXCLUDE_TAGS = f"{REQUIRES_FCV_TAG},multiversion_incompatible,{BACKPORT_REQUIRED_TAG}"
EXCLUDE_TAGS_FILE = "multiversion_exclude_tags.yml"
GEN_PARENT_TASK = "generator_tasks"
ASAN_SIGNATURE = "detect_leaks=1"

# The directory in which BACKPORTS_REQUIRED_FILE resides.
ETC_DIR = "etc"
BACKPORTS_REQUIRED_FILE = "backports_required_for_multiversion_tests.yml"
BACKPORTS_REQUIRED_BASE_URL = "https://raw.githubusercontent.com/mongodb/mongo"


class EvgExpansions(BaseModel):
    """Evergreen expansions file contents."""

    project: str
    target_resmoke_time: int = 60
    max_sub_suites: int = 5
    max_tests_per_suite: int = 100
    san_options: Optional[str]
    task_name: str
    suite: Optional[str]
    num_files: Optional[int]
    num_tasks: Optional[int]
    resmoke_args: Optional[str]
    npm_command: Optional[str]
    jstestfuzz_vars: Optional[str]
    build_variant: str
    continue_on_failure: Optional[bool]
    resmoke_jobs_max: Optional[int]
    should_shuffle: Optional[bool]
    timeout_secs: Optional[int]
    require_multiversion: Optional[bool]
    use_large_distro: Optional[bool]
    large_distro_name: Optional[str]
    revision: str
    build_id: str
    create_misc_suite: bool = True
    is_patch: bool = False
    is_jstestfuzz: bool = False

    @property
    def task(self) -> str:
        """Get the name of the task."""
        return remove_gen_suffix(self.task_name)

    @classmethod
    def from_yaml_file(cls, path: str) -> "EvgExpansions":
        """Read the evergreen expansions from the given file."""
        return cls(**read_yaml_file(path))

    def config_location(self) -> str:
        """Get the location to store the configuration."""
        return f"{self.build_variant}/{self.revision}/generate_tasks/{self.task}_gen-{self.build_id}.tgz"

    def is_asan_build(self) -> bool:
        """Determine if this task is an ASAN build."""
        san_options = self.san_options
        if san_options:
            return ASAN_SIGNATURE in san_options
        return False

    def get_generation_options(self) -> GenTaskOptions:
        """Get options for how tasks should be generated."""
        return GenTaskOptions(
            create_misc_suite=self.create_misc_suite,
            is_patch=self.is_patch,
            generated_config_dir=DEFAULT_CONFIG_DIR,
            use_default_timeouts=False,
        )

    def get_fuzzer_params(self, version_config: str, is_sharded: bool) -> FuzzerGenTaskParams:
        """
        Get parameters to generate fuzzer tasks.

        :param version_config: Version configuration to generate for.
        :param is_sharded: If configuration is for sharded tests.
        :return: Parameters to generate fuzzer tasks.
        """
        name = f"{self.suite}_multiversion_{version_config}"
        add_resmoke_args = get_multiversion_resmoke_args(is_sharded)
        resmoke_args = f"{self.resmoke_args or ''} --mixedBinVersions={version_config} {add_resmoke_args}"

        return FuzzerGenTaskParams(
            num_files=self.num_files,
            num_tasks=self.num_tasks,
            resmoke_args=resmoke_args,
            npm_command=self.npm_command,
            jstestfuzz_vars=self.jstestfuzz_vars,
            task_name=name,
            variant=self.build_variant,
            continue_on_failure=self.continue_on_failure,
            resmoke_jobs_max=self.resmoke_jobs_max,
            should_shuffle=self.should_shuffle,
            timeout_secs=self.timeout_secs,
            require_multiversion=self.require_multiversion,
            suite=self.suite or self.task,
            use_large_distro=self.use_large_distro,
            large_distro_name=self.large_distro_name,
            config_location=self.config_location(),
        )

    def get_split_params(self) -> SuiteSplitParameters:
        """Get the parameters specified to split suites."""
        return SuiteSplitParameters(
            task_name=self.task_name,
            suite_name=self.suite or self.task,
            filename=self.suite or self.task,
            test_file_filter=None,
            build_variant=self.build_variant,
            is_asan=self.is_asan_build(),
        )

    def get_split_config(self, start_date: datetime, end_date: datetime) -> SuiteSplitConfig:
        """
        Get the configuration specifed to split suites.

        :param start_date: Start date for historic results query.
        :param end_date: End date for historic results query.
        :return: Configuration to use for splitting suites.
        """
        return SuiteSplitConfig(
            evg_project=self.project,
            target_resmoke_time=self.target_resmoke_time,
            max_sub_suites=self.max_sub_suites,
            max_tests_per_suite=self.max_tests_per_suite,
            start_date=start_date,
            end_date=end_date,
        )

    def get_generation_params(self, is_sharded: bool) -> MultiversionGenTaskParams:
        """
        Get the parameters to use to generating multiversion tasks.

        :param is_sharded: True if a sharded sutie is being generated.
        :return: Parameters to use for generating multiversion tasks.
        """
        version_config_list = get_version_configs(is_sharded)
        return MultiversionGenTaskParams(
            mixed_version_configs=version_config_list,
            is_sharded=is_sharded,
            resmoke_args=self.resmoke_args,
            parent_task_name=self.task,
            origin_suite=self.suite or self.task,
            use_large_distro=self.use_large_distro,
            large_distro_name=self.large_distro_name,
            config_location=self.config_location(),
        )


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


def get_backports_required_hash_for_shell_version(mongo_shell_path=None):
    """Parse the last-lts shell binary to get the commit hash."""
    if platform.startswith("win"):
        shell_version = check_output([mongo_shell_path + ".exe", "--version"]).decode('utf-8')
    else:
        shell_version = check_output([mongo_shell_path, "--version"]).decode('utf-8')
    for line in shell_version.splitlines():
        if "gitVersion" in line:
            version_line = line.split(':')[1]
            # We identify the commit hash as the string enclosed by double quotation marks.
            result = re.search(r'"(.*?)"', version_line)
            if result:
                commit_hash = result.group().strip('"')
                if not commit_hash.isalnum():
                    raise ValueError(f"Error parsing commit hash. Expected an "
                                     f"alpha-numeric string but got: {commit_hash}")
                return commit_hash
            else:
                break
    raise ValueError("Could not find a valid commit hash from the last-lts mongo binary.")


def get_last_lts_yaml(commit_hash):
    """Download BACKPORTS_REQUIRED_FILE from the last LTS commit and return the yaml."""
    LOGGER.info(f"Downloading file from commit hash of last-lts branch {commit_hash}")
    response = requests.get(
        f'{BACKPORTS_REQUIRED_BASE_URL}/{commit_hash}/{ETC_DIR}/{BACKPORTS_REQUIRED_FILE}')
    # If the response was successful, no exception will be raised.
    response.raise_for_status()

    last_lts_file = f"{commit_hash}_{BACKPORTS_REQUIRED_FILE}"
    temp_dir = tempfile.mkdtemp()
    with open(os.path.join(temp_dir, last_lts_file), "w") as fileh:
        fileh.write(response.text)

    backports_required_last_lts = read_yaml_file(os.path.join(temp_dir, last_lts_file))
    return backports_required_last_lts


class MultiVersionGenerateOrchestrator:
    """An orchestrator for generating multiversion tasks."""

    @inject.autoparams()
    def __init__(self, evg_api: EvergreenApi, multiversion_util: MultiversionUtilService,
                 gen_task_options: GenTaskOptions) -> None:
        """
        Initialize the orchestrator.

        :param evg_api: Evergreen API client.
        :param multiversion_util: Multiverison utilities service.
        :param gen_task_options: Options to use for generating tasks.
        """
        self.evg_api = evg_api
        self.multiversion_util = multiversion_util
        self.gen_task_options = gen_task_options

    def generate_fuzzer(self, evg_expansions: EvgExpansions) -> GeneratedConfiguration:
        """
        Generate configuration for the fuzzer task specified by the expansions.

        :param evg_expansions: Evergreen expansions describing what to generate.
        :return: Configuration to generate the specified task.
        """
        suite = evg_expansions.suite
        is_sharded = self.multiversion_util.is_suite_sharded(suite)
        version_config_list = get_version_configs(is_sharded)

        builder = EvgConfigBuilder()  # pylint: disable=no-value-for-parameter

        fuzzer_task_set = set()
        for version_config in version_config_list:
            fuzzer_params = evg_expansions.get_fuzzer_params(version_config, is_sharded)
            fuzzer_task = builder.generate_fuzzer(fuzzer_params)
            fuzzer_task_set = fuzzer_task_set.union(fuzzer_task.sub_tasks)

        existing_tasks = {ExistingTask(task) for task in fuzzer_task_set}
        existing_tasks.add({ExistingTask(f"{suite}_multiversion_gen")})
        builder.add_display_task(evg_expansions.task, existing_tasks, evg_expansions.build_variant)
        return builder.build(f"{evg_expansions.task}.json")

    def generate_resmoke_suite(self, evg_expansions: EvgExpansions) -> GeneratedConfiguration:
        """
        Generate configuration for the resmoke task specified by the expansions.

        :param evg_expansions: Evergreen expansions describing what to generate.
        :return: Configuration to generate the specified task.
        """
        suite = evg_expansions.suite or evg_expansions.task
        is_sharded = self.multiversion_util.is_suite_sharded(suite)

        split_params = evg_expansions.get_split_params()
        gen_params = evg_expansions.get_generation_params(is_sharded)

        builder = EvgConfigBuilder()  # pylint: disable=no-value-for-parameter
        builder.add_multiversion_suite(split_params, gen_params)
        builder.add_display_task(GEN_PARENT_TASK, {f"{split_params.task_name}"},
                                 evg_expansions.build_variant)
        return builder.build(f"{evg_expansions.task}.json")

    def generate(self, evg_expansions: EvgExpansions) -> None:
        """
        Generate configuration for the specified task and save it to disk.

        :param evg_expansions: Evergreen expansions describing what to generate.
        """
        if evg_expansions.is_jstestfuzz:
            generated_config = self.generate_fuzzer(evg_expansions)
        else:
            generated_config = self.generate_resmoke_suite(evg_expansions)
        generated_config.write_all_to_dir(DEFAULT_CONFIG_DIR)


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
    enable_logging(False)

    end_date = datetime.utcnow().replace(microsecond=0)
    start_date = end_date - timedelta(days=LOOKBACK_DURATION_DAYS)

    evg_expansions = EvgExpansions.from_yaml_file(expansion_file)

    def dependencies(binder: inject.Binder) -> None:
        binder.bind(SuiteSplitConfig, evg_expansions.get_split_config(start_date, end_date))
        binder.bind(SplitStrategy, greedy_division)
        binder.bind(FallbackStrategy, round_robin_fallback)
        binder.bind(GenTaskOptions, evg_expansions.get_generation_options())
        binder.bind(EvergreenApi, RetryingEvergreenApi.get_api(config_file=evergreen_config))
        binder.bind(GenerationConfiguration,
                    GenerationConfiguration.from_yaml_file(gen_fuzzer.GENERATE_CONFIG_FILE))
        binder.bind(ResmokeProxyConfig,
                    ResmokeProxyConfig(resmoke_suite_dir=DEFAULT_TEST_SUITE_DIR))

    inject.configure(dependencies)

    generate_orchestrator = MultiVersionGenerateOrchestrator()  # pylint: disable=no-value-for-parameter
    generate_orchestrator.generate(evg_expansions)


@main.command("generate-exclude-tags")
@click.option("--output", type=str, default=os.path.join(CONFIG_DIR, EXCLUDE_TAGS_FILE),
              show_default=True, help="Where to output the generated tags.")
def generate_exclude_yaml(output: str) -> None:
    # pylint: disable=too-many-locals
    """
    Create a tag file associating multiversion tests to tags for exclusion.

    Compares the BACKPORTS_REQUIRED_FILE on the current branch with the same file on the
    last-lts branch to determine which tests should be denylisted.
    """

    enable_logging(False)

    location, _ = os.path.split(os.path.abspath(output))
    if not os.path.isdir(location):
        LOGGER.info(f"Cannot write to {output}. Not generating tag file.")
        return

    backports_required_latest = read_yaml_file(os.path.join(ETC_DIR, BACKPORTS_REQUIRED_FILE))

    # Get the state of the backports_required_for_multiversion_tests.yml file for the last-lts
    # binary we are running tests against. We do this by using the commit hash from the last-lts
    # mongo shell executable.
    last_lts_commit_hash = get_backports_required_hash_for_shell_version(
        mongo_shell_path=LAST_LTS_MONGO_BINARY)

    # Get the yaml contents from the last-lts commit.
    backports_required_last_lts = get_last_lts_yaml(last_lts_commit_hash)

    def diff(list1, list2):
        return [elem for elem in (list1 or []) if elem not in (list2 or [])]

    suites_latest = backports_required_latest["last-lts"]["suites"] or {}
    # Check if the changed syntax for etc/backports_required_for_multiversion_tests.yml has been
    # backported.
    # This variable and all branches where it's not set can be deleted after backporting the change.
    change_backported = "last-lts" in backports_required_last_lts.keys()
    if change_backported:
        always_exclude = diff(backports_required_latest["last-lts"]["all"],
                              backports_required_last_lts["last-lts"]["all"])
        suites_last_lts: defaultdict = defaultdict(
            list, backports_required_last_lts["last-lts"]["suites"])
    else:
        always_exclude = diff(backports_required_latest["last-lts"]["all"],
                              backports_required_last_lts["all"])
        suites_last_lts: defaultdict = defaultdict(list, backports_required_last_lts["suites"])

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
