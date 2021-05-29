"""Functions for working with resmoke task names."""

import math

GEN_SUFFIX = "_gen"


def name_generated_task(parent_name, task_index, total_tasks, variant=None):
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

    index_width = int(math.ceil(math.log10(total_tasks)))
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
