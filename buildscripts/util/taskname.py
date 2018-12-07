"""Functions for working with resmoke task names."""

import math


def name_generated_task(parent_name, task_index, total_tasks, variant=None):
    """
    Create a zero-padded sub-task name.

    :param parent_name: Name of the parent task.
    :param task_index: Index of this sub-task.
    :param total_tasks: Total number of sub-tasks being generated.
    :return: Zero-padded name of sub-task.
    """
    suffix = ""
    if variant:
        suffix = "_{0}".format(variant)

    index_width = int(math.ceil(math.log10(total_tasks)))
    return "{0}_{1}{2}".format(parent_name, str(task_index).zfill(index_width), suffix)
