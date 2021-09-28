"""Task generation for multiversion resmoke tasks."""
from typing import NamedTuple, Set, List, Optional

import inject
from shrub.v2 import Task, FunctionCall, TaskDependency

from buildscripts.resmokelib.multiversionconstants import REQUIRES_FCV_TAG
from buildscripts.task_generation.suite_split import GeneratedSuite
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions

BACKPORT_REQUIRED_TAG = "backport_required_multiversion"
EXCLUDE_TAGS = f"{REQUIRES_FCV_TAG},multiversion_incompatible,{BACKPORT_REQUIRED_TAG}"
EXCLUDE_TAGS_FILE = "multiversion_exclude_tags.yml"


class MultiversionGenTaskParams(NamedTuple):
    """
    Parameters for how multiversion tests should be generated.

    mixed_version_configs: List of version configuration to generate.
    is_sharded: Whether sharded tests are being generated.
    resmoke_args: Arguments to pass to resmoke.
    parent_task_name: Name of parent task containing all sub tasks.
    origin_suite: Resmoke suite generated tests are based off.
    """

    mixed_version_configs: List[str]
    is_sharded: bool
    resmoke_args: str
    parent_task_name: str
    origin_suite: str
    use_large_distro: bool
    large_distro_name: Optional[str]
    config_location: str
    name_prefix: Optional[str] = None
    test_list: Optional[str] = None
    create_misc_suite: bool = True
    add_to_display_task: bool = True

    def get_multiversion_resmoke_args(self) -> str:
        """Return resmoke args used to configure a cluster for multiversion testing."""
        if self.is_sharded:
            return "--numShards=2 --numReplSetNodes=2 "
        return "--numReplSetNodes=3 --linearChain=on "


class MultiversionGenTaskService:
    """A service for generating multiversion tests."""

    @inject.autoparams()
    def __init__(self, gen_task_options: GenTaskOptions) -> None:
        """
        Initialize the service.

        :param gen_task_options: Options for how tasks should be generated.
        """
        self.gen_task_options = gen_task_options

    def generate_tasks(self, suite: GeneratedSuite, params: MultiversionGenTaskParams) -> Set[Task]:
        """
        Generate multiversion tasks for the given suite.

        :param suite: Suite to generate multiversion tasks for.
        :param params: Parameters for how tasks should be generated.
        :return: Evergreen configuration to generate the specified tasks.
        """
        sub_tasks = set()
        for version_config in params.mixed_version_configs:
            for index, sub_suite in enumerate(suite.sub_suites):
                # Generate the newly divided test suites
                sub_suite_name = sub_suite.name(len(suite))
                sub_task_name = f"{sub_suite_name}_{version_config}_{suite.build_variant}"
                if params.name_prefix is not None:
                    sub_task_name = f"{params.name_prefix}:{sub_task_name}"

                sub_tasks.add(
                    self._generate_task(sub_task_name, version_config, params, suite, index))

            if params.create_misc_suite:
                # Also generate the misc task.
                misc_suite_name = f"{params.origin_suite}_misc"
                misc_task_name = f"{misc_suite_name}_{version_config}_{suite.build_variant}"
                sub_tasks.add(
                    self._generate_task(misc_task_name, version_config, params, suite, None))

        return sub_tasks

    # pylint: disable=too-many-arguments
    def _generate_task(self, sub_task_name: str, mixed_version_config: str,
                       params: MultiversionGenTaskParams, suite: GeneratedSuite,
                       index: Optional[int]) -> Task:
        """
        Generate a sub task to be run with the provided suite and  mixed version config.

        :param sub_task_name: Name of task being generated.
        :param mixed_version_config: Versions task is being generated for.
        :param params: Parameters for how tasks should be generated.
        :return: Shrub configuration for task specified.
        """
        suite_file = self.gen_task_options.suite_location(suite.sub_suite_config_file(index))

        run_tests_vars = {
            "resmoke_args": self._build_resmoke_args(suite_file, mixed_version_config, params),
            "task": params.parent_task_name,
            "gen_task_config_location": params.config_location,
            "require_multiversion": True,
        }

        commands = [
            FunctionCall("do setup"),
            # Fetch and download the proper mongod binaries before running multiversion tests.
            FunctionCall("configure evergreen api credentials"),
            FunctionCall("do multiversion setup"),
            FunctionCall("run generated tests", run_tests_vars),
        ]

        return Task(sub_task_name, commands, {TaskDependency("archive_dist_test_debug")})

    def _build_resmoke_args(self, suite_file: str, mixed_version_config: str,
                            params: MultiversionGenTaskParams) -> str:
        """
        Get the resmoke args needed to run the specified task.

        :param suite_file: Path to resmoke suite configuration to run.
        :param mixed_version_config: Versions task is being generated for.
        :param params: Parameters for how tasks should be generated.
        :return: Arguments to pass to resmoke to run the generated task.
        """
        tag_file_location = self.gen_task_options.generated_file_location(EXCLUDE_TAGS_FILE)

        return (
            f"{params.resmoke_args} "
            f" --suite={suite_file}.yml "
            f" --mixedBinVersions={mixed_version_config}"
            f" --excludeWithAnyTags={EXCLUDE_TAGS},{params.parent_task_name}_{BACKPORT_REQUIRED_TAG} "
            f" --tagFile={tag_file_location} "
            f" --originSuite={params.origin_suite} "
            f" {params.get_multiversion_resmoke_args()} "
            f" {params.test_list if params.test_list else ''} ")
