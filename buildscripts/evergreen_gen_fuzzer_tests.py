#!/usr/bin/env python3
"""Generate fuzzer tests to run in evergreen in parallel."""

import argparse
import math
import os

from collections import namedtuple

from shrub.config import Configuration
from shrub.command import CommandDefinition
from shrub.task import TaskDependency
from shrub.variant import DisplayTaskDefinition
from shrub.variant import TaskSpec

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

    return ConfigOptions(num_files, num_tasks, resmoke_args, npm_command, jstestfuzz_vars, name,
                         variant, continue_on_failure, resmoke_jobs_max, should_shuffle,
                         timeout_secs, use_multiversion)


def _name_task(parent_name, task_index, total_tasks):
    """
    Create a zero-padded sub-task name.

    :param parent_name: Name of the parent task.
    :param task_index: Index of this sub-task.
    :param total_tasks: Total number of sub-tasks being generated.
    :return: Zero-padded name of sub-task.
    """
    index_width = int(math.ceil(math.log10(total_tasks)))
    return "{0}_{1}".format(parent_name, str(task_index).zfill(index_width))


def _generate_evg_tasks(options):
    """
    Generate an evergreen configuration for fuzzers based on the options given.

    :param options: task options.
    :return: An evergreen configuration.
    """
    evg_config = Configuration()

    task_names = []
    task_specs = []

    for task_index in range(options.num_tasks):
        name = taskname.name_generated_task(options.name, task_index, options.num_tasks,
                                            options.variant)
        task_names.append(name)
        task_specs.append(TaskSpec(name))
        task = evg_config.task(name)

        commands = [CommandDefinition().function("do setup")]
        if options.use_multiversion:
            commands.append(CommandDefinition().function("do multiversion setup"))

        commands.append(CommandDefinition().function("run jstestfuzz").vars({
            "jstestfuzz_vars":
                "--numGeneratedFiles {0} {1}".format(options.num_files, options.jstestfuzz_vars),
            "npm_command":
                options.npm_command
        }))
        run_tests_vars = {
            "continue_on_failure": options.continue_on_failure,
            "resmoke_args": options.resmoke_args,
            "resmoke_jobs_max": options.resmoke_jobs_max,
            "should_shuffle": options.should_shuffle,
            "task_path_suffix": options.use_multiversion,
            "timeout_secs": options.timeout_secs,
        }

        commands.append(CommandDefinition().function("run tests").vars(run_tests_vars))
        task.dependency(TaskDependency("compile")).commands(commands)

    dt = DisplayTaskDefinition(options.name).execution_tasks(task_names)\
        .execution_task("{0}_gen".format(options.name))
    evg_config.variant(options.variant).tasks(task_specs).display_task(dt)

    return evg_config


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

    options = parser.parse_args()

    config_options = _get_config_options(options, options.expansion_file)

    evg_config = _generate_evg_tasks(config_options)

    if not os.path.exists(CONFIG_DIRECTORY):
        os.makedirs(CONFIG_DIRECTORY)

    with open(os.path.join(CONFIG_DIRECTORY, config_options.name + ".json"), "w") as file_handle:
        file_handle.write(evg_config.to_json())


if __name__ == '__main__':
    main()
