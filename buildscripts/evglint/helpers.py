"""Helpers for iterating over the yaml dictionary."""
import re
from typing import Generator, Tuple, Union, List, Callable

_CommandList = List[dict]
_Commands = Union[dict, _CommandList]
_Selector = Callable[[str, _Commands], Generator]


def _in_dict_and_truthy(dictionary: dict, key: str) -> bool:
    return key in dictionary and dictionary[key]


def iterate_commands(yaml_dict: dict) -> Generator[Tuple[str, dict], None, None]:
    """Return a Generator that yields commands from the yaml dict.

    :param dict yaml_dict: the parsed yaml dictionary

    Yields a Tuple, 0: is a human friendly description of where the command
    block can be found, 1: a dict representing the command
    """
    generator = iterate_commands_context(yaml_dict)
    for (context, command, _) in generator:
        yield (context, command)


# pylint: disable=too-many-branches
def _iterator(yaml_dict: dict, selector: _Selector,
              skip_blocks: List[str] = None) -> Generator[Tuple[str, dict, dict], None, None]:
    def _should_process(yaml_dict: dict, key: str) -> bool:
        if skip_blocks and key in skip_blocks:
            return False

        return _in_dict_and_truthy(yaml_dict, key)

    if _should_process(yaml_dict, "functions"):
        for function, commands in yaml_dict["functions"].items():
            if not commands:
                continue
            gen = selector(f"Function '{function}'", commands)
            for out in gen:
                yield out

    if _should_process(yaml_dict, "tasks"):
        for task in yaml_dict["tasks"]:
            if _in_dict_and_truthy(task, "commands"):
                gen = selector(f"Task '{task['name']}'", task["commands"])
                for out in gen:
                    yield out
            if _in_dict_and_truthy(task, "setup_task"):
                gen = selector(f"Task '{task['name']}', setup_task", task["setup_task"])
                for out in gen:
                    yield out
            if _in_dict_and_truthy(task, "teardown_task"):
                gen = selector(f"Task '{task['name']}', teardown_task", task["teardown_task"])
                for out in gen:
                    yield out
            if _in_dict_and_truthy(task, "setup_group"):
                gen = selector(f"Task '{task['name']}', setup_group", task["setup_group"])
                for out in gen:
                    yield out
            if _in_dict_and_truthy(task, "teardown_group"):
                gen = selector(f"Task '{task['name']}', teardown_group", task["teardown_group"])
                for out in gen:
                    yield out
            if _in_dict_and_truthy(task, "timeout"):
                gen = selector(f"Task '{task['name']}', timeout", task["timeout"])
                for out in gen:
                    yield out

    if _should_process(yaml_dict, "pre"):
        gen = selector("Global pre", yaml_dict["pre"])
        for out in gen:
            yield out
    if _should_process(yaml_dict, "post"):
        gen = selector("Global post", yaml_dict["post"])
        for out in gen:
            yield out
    if _should_process(yaml_dict, "timeout"):
        gen = selector("Global timeout", yaml_dict["timeout"])
        for out in gen:
            yield out


def iterate_commands_context(yaml_dict: dict, skip_blocks: List[str] = None
                             ) -> Generator[Tuple[str, dict, dict], None, None]:
    """Return a Generator that yields commands from the yaml dict.

    :param dict yaml_dict: the parsed yaml dictionary
    :param list skip_blocks: skip root level keys in the yaml dictionary in list

    Yields a Tuple, 0: is a human friendly description of where the command
    block can be found, 1: a dict representing the command, 2: a dict or list of
    dicts that provides the whole context of where that command is used. For
    example, when iterating over commands in a function, this will be the list
    of dicts or single dict that makes up the function definition.

    Functions will not be returned.
    """

    def _helper(prefix: str,
                commands: _Commands) -> Generator[Tuple[str, _Commands, _CommandList], None, None]:
        # commands are either a singular dict (representing one command), or
        # a list of dicts
        if isinstance(commands, dict):
            # never yield functions
            if "command" in commands:
                yield (f'{prefix}, command', commands, commands)
        else:
            for idx, command in enumerate(commands):
                if "command" in command:
                    yield (f"{prefix}, command {idx}", command, commands)

    gen = _iterator(yaml_dict, _helper, skip_blocks)
    for out in gen:
        yield out


