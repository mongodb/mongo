#!/usr/bin/env python3
"""Generate burn in tests to run on certain build variants."""

import sys
import os

from collections import namedtuple
from typing import Any, Dict, Iterable
import click

from evergreen.api import RetryingEvergreenApi
from git import Repo
from shrub.config import Configuration
from shrub.variant import TaskSpec

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
import buildscripts.util.read_config as read_config
from buildscripts.ciconfig import evergreen
from buildscripts.ciconfig.evergreen import EvergreenProjectConfig
from buildscripts.burn_in_tests import create_generate_tasks_config, create_tests_by_task, \
    GenerateConfig, RepeatConfig, DEFAULT_REPO_LOCATIONS

# pylint: enable=wrong-import-position

CONFIG_DIRECTORY = "generated_burn_in_tags_config"
CONFIG_FILE = "burn_in_tags_gen.json"
EVERGREEN_FILE = "etc/evergreen.yml"
EVG_CONFIG_FILE = ".evergreen.yml"

ConfigOptions = namedtuple("ConfigOptions", [
    "build_variant",
    "run_build_variant",
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


def _get_config_options(expansions_file_data, build_variant, run_build_variant):
    """
    Get the configuration to use.

    :param expansions_file_data: Config data file to use.
    :param build_variant: The buildvariant the current patch should be compared against to figure
        out which tests have changed.
    :param run_build_variant: The buildvariant the generated task should be run on.
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
    repeat_tests_secs = int(expansions_file_data["repeat_tests_secs"])
    project = expansions_file_data["project"]

    return ConfigOptions(build_variant, run_build_variant, base_commit, max_revisions, branch,
                         check_evergreen, distro, repeat_tests_secs, repeat_tests_min,
                         repeat_tests_max, project)


def _create_evg_build_variant_map(expansions_file_data, evergreen_conf):
    """
    Generate relationship of base buildvariant to generated buildvariant.

    :param expansions_file_data: Config data file to use.
    :param evergreen_conf: Evergreen configuration.
    :return: Map of base buildvariants to their generated buildvariants.
    """
    burn_in_tags_gen_variant = expansions_file_data["build_variant"]
    burn_in_tags_gen_variant_config = evergreen_conf.get_variant(burn_in_tags_gen_variant)
    burn_in_tag_build_variants = burn_in_tags_gen_variant_config.expansions.get(
        "burn_in_tag_buildvariants")

    if burn_in_tag_build_variants:
        return {
            base_variant: f"{base_variant}-required"
            for base_variant in burn_in_tag_build_variants.split(" ")
        }

    return {}


def _generate_evg_build_variant(shrub_config, build_variant, run_build_variant,
                                burn_in_tags_gen_variant, evg_conf):
    """
    Generate buildvariants for a given shrub config.

    :param shrub_config: Shrub config object that the generated buildvariant will be built upon.
    :param build_variant: The base variant that the generated run_buildvariant will be based on.
    :param run_build_variant: The generated buildvariant.
    :param burn_in_tags_gen_variant: The buildvariant on which the burn_in_tags_gen task runs.
    """
    base_variant_config = evg_conf.get_variant(build_variant)

    new_variant_display_name = f"! {base_variant_config.display_name}"
    new_variant_run_on = base_variant_config.run_on[0]

    task_spec = TaskSpec("compile_without_package_TG")

    new_variant = shrub_config.variant(run_build_variant).expansion("burn_in_bypass",
                                                                    burn_in_tags_gen_variant)
    new_variant.display_name(new_variant_display_name)
    new_variant.run_on(new_variant_run_on)
    new_variant.task(task_spec)

    base_variant_expansions = base_variant_config.expansions
    new_variant.expansions(base_variant_expansions)

    modules = base_variant_config.modules
    new_variant.modules(modules)


# pylint: disable=too-many-arguments
def _generate_evg_tasks(evergreen_api, shrub_config, expansions_file_data, build_variant_map, repos,
                        evg_conf):
    """
    Generate burn in tests tasks for a given shrub config and group of buildvariants.

    :param evergreen_api: Evergreen.py object.
    :param shrub_config: Shrub config object that the build variants will be built upon.
    :param expansions_file_data: Config data file to use.
    :param build_variant_map: Map of base buildvariants to their generated buildvariant.
    :param repos: Git repositories.
    """
    for build_variant, run_build_variant in build_variant_map.items():
        config_options = _get_config_options(expansions_file_data, build_variant, run_build_variant)
        tests_by_task = create_tests_by_task(build_variant, repos, evg_conf)
        if tests_by_task:
            _generate_evg_build_variant(shrub_config, build_variant, run_build_variant,
                                        expansions_file_data["build_variant"], evg_conf)
            gen_config = GenerateConfig(build_variant, config_options.project, run_build_variant,
                                        config_options.distro).validate(evg_conf)
            repeat_config = RepeatConfig(repeat_tests_min=config_options.repeat_tests_min,
                                         repeat_tests_max=config_options.repeat_tests_max,
                                         repeat_tests_secs=config_options.repeat_tests_secs)

            create_generate_tasks_config(shrub_config, tests_by_task, gen_config, repeat_config,
                                         evergreen_api, include_gen_task=False)


def _write_to_file(shrub_config):
    """
    Save shrub config to file.

    :param shrub_config: Shrub config object.
    """
    if not os.path.exists(CONFIG_DIRECTORY):
        os.makedirs(CONFIG_DIRECTORY)

    with open(os.path.join(CONFIG_DIRECTORY, CONFIG_FILE), "w") as file_handle:
        file_handle.write(shrub_config.to_json())


def burn_in(expansions_file_data: Dict[str, Any], evg_conf: EvergreenProjectConfig,
            evergreen_api: RetryingEvergreenApi, repos: Iterable[Repo]):
    """Execute Main program."""

    shrub_config = Configuration()
    build_variant_map = _create_evg_build_variant_map(expansions_file_data, evg_conf)
    _generate_evg_tasks(evergreen_api, shrub_config, expansions_file_data, build_variant_map, repos,
                        evg_conf)
    _write_to_file(shrub_config)


@click.command()
@click.option("--expansion-file", "expansion_file", required=True,
              help="Location of expansions file generated by evergreen.")
def main(expansion_file: str):
    """
    Run new or changed tests in repeated mode to validate their stability.

    burn_in_tags detects jstests that are new or changed since the last git command and then
    runs those tests in a loop to validate their reliability.

    \f
    :param expansion_file: The expansion file containing the configuration params.
    """
    evg_api = RetryingEvergreenApi.get_api(config_file=EVG_CONFIG_FILE)
    repos = [Repo(x) for x in DEFAULT_REPO_LOCATIONS if os.path.isdir(x)]
    expansions_file_data = read_config.read_config_file(expansion_file)
    evg_conf = evergreen.parse_evergreen_file(EVERGREEN_FILE)

    burn_in(expansions_file_data, evg_conf, evg_api, repos)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
