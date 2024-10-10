"""API to parse and access the configuration present in a evergreen.yml file.

The API also provides methods to access specific fields present in the mongodb/mongo
configuration file.
"""

from __future__ import annotations

import datetime
import os
import re
import shutil
import subprocess
import sys
from typing import Any, Dict, List, Optional, Set

import structlog
import yaml

ENTERPRISE_MODULE_NAME = "enterprise"
ASAN_SIGNATURE = "detect_leaks=1"

LOGGER = structlog.get_logger(__name__)


def parse_evergreen_file(path, evergreen_binary="evergreen"):
    """Read an Evergreen file and return EvergreenProjectConfig instance."""
    if evergreen_binary:
        if not shutil.which(evergreen_binary):
            # On windows in python3.8 there was an update to no longer use HOME in os.path.expanduser
            # However, cygwin is weird and has HOME but not USERPROFILE
            # So we just check if HOME is set and USERPROFILE is not
            # Then we just set USERPROFILE and unset it after
            # Bug is here: https://bugs.python.org/issue36264

            prev_environ = os.environ.copy()
            if sys.platform in ("win32", "cygwin"):
                LOGGER.info(f"Previous os.environ={os.environ} before updating 'USERPROFILE'")
                if "HOME" in os.environ:
                    os.environ["USERPROFILE"] = os.environ["HOME"]
                else:
                    LOGGER.warn(
                        "'HOME' enviorment variable unset. This will likely cause us to be unable to find evergreen binary."
                    )

            default_evergreen_location = os.path.expanduser(os.path.join("~", "evergreen"))

            # Restore enviorment if it was modified above on windows
            os.environ.clear()
            os.environ.update(prev_environ)

            if os.path.exists(default_evergreen_location):
                evergreen_binary = default_evergreen_location
            elif os.path.exists(f"{default_evergreen_location}.exe"):
                evergreen_binary = f"{default_evergreen_location}.exe"
            else:
                raise EnvironmentError(
                    f"Executable {evergreen_binary} (default location: {default_evergreen_location}) does not exist or is not in the PATH. PATH={os.environ.get('PATH')}"
                )
        else:
            evergreen_binary = shutil.which(evergreen_binary)

        # Call 'evergreen evaluate path' to pre-process the project configuration file.
        cmd = [evergreen_binary, "evaluate", path]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(
                "Unable to evaluate {}.\nSTDOUT:{}\nSTDERR:{}".format(
                    path, result.stdout, result.stderr
                )
            )
        config = yaml.safe_load(result.stdout)
    else:
        with open(path, "r") as fstream:
            config = yaml.safe_load(fstream)

    return EvergreenProjectConfig(config)


class EvergreenProjectConfig(object):
    """Represent an Evergreen project configuration file."""

    def __init__(self, conf):
        """Initialize the EvergreenProjectConfig from a YML dictionary."""
        self._conf = conf
        self.tasks = [Task(task_dict) for task_dict in self._conf["tasks"]]
        self._tasks_by_name = {task.name: task for task in self.tasks}
        self.task_groups = [
            TaskGroup(task_group_dict) for task_group_dict in self._conf.get("task_groups", [])
        ]
        self._task_groups_by_name = {task_group.name: task_group for task_group in self.task_groups}
        self.variants = [
            Variant(variant_dict, self._tasks_by_name, self._task_groups_by_name)
            for variant_dict in self._conf["buildvariants"]
        ]
        self._variants_by_name = {variant.name: variant for variant in self.variants}
        self.distro_names = set()
        for variant in self.variants:
            self.distro_names.update(variant.distro_names)
        self.functions = self._conf["functions"]

    @property
    def task_names(self) -> List[str]:
        """Get the list of task names."""
        return list(self._tasks_by_name.keys())

    def get_task(self, task_name: str) -> Task:
        """Return the task with the given name as a Task instance."""
        return self._tasks_by_name.get(task_name)

    @property
    def task_group_names(self):
        """Get the list of task_group names."""
        return list(self._task_groups_by_name.keys())

    def get_task_group(self, task_group_name):
        """Return the task_group with the given name as a Task instance."""
        return self._task_groups_by_name.get(task_group_name)

    @property
    def variant_names(self):
        """Get the list of build variant names."""
        return list(self._variants_by_name.keys())

    def get_variant(self, variant_name: str) -> Variant:
        """Return the variant with the given name as a Variant instance."""
        return self._variants_by_name.get(variant_name)

    def get_required_variants(self) -> Set[Variant]:
        """Get the list of required build variants."""
        return {variant for variant in self.variants if variant.is_required_variant()}

    def get_task_names_by_tag(self, tag):
        """Return the list of tasks that have the given tag."""
        return list(task.name for task in self.tasks if tag in task.tags)