def iterate_fn_calls_context(yaml_dict: dict) -> Generator[Tuple[str, dict, dict], None, None]:
    """Return a Generator that yields function calls from the yaml dict.

    :param dict yaml_dict: the parsed yaml dictionary

    Yields a Tuple, 0: is a human friendly description of where the command
    block can be found, 1: a dict representing the command, 2: a dict or list of
    dicts that provides the whole context of where that command is used. For
    example, when iterating over commands in a function, this will be the list
    of dicts or single dict that makes up the function definition.

    Function definitions will not be returned.
    """

    def _helper(prefix: str, commands: Union[dict, List[dict]]):
        # commands are either a singular dict (representing one command), or
        # a list of dicts
        if isinstance(commands, dict):
            # only yield functions
            if "func" in commands:
                yield (f"{prefix}, function call '{commands['func']}'", commands, commands)
        else:
            for idx, command in enumerate(commands):
                if "func" in command:
                    yield (f"{prefix}, command {idx} (function call: '{command['func']}')", command,
                           commands)

    gen = _iterator(yaml_dict, _helper)
    for out in gen:
        yield out


EVERGREEN_SCRIPT_RE = re.compile(r".*\/evergreen\/.*\.sh")


# Return true for any subprocess exec commands that look like this:
#- command: subprocess.exec
#  params:
# with one of :
#    args:
#      - r"\/evergreen\/.*\.sh"
# or
#    command: r"\/evergreen\/.*\.sh"
def match_subprocess_exec(command: dict) -> bool:
    """Return True if the command is a subprocess.exec command that consumes an evergreen shell script."""
    if "command" in command and command["command"] != "subprocess.exec":
        return False
    if "params" not in command:
        return False

    params = command["params"]
    try:
        if "args" in params and not EVERGREEN_SCRIPT_RE.search(params["args"][0]):
            return False
        elif "command" in params and not EVERGREEN_SCRIPT_RE.search(params["command"]):
            return False
    except IndexError:
        return False

    return True


def match_expansions_update(command: dict) -> bool:
    """Return True if the command is an expansions.update command."""
    return "command" in command and command["command"] == "expansions.update"


def match_timeout_update(command: dict) -> bool:
    """Return True if the command is a timeout.update command."""
    return "command" in command and command["command"] == "timeout.update"


def match_expansions_write(command: dict) -> bool:
    """Return True if the command is a properly formed expansions.write command.

    Properly formed is of the form:
      command: expansions.write
      params:
        file: expansions.yml
        redacted: true
    """
    if "command" not in command or command["command"] != "expansions.write":
        return False

    if "params" not in command:
        return False

    params = command["params"]
    if "file" not in params or params["file"] != "expansions.yml":
        return False
    if "redacted" not in params or not params["redacted"]:
        return False

    return True


def iterate_command_lists(yaml_dict: dict) -> Generator[Tuple[str, _Commands], None, None]:
    """Return a Generator that yields every single command list once.

    Command lists are defined as the list of commands found in tasks:
    commands, setup_task, setup_group, teardown_task, teardown_group, timeout;
    globally: pre, post, timeout, and the definitions of functions.

    :param dict yaml_dict: the parsed yaml dictionary

    Yields a Tuple, 0: is a human friendly description of where the command
    block can be found, representing the command, 1: a dict or list of
    dicts that provides the whole context of where that command is used. For
    example, when iterating over commands in a function, this will be the list
    of dicts or single dict that makes up the function definition.
    """

    def _helper(prefix: str, commands: Union[dict, List[dict]]):
        yield (prefix, commands)

    gen = _iterator(yaml_dict, _helper)
    for out in gen:
        yield out
