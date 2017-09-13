"""API to parse and access the configuration present in a evergreen.yml file.
The API also provides methods to access specific fields present in the mongodb/mongo
configuration file.
"""

import datetime
import fnmatch
import re

import yaml


class EvergreenProjectConfig(object):
    """Represent an Evergreen project configuration file."""

    def __init__(self, path):
        """Initialize the EvergreenProjectConfig from a file path name."""
        with open(path, "r") as fstream:
            self._conf = yaml.load(fstream)
        self.path = path
        self.tasks = [Task(task_dict) for task_dict in self._conf["tasks"]]
        self._tasks_by_name = {task.name: task for task in self.tasks}
        self.variants = [Variant(variant_dict, self._tasks_by_name)
                         for variant_dict in self._conf["buildvariants"]]
        self._variants_by_name = {variant.name: variant for variant in self.variants}
        self.distro_names = set()
        for variant in self.variants:
            self.distro_names.update(variant.distro_names)

    @property
    def task_names(self):
        """The list of task names."""
        return self._tasks_by_name.keys()

    def get_task(self, task_name):
        """Return the task with the given name as a Task instance."""
        return self._tasks_by_name.get(task_name)

    @property
    def lifecycle_task_names(self):
        """The list of names of the tasks that have not been excluded from test lifecycle."""
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
        """The list of build variant names."""
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
        """The task name."""
        return self.raw["name"]

    @property
    def depends_on(self):
        """The list of task names this task depends on."""
        return self.raw.get("depends_on", [])

    @property
    def resmoke_args(self):
        """The value of the resmoke_args argument of the 'run tests' function if it is
        defined, or None.
        """
        for command in self.raw.get("commands", []):
            if command.get("func") == "run tests":
                return command.get("vars", {}).get("resmoke_args")
        return None

    @property
    def resmoke_suite(self):
        """The value of the --suites option in the resmoke_args argument of the 'run tests'
         function if it is defined, or None. """
        args = self.resmoke_args
        if args:
            return ResmokeArgs.get_arg(args, "suites")

    def __str__(self):
        return self.name


class Variant(object):
    """Represent a build variant configuration as found in an Evergreen project
     configuration file.
     """

    def __init__(self, conf_dict, task_map):
        self.raw = conf_dict
        run_on = self.run_on
        self.tasks = [VariantTask(task_map.get(t["name"]), t.get("distros", run_on), self)
                      for t in conf_dict["tasks"]]
        self.distro_names = set(run_on)
        for task in self.tasks:
            self.distro_names.update(task.run_on)

    @property
    def name(self):
        """The build variant name."""
        return self.raw["name"]

    @property
    def display_name(self):
        """The build variant display name, or None if not found."""
        return self.raw.get("display_name")

    @property
    def batchtime(self):
        """The build variant batchtime parameter as a datetime.timedelta, or None if not found."""
        batchtime = self.raw.get("batchtime")
        return datetime.timedelta(minutes=batchtime) if batchtime is not None else None

    @property
    def modules(self):
        """The build variant modules parameter as a list of module names."""
        modules = self.raw.get("modules")
        return modules if modules is not None else []

    @property
    def run_on(self):
        """The build variant run_on parameter as a list of distro names."""
        run_on = self.raw.get("run_on")
        return run_on if run_on is not None else []

    @property
    def task_names(self):
        """The list of task names."""
        return [t.name for t in self.tasks]

    def get_task(self, task_name):
        """Return the task with the given name as an instance of VariantTask or None if this
        variant does not run the task.
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
        """Return the value of the test_flags expansion or None if not found."""
        return self.expansion("test_flags")

    @property
    def num_jobs_available(self):
        """Return the value of the num_jobs_available expansion or None if not found."""
        return self.expansion("num_jobs_available")


class VariantTask(Task):
    """Represent a task definition in the context of a build variant."""
    def __init__(self, task, run_on, variant):
        Task.__init__(self, task.raw)
        self.run_on = run_on
        self.variant = variant

    @property
    def combined_resmoke_args(self):
        """Return the combined resmoke arguments resulting from the concatenation of the task's
        resmoke_args parameter and the variant's test_flags parameter.

        If the task does not have a 'resmoke_args' parameter, then None is returned.
        """
        resmoke_args = self.resmoke_args
        test_flags = self.variant.test_flags
        if resmoke_args is None:
            return None
        elif test_flags is None:
            return self.resmoke_args
        else:
            return "{} {}".format(resmoke_args, test_flags)


class ResmokeArgs(object):

    @staticmethod
    def get_arg(resmoke_args, name):
        """Return the value of the option --'name' in the 'resmoke_args' string or
        None if not found.
        """
        match = re.search(r"--{}[=\s](?P<value>\w+)".format(name), resmoke_args)
        if match:
            return match.group("value")
