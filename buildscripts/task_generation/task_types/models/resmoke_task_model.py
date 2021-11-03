"""Model mapping resmoke suites to tasks."""
from typing import NamedTuple, Optional, List

from shrub.v2 import Task


class ResmokeTask(NamedTuple):
    """Wrapper around shrub.py Task objects with tighter integration with resmoke.py."""

    shrub_task: Task
    resmoke_suite_name: str
    # Path to the generated file does not include the generated config directory.
    execution_task_suite_yaml_path: str
    execution_task_suite_yaml_name: str
    test_list: Optional[List[str]]
    excludes: Optional[List[str]]
