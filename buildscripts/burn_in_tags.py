#!/usr/bin/env python3
"""Generate burn in tests to run on certain build variants."""

import argparse
import sys
import os

from collections import namedtuple

from shrub.config import Configuration
from shrub.variant import TaskSpec
from shrub.variant import Variant

from evergreen.api import RetryingEvergreenApi

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
import buildscripts.util.read_config as read_config
from buildscripts.ciconfig import evergreen
from buildscripts.burn_in_tests import create_generate_tasks_config, create_tests_by_task
# pylint: enable=wrong-import-position

CONFIG_DIRECTORY = "generated_burn_in_tags_config"
CONFIG_FILE = "burn_in_tags_gen.json"
EVERGREEN_FILE = "etc/evergreen.yml"
EVG_CONFIG_FILE = ".evergreen.yml"

ConfigOptions = namedtuple("ConfigOptions", [
    "buildvariant",
    "run_buildvariant",
    "base_commit",
    "max_revisions",
    "branch",
    "check_evergreen",
    "distro",
    "repeat_tests_secs",
    "repeat_tests_min",
    "repeat_tests_max",
    "project",
])


def _get_config_options(expansions_file_data, buildvariant, run_buildvariant):
    """
    Get the configuration to use.

    :param expansions_file_data: Config data file to use.
    :param buildvariant: The buildvariant the current patch should be compared against to figure
        out which tests have changed.
    :param run_buildvariant: The buildvariant the generated task should be run on.
    :return: ConfigOptions for the generated task to use.
    """
    base_commit = expansions_file_data["revision"]
    max_revisions = int(expansions_file_data["max_revisions"])
    branch = expansions_file_data["branch_name"]
    is_patch = expansions_file_data.get("is_patch", False)
    check_evergreen = is_patch != "true"
    distro = expansions_file_data["distro_id"]
    repeat_tests_min = int(expansions_file_data["repeat_tests_min"])
    repeat_tests_max = int(expansions_file_data["repeat_tests_max"])
    repeat_tests_secs = float(expansions_file_data["repeat_tests_secs"])
    project = expansions_file_data["project"]

    return ConfigOptions(buildvariant, run_buildvariant, base_commit, max_revisions, branch,
                         check_evergreen, distro, repeat_tests_secs, repeat_tests_min,
                         repeat_tests_max, project)


def _create_evg_buildvariant_map(expansions_file_data):
    """
    Generate relationship of base buildvariant to generated buildvariant.

    :param expansions_file_data: Config data file to use.
    :return: Map of base buildvariants to their generated buildvariants.
    """
    buildvariant_map = {}
    base_variants = expansions_file_data["base_variants"].split(
        " ") if expansions_file_data["base_variants"] else []
    for base_variant in base_variants:
        new_variant_name = f"{base_variant}-required"
        buildvariant_map[base_variant] = new_variant_name
    return buildvariant_map


def _generate_evg_buildvariant(shrub_config, buildvariant, run_buildvariant):
    """
    Generate buildvariants for a given shrub config.

    :param shrub_config: Shrub config object that the generated buildvariant will be built upon.
    :param buildvariant: The base variant that the generated run_buildvariant will be based on.
    :param run_buildvariant: The generated buildvariant.
    """
    evergreen_conf = evergreen.parse_evergreen_file(EVERGREEN_FILE)
    base_variant_config = evergreen_conf.get_variant(buildvariant)

    new_variant_display_name = f"! {base_variant_config.display_name}"
    new_variant_run_on = base_variant_config.run_on[0]

    task_spec = TaskSpec("compile_TG")
    task_spec.distro("rhel62-large")

    new_variant = shrub_config.variant(run_buildvariant)
    new_variant.display_name(new_variant_display_name)
    new_variant.run_on(new_variant_run_on)
    new_variant.task(task_spec)

    base_variant_expansions = base_variant_config.expansions
    new_variant.expansions(base_variant_expansions)

    modules = base_variant_config.modules
    new_variant.modules(modules)


def _generate_evg_tasks(evergreen_api, shrub_config, expansions_file_data, buildvariant_map):
    """
    Generate burn in tests tasks for a given shrub config and group of buildvariants.

    :param evergreen_api: Evergreen.py object.
    :param shrub_config: Shrub config object that the build variants will be built upon.
    :param expansions_file_data: Config data file to use.
    :param buildvariant_map: Map of base buildvariants to their generated buildvariant.
    """
    for buildvariant, run_buildvariant in buildvariant_map.items():
        config_options = _get_config_options(expansions_file_data, buildvariant, run_buildvariant)
        tests_by_task = create_tests_by_task(config_options, evergreen_api)
        if tests_by_task:
            _generate_evg_buildvariant(shrub_config, buildvariant, run_buildvariant)
            create_generate_tasks_config(evergreen_api, shrub_config, config_options, tests_by_task,
                                         False)


def _write_to_file(shrub_config):
    """
    Save shrub config to file.

    :param shrub_config: Shrub config object.
    """
    if not os.path.exists(CONFIG_DIRECTORY):
        os.makedirs(CONFIG_DIRECTORY)

    with open(os.path.join(CONFIG_DIRECTORY, CONFIG_FILE), "w") as file_handle:
        file_handle.write(shrub_config.to_json())


def main(evergreen_api):
    """Execute Main program."""

    parser = argparse.ArgumentParser(description=main.__doc__)
    parser.add_argument("--expansion-file", dest="expansion_file", type=str,
                        help="Location of expansions file generated by evergreen.")
    cmd_line_options = parser.parse_args()
    expansions_file_data = read_config.read_config_file(cmd_line_options.expansion_file)

    shrub_config = Configuration()
    buildvariant_map = _create_evg_buildvariant_map(expansions_file_data)
    _generate_evg_tasks(evergreen_api, shrub_config, expansions_file_data, buildvariant_map)
    _write_to_file(shrub_config)


if __name__ == '__main__':
    main(RetryingEvergreenApi.get_api(config_file=EVG_CONFIG_FILE))
