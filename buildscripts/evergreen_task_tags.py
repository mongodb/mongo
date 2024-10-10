#!/usr/bin/env python3
"""Script to gather information about how tags are used in evergreen tasks."""

from __future__ import absolute_import, print_function

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

    parser.add_argument(
        "--list-tags",
        action="store_true",
        default=False,
        help="List all tags used by tasks in evergreen yml.",
    )
    parser.add_argument("--list-tasks", type=str, help="List all tasks for the given buildvariant.")
    parser.add_argument(
        "--list-variants-and-tasks",
        action="store_true",
        help="List all tasks for every buildvariant.",
    )
    parser.add_argument(
        "-t",
        "--tasks-for-tag",
        type=str,
        default=None,
        action="append",
        help="List all tasks that use the given tag.",
    )
    parser.add_argument(
        "-x",
        "--remove-tasks-for-tag-filter",
        type=str,
        default=None,
        action="append",
        help="Remove tasks tagged with given tag.",
    )
    parser.add_argument(
        "--evergreen-file",
        type=str,
        default=DEFAULT_EVERGREEN_FILE,
        help="Location of evergreen file.",
    )

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


def get_all_tasks(evg_config, build_variant):
    """
    Get all tasks for the given build variant.

    :param evg_config: Evergreen configuration.
    :param build_variant: Build Variant to get tasks for.
    :return: List of task name belonging to given build variant.
    """
    bv = evg_config.get_variant(build_variant)
    return bv.task_names


def list_all_tasks(evg_config, build_variant):
    """
    Print all tasks for the given build variant.

    :param evg_config: Evergreen configuration.
    :param build_variant: Build Variant to get tasks for.
    """
    tasks = get_all_tasks(evg_config, build_variant)
    for task in tasks:
        print(task)


def list_all_variants_and_tasks(evg_config):
    """
    Print all tasks for every build variant.

    :param evg_config: Evergreen configuration.
    """
    for variant in evg_config.variant_names:
        tasks = get_all_tasks(evg_config, variant)
        for task in tasks:
            print("%s | %s" % (variant, task))


def is_task_tagged(task, tags, filters):
    """
    Determine if given task match tag query.

    :param task: Task to check.
    :param tags: List of tags that should belong to the task.
    :param filters: List of tags that should not belong to the task.
    :return: True if task matches the query.
    """
    if all(tag in task.tags for tag in tags):
        if not filters or not any(tag in task.tags for tag in filters):
            return True

    return False


def get_tasks_with_tag(evg_config, tags, filters):
    """
    Get all tasks marked with the given tag in the evergreen configuration.

    :param evg_config: evergreen configuration.
    :param tags: tag to search for.
    :param filters: lst of tags to filter out.
    :return: list of tasks marked with the given tag.
    """
    return sorted([task.name for task in evg_config.tasks if is_task_tagged(task, tags, filters)])


def list_tasks_with_tag(evg_config, tags, filters):
    """
    Print all tasks that are marked with the given tag.

    :param evg_config: evergreen configuration.
    :param tags: list of tags to search for.
    :param filters: list of tags to filter out.
    """
    task_list = get_tasks_with_tag(evg_config, tags, filters)
    for task in task_list:
        print(task)


def main():
    """Gather information about how tags are used in evergreen tasks."""
    options = parse_command_line()

    evg_config = evergreen.parse_evergreen_file(options.evergreen_file)

    if options.list_tags:
        list_all_tags(evg_config)

    if options.list_variants_and_tasks:
        list_all_variants_and_tasks(evg_config)

    if options.list_tasks:
        list_all_tasks(evg_config, options.list_tasks)

    if options.tasks_for_tag:
        list_tasks_with_tag(evg_config, options.tasks_for_tag, options.remove_tasks_for_tag_filter)


if __name__ == "__main__":
    main()
