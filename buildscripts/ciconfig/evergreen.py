"""API to parse and access the configuration present in a evergreen.yml file.

The API also provides methods to access specific fields present in the mongodb/mongo
configuration file.
"""
from __future__ import annotations

import datetime
import distutils.spawn
from typing import Set, List, Optional

import yaml

import buildscripts.util.runcommand as runcommand

ENTERPRISE_MODULE_NAME = "enterprise"
ASAN_SIGNATURE = "detect_leaks=1"


def parse_evergreen_file(path, evergreen_binary="evergreen"):
    """Read an Evergreen file and return EvergreenProjectConfig instance."""
    if evergreen_binary:
        if not distutils.spawn.find_executable(evergreen_binary):
            raise EnvironmentError(
                "Executable '{}' does not exist or is not in the PATH.".format(evergreen_binary))

        # Call 'evergreen evaluate path' to pre-process the project configuration file.
        cmd = runcommand.RunCommand(evergreen_binary)
        cmd.add("evaluate")
        cmd.add_file(path)
        error_code, output = cmd.execute()
        if error_code:
            raise RuntimeError("Unable to evaluate {}: {}".format(path, output))
        config = yaml.safe_load(output)
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
    def generate_resmoke_tasks_command(self):
        """Return the 'generate resmoke tasks' command if found, or None."""
        return self.find_func_command("generate resmoke tasks")

    @property
    def is_generate_resmoke_task(self) -> bool:
        """Return True if 'generate resmoke tasks' command is found."""
        return self.generate_resmoke_tasks_command is not None

    @property
    def run_tests_command(self):
        """Return the 'run tests' command if found, or None."""
        return self.find_func_command("run tests")

    @property
    def is_run_tests_task(self):
        """Return True if 'run_tests' command is found."""
        return self.run_tests_command is not None

    @property
    def multiversion_setup_command(self):
        """Return the 'do multiversion setup' command if found, or None."""
        return self.find_func_command("do multiversion setup")

    @property
    def generated_task_name(self):
        """
        Get basename of the tasks generated by this _gen task.

        :return: Basename of the generated tasks.
        """
        if not self.is_generate_resmoke_task:
            raise TypeError("Only _gen tasks can have generated task names")

        return self.name[:-4]

    def get_resmoke_command_vars(self):
        """Get the vars for either 'generate resmoke tasks' or 'run tests', both eventually call resmoke.py."""
        if self.is_run_tests_task:
            return self.run_tests_command.get("vars")
        elif self.is_generate_resmoke_task:
            return self.generate_resmoke_tasks_command.get("vars")

        return None

    def get_suite_name(self):
        """Get the name of the resmoke.py suite; the `suite` expansion overrides the task name."""

        if self.is_run_tests_task:
            suite_name = self.name
        elif self.is_generate_resmoke_task:
            suite_name = self.generated_task_name
        else:
            raise ValueError(f"{self.name} task does not run a resmoke.py test suite")

        command_vars = self.get_resmoke_command_vars()
        if command_vars is not None:
            suite_name = command_vars.get("suite", suite_name)

        return suite_name

    @property
    def resmoke_args(self):
        """Get the resmoke_args from 'run tests' function if defined, or None."""
        suite_name = self.get_suite_name()
        command_vars = self.get_resmoke_command_vars()

        other_args = ""
        if command_vars:
            other_args = command_vars.get("resmoke_args", other_args)

        if not suite_name and not other_args:
            return None

        return f"--suites={suite_name} {other_args}"

    @property
    def tags(self):
        """Get a set of tags this task has been marked with."""
        if self._tags is None:
            self._tags = set(self.raw.get("tags", []))
        return self._tags

    def require_multiversion_setup(self):
        """Check if the task requires running the multiversion setup."""
        return "multiversion" in self.tags

    def require_multiversion_version_combo(self):
        """Check if the task requires generating combinations of multiversion versions."""
        return "multiversion" in self.tags and "no_version_combination" not in self.tags

    def requires_npm(self):
        """Check if the task needs to run npm setup."""
        return "require_npm" in self.tags

    def is_test_name_random(self):
        """
        Check if the name of the tests are randomly generated.

        Those tests won't have associated test history.
        """
        return "random_name" in self.tags

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
                        VariantTask(task_map.get(task_in_group), task.get("distros", run_on), self))
            else:
                self.tasks.append(
                    VariantTask(task_map.get(task["name"]), task.get("distros", run_on), self))
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
        """Determine if this build variant include the enterprise module."""
        return ENTERPRISE_MODULE_NAME in set(self.modules)

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
        return self.display_name.startswith("! ")

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
    def combined_resmoke_args(self):
        """Get the combined resmoke arguments.

        This results from the concatenation of the task's resmoke_args parameter and the
        variant's test_flags parameter.
        """
        resmoke_args = self.resmoke_args
        test_flags = self.variant.test_flags
        if resmoke_args is None:
            return None
        elif test_flags is None:
            return self.resmoke_args
        return "{} {}".format(resmoke_args, test_flags)
