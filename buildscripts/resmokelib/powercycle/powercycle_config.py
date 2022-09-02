"""Powercycle tasks config."""
import yaml

from buildscripts.resmokelib.powercycle import powercycle, powercycle_constants

POWERCYCLE_TASKS_CONFIG = "buildscripts/resmokeconfig/powercycle/powercycle_tasks.yml"


class PowercycleTaskConfig:
    """Class represents single task in powercycle tasks config."""

    def __init__(self, task_yaml):
        """Initialize."""

        self.name = task_yaml.get("name", "")
        self.crash_method = task_yaml.get("crash_method", powercycle_constants.DEFAULT_CRASH_METHOD)
        self.test_loops = task_yaml.get("test_loops", powercycle_constants.DEFAULT_TEST_LOOPS)
        self.seed_doc_num = task_yaml.get("seed_doc_num", powercycle_constants.DEFAULT_SEED_DOC_NUM)

        self.write_concern = task_yaml.get("write_concern", "{}")
        self.read_concern_level = task_yaml.get("read_concern_level", None)

        self.fcv = task_yaml.get("fcv", None)
        self.repl_set = task_yaml.get("repl_set", None)
        self.mongod_options = task_yaml.get("mongod_options",
                                            powercycle_constants.DEFAULT_MONGOD_OPTIONS)

    def __str__(self):
        """Return as dict."""

        return self.__dict__.__str__()


def get_task_config(task_name, is_remote):
    """Return powercycle task config."""

    if is_remote:
        config_location = powercycle.abs_path(
            f"{powercycle_constants.REMOTE_DIR}/{POWERCYCLE_TASKS_CONFIG}")
    else:
        config_location = powercycle.abs_path(POWERCYCLE_TASKS_CONFIG)

    with open(config_location) as file_handle:
        raw_yaml = yaml.safe_load(file_handle)
    tasks_raw_yaml = raw_yaml.get("tasks", [])

    for single_task_yaml in tasks_raw_yaml:
        if single_task_yaml["name"] == task_name:
            return PowercycleTaskConfig(single_task_yaml)

    raise Exception(f"Task with name '{task_name}' is not found"
                    f" in powercycle tasks configuration file '{POWERCYCLE_TASKS_CONFIG}'."
                    f" Please add a task there with the appropriate name.")
