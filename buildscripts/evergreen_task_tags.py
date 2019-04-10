#!/usr/bin/env python3
"""Script to gather information about how tags are used in evergreen tasks."""

from __future__ import absolute_import
from __future__ import print_function

import argparse
import os
import sys

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.ciconfig import evergreen  # pylint: disable=wrong-import-position

DEFAULT_EVERGREEN_FILE = "etc/evergreen.yml"


def parse_command_line():
    """Parse command line options."""
    parser = argparse.ArgumentParser(description=main.__doc__)

    parser.add_argument("--list-tags", action="store_true", default=False,
                        help="List all tags used by tasks in evergreen yml.")
    parser.add_argument("--tasks-for-tag", type=str, default=None,
                        help="List all tasks that use the given tag.")
    parser.add_argument("--evergreen-file", type=str, default=DEFAULT_EVERGREEN_FILE,
                        help="Location of evergreen file.")

    options = parser.parse_args()

    return options


def get_all_task_tags(evg_config):
    """Get all task tags used in the evergreen configuration."""
    if evg_config.tasks:
        return sorted(set.union(*[task.tags for task in evg_config.tasks]))
    return set()


def list_all_tags(evg_config):
    """
    Print all task tags found in the evergreen configuration.

    :param evg_config: evergreen configuration.
    """
    all_tags = get_all_task_tags(evg_config)
    for tag in all_tags:
        print(tag)


def get_tasks_with_tag(evg_config, tag):
    """
    Get all tasks marked with the given tag in the evergreen configuration.

    :param evg_config: evergreen configuration.
    :param tag: tag to search for.
    :return: list of tasks marked with the given tag.
    """
    return sorted([task.name for task in evg_config.tasks if tag in task.tags])


def list_tasks_with_tag(evg_config, tag):
    """
    Print all tasks that are marked with the given tag.

    :param evg_config: evergreen configuration.
    :param tag: tag to search for.
    """
    task_list = get_tasks_with_tag(evg_config, tag)
    for task in task_list:
        print(task)


def main():
    """Gather information about how tags are used in evergreen tasks."""
    options = parse_command_line()

    evg_config = evergreen.parse_evergreen_file(options.evergreen_file)

    if options.list_tags:
        list_all_tags(evg_config)

    if options.tasks_for_tag:
        list_tasks_with_tag(evg_config, options.tasks_for_tag)


if __name__ == "__main__":
    main()