class Task(object):
    """Represent a task configuration as found in an Evergreen project configuration file."""

    def __init__(self, conf_dict):
        """Initialize a Task from a dictionary containing its configuration."""
        self.raw = conf_dict

        # Lazy parse task tags.
        self._tags = None

    @property
    def name(self):
        """Get the task name."""
        return self.raw["name"]

    @property
    def depends_on(self):
        """Get the list of task names this task depends on."""
        return self.raw.get("depends_on", [])

    def find_func_command(self, func_command):
        """Return the 'func_command' if found, or None."""
        for command in self.raw.get("commands", []):
            if command.get("func") == func_command:
                return command
        return None

    @property
    def generate_resmoke_tasks_command(self) -> Optional[Dict[str, Any]]:
        """Return the 'generate resmoke tasks' command if found, or None."""
        return self.find_func_command("generate resmoke tasks")

    @property
    def is_generate_resmoke_task(self) -> bool:
        """Return True if 'generate resmoke tasks' command is found."""
        return self.generate_resmoke_tasks_command is not None

    @property
    def run_tests_command(self) -> Optional[Dict[str, Any]]:
        """Return the 'run tests' command if found, or None."""
        return self.find_func_command("run tests")

    @property
    def is_run_tests_task(self) -> bool:
        """Return True if 'run_tests' command is found."""
        return self.run_tests_command is not None

    @property
    def initialize_multiversion_tasks_command(self) -> Optional[Dict[str, Any]]:
        """Return the 'initialize multiversion tasks' command if found, or None."""
        return self.find_func_command("initialize multiversion tasks")

    @property
    def is_initialize_multiversion_tasks_task(self) -> bool:
        """Return True if 'initialize multiversion tasks' command is found."""
        return self.initialize_multiversion_tasks_command is not None

    @property
    def generated_task_name(self) -> str:
        """
        Get basename of the tasks generated by this _gen task.

        :return: Basename of the generated tasks.
        """
        if not self.is_generate_resmoke_task:
            raise TypeError("Only _gen tasks can have generated task names")

        return self.name[:-4]

    def get_resmoke_command_vars(self) -> Dict[str, Any]:
        """Get the vars for either 'generate resmoke tasks' or 'run tests', both eventually call resmoke.py."""
        if self.is_run_tests_task:
            return self.run_tests_command.get("vars", {})
        if self.is_generate_resmoke_task:
            return self.generate_resmoke_tasks_command.get("vars", {})

        return {}

    def get_suite_names(self) -> List[str]:
        """Get the names of the resmoke.py suites from the task definition."""

        command_vars = self.get_resmoke_command_vars()

        if self.is_run_tests_task:
            return [command_vars.get("suite", self.name)]
        if self.is_generate_resmoke_task and not self.is_initialize_multiversion_tasks_task:
            return [command_vars.get("suite", self.generated_task_name)]
        if self.is_initialize_multiversion_tasks_task:
            return [
                suite for suite in self.initialize_multiversion_tasks_command.get("vars", {}).keys()
            ]

        raise ValueError(f"{self.name} task does not run a resmoke.py test suite")

    @property
    def suite_to_resmoke_args_map(self) -> Dict[str, str]:
        """Get the resmoke.py arguments from the task definition."""
        output = {}

        for suite_name in self.get_suite_names():
            resmoke_args = f"--suites={suite_name}"

            more_args = self.get_resmoke_command_vars().get("resmoke_args")
            if more_args is not None:
                resmoke_args = f"{resmoke_args} {more_args}"

            output[suite_name] = resmoke_args

        return output

    @property
    def tags(self):
        """Get a set of tags this task has been marked with."""
        if self._tags is None:
            self._tags = set(self.raw.get("tags", []))
        return self._tags

    def __str__(self):
        return self.name


class TaskGroup(object):
    """Represent a task_group configuration as found in an Evergreen project configuration file."""

    def __init__(self, conf_dict):
        """Initialize a TaskGroup from a dictionary containing its configuration."""
        self.raw = conf_dict

    @property
    def name(self):
        """Get the task_group name."""
        return self.raw["name"]

    @property
    def tasks(self):
        """Get the list of task names for task_group."""
        return self.raw.get("tasks", [])

    def __str__(self):
        return self.name


