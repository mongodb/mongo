"""Helpers for iterating over the yaml dictionary."""
from typing import Generator, Tuple, Union, List


def _in_dict_and_truthy(dictionary: dict, key: str) -> bool:
    return key in dictionary and dictionary[key]


# pylint: disable=too-many-branches
def iterate_commands(yaml_dict: dict) -> Generator[Tuple[str, dict], None, None]:
    """Return a Generator that yields commands from the yaml dict.

    Yields a Tuple, 0: is a human friendly description of where the command
    block can be found, 1: a dict representing the command. Functions will not
    be returned.
    """

    def _helper(prefix: str, commands: Union[dict, List[dict]]):
        # commands are either a singular dict (representing one command), or
        # a list of dicts
        if isinstance(commands, dict):
            # only yield functions
            if "command" in commands:
                yield (f'{prefix}, command', commands)
        else:
            for idx, command in enumerate(commands):
                if "command" in command:
                    yield (f"{prefix}, command {idx}", command)

    if "functions" in yaml_dict:
        for function, commands in yaml_dict["functions"].items():
            if not commands:
                continue
            gen = _helper(f"Function '{function}'", commands)
            for out in gen:
                yield out

    for task in yaml_dict["tasks"]:
        if _in_dict_and_truthy(task, "commands"):
            gen = _helper(f"Task '{task['name']}'", task["commands"])
            for out in gen:
                yield out
        if _in_dict_and_truthy(task, "setup_task"):
            gen = _helper(f"Task '{task['name']}', setup_task", task["setup_task"])
            for out in gen:
                yield out
        if _in_dict_and_truthy(task, "teardown_task"):
            gen = _helper(f"Task '{task['name']}', teardown_task", task["teardown_task"])
            for out in gen:
                yield out
        if _in_dict_and_truthy(task, "setup_group"):
            gen = _helper(f"Task '{task['name']}', setup_group", task["setup_group"])
            for out in gen:
                yield out
        if _in_dict_and_truthy(task, "teardown_group"):
            gen = _helper(f"Task '{task['name']}', teardown_group", task["teardown_group"])
            for out in gen:
                yield out
        if _in_dict_and_truthy(task, "timeout"):
            gen = _helper(f"Task '{task['name']}', timeout", task["timeout"])
            for out in gen:
                yield out

    if _in_dict_and_truthy(yaml_dict, "pre"):
        gen = _helper("Global pre", yaml_dict["pre"])
        for out in gen:
            yield out
    if _in_dict_and_truthy(yaml_dict, "post"):
        gen = _helper("Global post", yaml_dict["post"])
        for out in gen:
            yield out
    if _in_dict_and_truthy(yaml_dict, "timeout"):
        gen = _helper("Global timeout", yaml_dict["timeout"])
        for out in gen:
            yield out
