#!/usr/bin/env python3
"""Generate fuzzer tests to run in evergreen in parallel."""
import argparse
from collections import namedtuple
from typing import Set

from shrub.v2 import ShrubProject, FunctionCall, Task, TaskDependency, BuildVariant, ExistingTask

from buildscripts.util.fileops import write_file_to_dir
import buildscripts.util.read_config as read_config
import buildscripts.util.taskname as taskname

CONFIG_DIRECTORY = "generated_resmoke_config"

ConfigOptions = namedtuple("ConfigOptions", [
    "num_files",
    "num_tasks",
    "resmoke_args",
    "npm_command",
    "jstestfuzz_vars",
    "name",
    "variant",
    "continue_on_failure",
    "resmoke_jobs_max",
    "should_shuffle",
    "timeout_secs",
    "use_multiversion",
    "suite",
])


def _get_config_options(cmd_line_options, config_file):  # pylint: disable=too-many-locals
    """
    Get the configuration to use.

    Command line options override config files options.

    :param cmd_line_options: Command line options specified.
    :param config_file: config file to use.
    :return: ConfigOptions to use.
    """
    config_file_data = read_config.read_config_file(config_file)

    num_files = int(
        read_config.get_config_value("num_files", cmd_line_options, config_file_data,
                                     required=True))
    num_tasks = int(
        read_config.get_config_value("num_tasks", cmd_line_options, config_file_data,
                                     required=True))
    resmoke_args = read_config.get_config_value("resmoke_args", cmd_line_options, config_file_data,
                                                default="")
    npm_command = read_config.get_config_value("npm_command", cmd_line_options, config_file_data,
                                               default="jstestfuzz")
    jstestfuzz_vars = read_config.get_config_value("jstestfuzz_vars", cmd_line_options,
                                                   config_file_data, default="")
    name = read_config.get_config_value("name", cmd_line_options, config_file_data, required=True)
    variant = read_config.get_config_value("build_variant", cmd_line_options, config_file_data,
                                           required=True)
    continue_on_failure = read_config.get_config_value("continue_on_failure", cmd_line_options,
                                                       config_file_data, default="false")
    resmoke_jobs_max = read_config.get_config_value("resmoke_jobs_max", cmd_line_options,
                                                    config_file_data, default="0")
    should_shuffle = read_config.get_config_value("should_shuffle", cmd_line_options,
                                                  config_file_data, default="false")
    timeout_secs = read_config.get_config_value("timeout_secs", cmd_line_options, config_file_data,
                                                default="1800")
    use_multiversion = read_config.get_config_value("task_path_suffix", cmd_line_options,
                                                    config_file_data, default=False)

    suite = read_config.get_config_value("suite", cmd_line_options, config_file_data, required=True)

    return ConfigOptions(num_files, num_tasks, resmoke_args, npm_command, jstestfuzz_vars, name,
                         variant, continue_on_failure, resmoke_jobs_max, should_shuffle,
                         timeout_secs, use_multiversion, suite)


def build_fuzzer_sub_task(task_name: str, task_index: int, options: ConfigOptions) -> Task:
    """
    Build a shrub task to run the fuzzer.

    :param task_name: Parent name of task.
    :param task_index: Index of sub task being generated.
    :param options: Options to use for task.
    :return: Shrub task to run the fuzzer.
    """
    sub_task_name = taskname.name_generated_task(task_name, task_index, options.num_tasks,
                                                 options.variant)

    run_jstestfuzz_vars = {
        "jstestfuzz_vars":
            "--numGeneratedFiles {0} {1}".format(options.num_files, options.jstestfuzz_vars),
        "npm_command":
            options.npm_command,
    }
    suite_arg = f"--suites={options.suite}"
    run_tests_vars = {
        "continue_on_failure": options.continue_on_failure,
        "resmoke_args": f"{suite_arg} {options.resmoke_args}",
        "resmoke_jobs_max": options.resmoke_jobs_max,
        "should_shuffle": options.should_shuffle,
        "task_path_suffix": options.use_multiversion,
        "timeout_secs": options.timeout_secs,
        "task": options.name
    }  # yapf: disable

    commands = [
        FunctionCall("do setup"),
        FunctionCall("do multiversion setup") if options.use_multiversion else None,
        FunctionCall("setup jstestfuzz"),
        FunctionCall("run jstestfuzz", run_jstestfuzz_vars),
        FunctionCall("run generated tests", run_tests_vars)
    ]
    commands = [command for command in commands if command is not None]

    return Task(sub_task_name, commands, {TaskDependency("compile")})


def generate_fuzzer_sub_tasks(task_name: str, options: ConfigOptions) -> Set[Task]:
    """
    Generate evergreen tasks for fuzzers based on the options given.

    :param task_name: Parent name for tasks being generated.
    :param options: task options.
    :return: Set of shrub tasks.
    """
    sub_tasks = {
        build_fuzzer_sub_task(task_name, index, options)
        for index in range(options.num_tasks)
    }
    return sub_tasks


def create_fuzzer_task(options: ConfigOptions, build_variant: BuildVariant) -> None:
    """
    Generate an evergreen configuration for fuzzers and add it to the given build variant.

    :param options: task options.
    :param build_variant: Build variant to add tasks to.
    """
    task_name = options.name
    sub_tasks = generate_fuzzer_sub_tasks(task_name, options)

    build_variant.display_task(task_name, sub_tasks,
                               execution_existing_tasks={ExistingTask(f"{options.name}_gen")})


def main():
    """Generate fuzzer tests to run in evergreen."""
    parser = argparse.ArgumentParser(description=main.__doc__)

    parser.add_argument("--expansion-file", dest="expansion_file", type=str,
                        help="Location of expansions file generated by evergreen.")
    parser.add_argument("--num-files", dest="num_files", type=int,
                        help="Number of files to generate per task.")
    parser.add_argument("--num-tasks", dest="num_tasks", type=int,
                        help="Number of tasks to generate.")
    parser.add_argument("--resmoke-args", dest="resmoke_args", help="Arguments to pass to resmoke.")
    parser.add_argument("--npm-command", dest="npm_command", help="npm command to run for fuzzer.")
    parser.add_argument("--jstestfuzz-vars", dest="jstestfuzz_vars",
                        help="options to pass to jstestfuzz.")
    parser.add_argument("--name", dest="name", help="name of task to generate.")
    parser.add_argument("--variant", dest="build_variant", help="build variant to generate.")
    parser.add_argument("--use-multiversion", dest="task_path_suffix",
                        help="Task path suffix for multiversion generated tasks.")
    parser.add_argument("--continue-on-failure", dest="continue_on_failure",
                        help="continue_on_failure value for generated tasks.")
    parser.add_argument("--resmoke-jobs-max", dest="resmoke_jobs_max",
                        help="resmoke_jobs_max value for generated tasks.")
    parser.add_argument("--should-shuffle", dest="should_shuffle",
                        help="should_shuffle value for generated tasks.")
    parser.add_argument("--timeout-secs", dest="timeout_secs",
                        help="timeout_secs value for generated tasks.")
    parser.add_argument("--suite", dest="suite", help="Suite to run using resmoke.")

    options = parser.parse_args()

    config_options = _get_config_options(options, options.expansion_file)
    build_variant = BuildVariant(config_options.variant)
    create_fuzzer_task(config_options, build_variant)

    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    write_file_to_dir(CONFIG_DIRECTORY, f"{config_options.name}.json", shrub_project.json())


if __name__ == '__main__':
    main()
