"""API to parse and access the configuration present in a evergreen.yml file.

The API also provides methods to access specific fields present in the mongodb/mongo
configuration file.
"""

from __future__ import print_function

import datetime
import distutils.spawn  # pylint: disable=no-name-in-module
import fnmatch
import os
import re

import yaml

import buildscripts.util.runcommand as runcommand


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
        config = yaml.load(output)
    else:
        with open(path, "r") as fstream:
            config = yaml.load(fstream)

    return EvergreenProjectConfig(config)


class EvergreenProjectConfig(object):  # pylint: disable=too-many-instance-attributes
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
    def task_names(self):
        """Get the list of task names."""
        return self._tasks_by_name.keys()

    def get_task(self, task_name):
        """Return the task with the given name as a Task instance."""
        return self._tasks_by_name.get(task_name)

    @property
    def task_group_names(self):
        """Get the list of task_group names."""
        return self._task_groups_by_name.keys()

    def get_task_group(self, task_group_name):
        """Return the task_group with the given name as a Task instance."""
        return self._task_groups_by_name.get(task_group_name)

    @property
    def lifecycle_task_names(self):
        """Get the list of names of the tasks that have not been excluded from test lifecycle."""
        excluded = self._get_test_lifecycle_excluded_task_names()
        return [name for name in self.task_names if name not in excluded]

    def _get_test_lifecycle_excluded_task_names(self):
        excluded_patterns = self._conf.get("test_lifecycle_excluded_tasks", [])
        excluded = []
        for pattern in excluded_patterns:
            excluded.extend(fnmatch.filter(self.task_names, pattern))
        return excluded

    @property
    def variant_names(self):
        """Get the list of build variant names."""
        return self._variants_by_name.keys()

    def get_variant(self, variant_name):
        """Return the variant with the given name as a Variant instance."""
        return self._variants_by_name.get(variant_name)


class Task(object):
    """Represent a task configuration as found in an Evergreen project configuration file."""

    def __init__(self, conf_dict):
        """Initialize a Task from a dictionary containing its configuration."""
        self.raw = conf_dict

    @property
    def name(self):
        """Get the task name."""
        return self.raw["name"]

    @property
    def depends_on(self):
        """Get the list of task names this task depends on."""
        return self.raw.get("depends_on", [])

    @property
    def resmoke_args(self):
        """Get the resmoke_args from 'run tests' function if defined, or None."""
        for command in self.raw.get("commands", []):
            if command.get("func") == "run tests":
                return command.get("vars", {}).get("resmoke_args")
        return None

    @property
    def resmoke_suite(self):
        """Get the --suites option in the resmoke_args of 'run tests' if defined, or None."""
        args = self.resmoke_args
        if args:
            return ResmokeArgs.get_arg(args, "suites")
        return None

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

    @property
    def run_on(self):
        """Get build variant run_on parameter as a list of distro names."""
        run_on = self.raw.get("run_on")
        return run_on if run_on is not None else []

    @property
    def task_names(self):
        """Get list of task names."""
        return [t.name for t in self.tasks]

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
    def test_flags(self):
        """Get the value of the test_flags expansion or None if not found."""
        return self.expansion("test_flags")

    @property
    def num_jobs_available(self):
        """Get the value of the num_jobs_available expansion or None if not found."""
        return self.expansion("num_jobs_available")


class VariantTask(Task):
    """Represent a task definition in the context of a build variant."""

    def __init__(self, task, run_on, variant):
        """Initialize VariantTask."""
        Task.__init__(self, task.raw)
        self.run_on = run_on
        self.variant = variant

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


class ResmokeArgs(object):
    """ResmokeArgs class."""

    @staticmethod
    def get_arg(resmoke_args, name):
        """Return the value from --'name' in the 'resmoke_args' string or None if not found."""
        match = re.search(r"--{}[=\s](?P<value>\w+)".format(name), resmoke_args)
        if match:
            return match.group("value")
        return None
