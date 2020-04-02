#!/usr/bin/env python3
"""Command line utility for running newly added or modified jstests under the appropriate multiversion passthrough suites."""

import os
import sys
from typing import Dict

import click
from evergreen.api import EvergreenApi
from git import Repo
from shrub.v2 import BuildVariant, ExistingTask, ShrubProject
import structlog
from structlog.stdlib import LoggerFactory

import buildscripts.evergreen_gen_multiversion_tests as gen_multiversion
import buildscripts.evergreen_generate_resmoke_tasks as gen_resmoke
from buildscripts.burn_in_tests import GenerateConfig, DEFAULT_PROJECT, CONFIG_FILE, _configure_logging, RepeatConfig, \
    _get_evg_api, EVERGREEN_FILE, DEFAULT_REPO_LOCATIONS, _set_resmoke_cmd, create_tests_by_task, \
    run_tests
from buildscripts.ciconfig.evergreen import parse_evergreen_file
from buildscripts.patch_builds.task_generation import validate_task_generation_limit
from buildscripts.resmokelib.suitesconfig import get_named_suites_with_root_level_key
from buildscripts.util.fileops import write_file

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.getLogger(__name__)

MULTIVERSION_CONFIG_KEY = gen_multiversion.MULTIVERSION_CONFIG_KEY
MULTIVERSION_PASSTHROUGH_TAG = gen_multiversion.PASSTHROUGH_TAG
RANDOM_MULTIVERSION_REPLSETS_TAG = gen_multiversion.RANDOM_REPLSETS_TAG
BURN_IN_MULTIVERSION_TASK = gen_multiversion.BURN_IN_TASK
TASK_PATH_SUFFIX = "/data/multiversion"


def create_multiversion_generate_tasks_config(tests_by_task: Dict, evg_api: EvergreenApi,
                                              generate_config: GenerateConfig) -> BuildVariant:
    """
    Create the multiversion config for the Evergreen generate.tasks file.

    :param tests_by_task: Dictionary of tests to generate tasks for.
    :param evg_api: Evergreen API.
    :param generate_config: Configuration of what to generate.
    :return: Shrub configuration with added tasks.
    """
    build_variant = BuildVariant(generate_config.build_variant)
    tasks = set()
    if tests_by_task:
        # Get the multiversion suites that will run in as part of burn_in_multiversion.
        multiversion_suites = get_named_suites_with_root_level_key(MULTIVERSION_CONFIG_KEY)
        for suite in multiversion_suites:
            idx = 0
            if suite["origin"] not in tests_by_task.keys():
                # Only generate burn in multiversion tasks for suites that would run the detected
                # changed tests.
                continue
            LOGGER.debug("Generating multiversion suite", suite=suite["multiversion_name"])

            # We hardcode the number of fallback sub suites and the target resmoke time here
            # since burn_in_tests cares about individual tests and not entire suites. The config
            # options here are purely used to generate the proper multiversion suites to run
            # tests against.
            config_options = {
                "suite": suite["origin"],
                "fallback_num_sub_suites": 1,
                "project": generate_config.project,
                "build_variant": generate_config.build_variant,
                "task_id": generate_config.task_id,
                "task_name": suite["multiversion_name"],
                "target_resmoke_time": 60,
            }
            config_options.update(gen_resmoke.DEFAULT_CONFIG_VALUES)

            config_generator = gen_multiversion.EvergreenMultiversionConfigGenerator(
                evg_api, gen_resmoke.ConfigOptions(config_options))
            test_list = tests_by_task[suite["origin"]]["tests"]
            for test in test_list:
                # Exclude files that should be blacklisted from multiversion testing.
                files_to_exclude = gen_multiversion.get_exclude_files(suite["multiversion_name"],
                                                                      TASK_PATH_SUFFIX)
                LOGGER.debug("Files to exclude", files_to_exclude=files_to_exclude, test=test,
                             suite=suite["multiversion_name"])
                if test not in files_to_exclude:
                    # Generate the multiversion tasks for each test.
                    sub_tasks = config_generator.get_burn_in_tasks(test, idx)
                    tasks = tasks.union(sub_tasks)
                    idx += 1

    existing_tasks = {ExistingTask(f"{BURN_IN_MULTIVERSION_TASK}_gen")}
    build_variant.display_task(BURN_IN_MULTIVERSION_TASK, tasks,
                               execution_existing_tasks=existing_tasks)
    return build_variant


@click.command()
@click.option("--no-exec", "no_exec", default=False, is_flag=True,
              help="Do not execute the found tests.")
@click.option("--generate-tasks-file", "generate_tasks_file", default=None, metavar='FILE',
              help="Run in 'generate.tasks' mode. Store task config to given file.")
@click.option("--build-variant", "build_variant", default=None, metavar='BUILD_VARIANT',
              help="Tasks to run will be selected from this build variant.")