class Variant(object):
    """Build variant configuration as found in an Evergreen project configuration file."""

    def __init__(self, conf_dict, task_map, task_group_map):
        """Initialize Variant."""
        self.raw = conf_dict
        run_on = self.run_on
        self.tasks = []
        for task in conf_dict["tasks"]:
            task_name = task.get("name")
            if task_name in task_group_map:
                # A task in conf_dict may be a task_group, containing a list of tasks.
                for task_in_group in task_group_map.get(task_name).tasks:
                    self.tasks.append(
                        VariantTask(task_map.get(task_in_group), task.get("distros", run_on), self)
                    )
            else:
                self.tasks.append(
                    VariantTask(task_map.get(task["name"]), task.get("distros", run_on), self)
                )
        self.distro_names = set(run_on)
        for task in self.tasks:
            self.distro_names.update(task.run_on)

    def __repr__(self):
        """Create a string version of object for debugging."""
        return self.name

    @property
    def name(self):
        """Get the build variant name."""
        return self.raw["name"]

    @property
    def display_name(self):
        """Get the build variant display name, or None if not found."""
        return self.raw.get("display_name")

    @property
    def batchtime(self):
        """Get the build variant batchtime parameter as datetime.timedelta.

        Return None if the batchtime parameter is not found.
        """
        batchtime = self.raw.get("batchtime")
        return datetime.timedelta(minutes=batchtime) if batchtime is not None else None

    @property
    def modules(self):
        """Get build variant modules parameter as a list of module names."""
        modules = self.raw.get("modules")
        return modules if modules is not None else []

    def is_enterprise_build(self) -> bool:
        """Determine if the build variant is configured for enterprise builds."""
        pattern = r"--enableEnterpriseTests\s*=?\s*off"
        return not re.search(pattern, str(self.raw))

    @property
    def run_on(self):
        """Get build variant run_on parameter as a list of distro names."""
        run_on = self.raw.get("run_on")
        return run_on if run_on is not None else []

    @property
    def task_names(self):
        """Get list of task names."""
        return [t.name for t in self.tasks]

    def is_required_variant(self) -> bool:
        """Return True if the variant is a required variant."""
        return self.display_name.startswith("!")

    def get_task(self, task_name):
        """Return the task with the given name as an instance of VariantTask.

        Return None if this variant does not run the task.
        """
        for task in self.tasks:
            if task.name == task_name:
                return task
        return None

    def __str__(self):
        return self.name

    # Expansions

    def expansion(self, name):
        """Return the value of the expansion named 'name', or None if not found."""
        return self.raw.get("expansions", {}).get(name)

    @property
    def expansions(self):
        """Get the expansions."""
        return self.raw.get("expansions", [])

    @property
    def test_flags(self):
        """Get the value of the test_flags expansion or None if not found."""
        return self.expansion("test_flags")

    @property
    def num_jobs_available(self):
        """Get the value of the num_jobs_available expansion or None if not found."""
        return self.expansion("num_jobs_available")

    def is_asan_build(self) -> bool:
        """Determine if this task is an ASAN build."""
        san_options = self.expansion("san_options")
        if san_options:
            return ASAN_SIGNATURE in san_options
        return False

    @property
    def idle_timeout_factor(self) -> Optional[float]:
        """Get the value of idle_timeout_factor expansion or None if not found."""
        factor = self.expansion("idle_timeout_factor")
        if factor:
            return float(factor)
        return None

    @property
    def exec_timeout_factor(self) -> Optional[float]:
        """Get the value of exec_timeout_factor expansion or None if not found."""
        factor = self.expansion("exec_timeout_factor")
        if factor:
            return float(factor)
        return None


class VariantTask(Task):
    """Represent a task definition in the context of a build variant."""

    def __init__(self, task, run_on, variant):
        """Initialize VariantTask."""
        Task.__init__(self, task.raw)
        self.run_on = run_on
        self.variant = variant

    def __repr__(self):
        """Create a string representation of object for debugging."""
        return f"{self.variant}: {self.name}"

    @property
    def combined_suite_to_resmoke_args_map(self) -> Dict[str, str]:
        """Get the combined resmoke arguments.

        This results from the concatenation of the task's resmoke_args parameter and the
        variant's test_flags parameter.
        """
        variant_test_flags = self.variant.test_flags
        if variant_test_flags is not None:
            output = {}
            for suite_name, task_resmoke_args in self.suite_to_resmoke_args_map.items():
                output[suite_name] = f"{task_resmoke_args} {variant_test_flags}"
            return output
        return self.suite_to_resmoke_args_map
