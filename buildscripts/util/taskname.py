"""Functions for working with resmoke task names."""

import math
import re

GEN_SUFFIX = "_gen"


def name_generated_task(parent_name, task_index=None, total_tasks=0, variant=None):
    """
    Create a zero-padded sub-task name.

    :param parent_name: Name of the parent task.
    :param task_index: Index of this sub-task.
    :param total_tasks: Total number of sub-tasks being generated.
    :param variant: Build variant to run task in.
    :return: Zero-padded name of sub-task.
    """
    suffix = ""
    if variant:
        suffix = f"_{variant}"

    if task_index is None:
        return f"{parent_name}_misc{suffix}"
    else:
        index_width = int(math.ceil(math.log10(total_tasks)))  # pylint: disable=c-extension-no-member
        return f"{parent_name}_{str(task_index).zfill(index_width)}{suffix}"


def remove_gen_suffix(task_name: str) -> str:
    """
    Remove '_gen' suffix from task_name.

    :param task_name: Original task name.
    :return: Task name with '_gen' removed, if it exists.
    """
    if task_name.endswith(GEN_SUFFIX):
        return task_name[:-4]
    return task_name


def determine_task_base_name(task_name: str, build_variant: str) -> str:
    """
    Determine the base name of a task.

    For generated tasks the base name will have the build variant and sub-task index
    stripped off. For other tasks, it is the unmodified task_name.

    :param task_name: Name of task to get base name of.
    :param build_variant: Build variant that may be included in task name.
    :return: Base name of given task.
    """
    match = re.match(f"(.*)_([0-9]+|misc)_{build_variant}", task_name)
    if match:
        return match.group(1)

    match = re.match(r"(.*)_([0-9]+|misc)", task_name)
    if match:
        return match.group(1)

    return task_name