@click.option("--run-build-variant", "run_build_variant", default=None, metavar='BUILD_VARIANT',
              help="Burn in tasks will be generated on this build variant.")
@click.option("--distro", "distro", default=None, metavar='DISTRO',
              help="The distro the tasks will execute on.")
@click.option("--project", "project", default=DEFAULT_PROJECT, metavar='PROJECT',
              help="The evergreen project the tasks will execute on.")
@click.option("--evg-api-config", "evg_api_config", default=CONFIG_FILE, metavar="FILE",
              help="Configuration file with connection info for Evergreen API.")
@click.option("--verbose", "verbose", default=False, is_flag=True, help="Enable extra logging.")
@click.option("--task_id", "task_id", default=None, metavar='TASK_ID',
              help="The evergreen task id.")
@click.argument("resmoke_args", nargs=-1, type=click.UNPROCESSED)
# pylint: disable=too-many-arguments,too-many-locals
def main(build_variant, run_build_variant, distro, project, generate_tasks_file, no_exec,
         resmoke_args, evg_api_config, verbose, task_id):
    """
    Run new or changed tests in repeated mode to validate their stability.

    Running burn_in_tests_multiversion will run new or changed tests against the appropriate generated multiversion
    suites. The purpose of these tests are to signal bugs in the generated multiversion suites as these tasks are
    excluded from the required build variants and are only run in certain daily build variants. As such, we only expect
    the burn-in multiversion tests to be run once for each binary version configuration, and `--repeat-*` arguments
    should be None when executing this script.

    There are two modes that burn_in_tests_multiversion can run in:

    (1) Normal mode: by default burn_in_tests will attempt to run all detected tests the
    configured number of times. This is useful if you have a test or tests you would like to
    check before submitting a patch to evergreen.

    (2) By specifying the `--generate-tasks-file`, burn_in_tests will run generate a configuration
    file that can then be sent to the Evergreen 'generate.tasks' command to create evergreen tasks
    to do all the test executions. This is the mode used to run tests in patch builds.

    NOTE: There is currently a limit of the number of tasks burn_in_tests will attempt to generate
    in evergreen. The limit is 1000. If you change enough tests that more than 1000 tasks would
    be generated, burn_in_test will fail. This is to avoid generating more tasks than evergreen
    can handle.
    \f

    :param build_variant: Build variant to query tasks from.
    :param run_build_variant:Build variant to actually run against.
    :param distro: Distro to run tests on.
    :param project: Project to run tests on.
    :param generate_tasks_file: Create a generate tasks configuration in this file.
    :param no_exec: Just perform test discover, do not execute the tests.
    :param resmoke_args: Arguments to pass through to resmoke.
    :param evg_api_config: Location of configuration file to connect to evergreen.
    :param verbose: Log extra debug information.
    """
    _configure_logging(verbose)

    evg_conf = parse_evergreen_file(EVERGREEN_FILE)
    repeat_config = RepeatConfig()  # yapf: disable
    generate_config = GenerateConfig(build_variant=build_variant,
                                     run_build_variant=run_build_variant,
                                     distro=distro,
                                     project=project,
                                     task_id=task_id)  # yapf: disable
    if generate_tasks_file:
        generate_config.validate(evg_conf)

    evg_api = _get_evg_api(evg_api_config, False)

    repos = [Repo(x) for x in DEFAULT_REPO_LOCATIONS if os.path.isdir(x)]

    resmoke_cmd = _set_resmoke_cmd(repeat_config, list(resmoke_args))

    tests_by_task = create_tests_by_task(generate_config.build_variant, repos, evg_conf)
    LOGGER.debug("tests and tasks found", tests_by_task=tests_by_task)

    if generate_tasks_file:
        multiversion_tasks = evg_conf.get_task_names_by_tag(MULTIVERSION_PASSTHROUGH_TAG)
        LOGGER.debug("Multiversion tasks by tag", tasks=multiversion_tasks,
                     tag=MULTIVERSION_PASSTHROUGH_TAG)
        # We expect the number of suites with MULTIVERSION_PASSTHROUGH_TAG to be the same as in
        # multiversion_suites. Multiversion passthrough suites must include
        # MULTIVERSION_CONFIG_KEY as a root level key and must be set to true.
        multiversion_suites = get_named_suites_with_root_level_key(MULTIVERSION_CONFIG_KEY)
        assert len(multiversion_tasks) == len(multiversion_suites)

        build_variant = create_multiversion_generate_tasks_config(tests_by_task, evg_api,
                                                                  generate_config)
        shrub_project = ShrubProject.empty()
        shrub_project.add_build_variant(build_variant)

        if not validate_task_generation_limit(shrub_project):
            sys.exit(1)

        write_file(generate_tasks_file, shrub_project.json())
    elif not no_exec:
        run_tests(tests_by_task, resmoke_cmd)
    else:
        LOGGER.info("Not running tests due to 'no_exec' option.")


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
