"""Lint rules."""
import re
from typing import Dict, List

from buildscripts.evglint.model import LintRule, LintError
from buildscripts.evglint.helpers import iterate_commands


def no_keyval_inc(yaml: dict) -> List[LintError]:
    """Prevent usage of keyval.inc."""

    def _out_message(context: str) -> LintError:
        return f"{context} includes keyval.inc, which is not permitted. Do not use keyval.inc."

    out: List[LintError] = []
    for context, command in iterate_commands(yaml):
        if "command" in command and command["command"] == "keyval.inc":
            out.append(_out_message(context))

    return out


def shell_exec_explicit_shell(yaml: dict) -> List[LintError]:
    """Require explicitly specifying shell in uses of shell.exec."""

    def _out_message(context: str) -> LintError:
        return f"{context} is a shell.exec command without an explicitly declared shell. You almost certainly want to add 'shell: bash' to the parameters list."

    out: List[LintError] = []
    for context, command in iterate_commands(yaml):
        if "command" in command and command["command"] == "shell.exec":
            if "params" not in command or "shell" not in command["params"]:
                out.append(_out_message(context))

    return out


SHELL_COMMANDS = ["subprocess.exec", "shell.exec"]


def no_working_dir_on_shell(yaml: dict) -> List[LintError]:
    """Do not allow working_dir to be set on shell.exec, subprocess.*."""

    def _out_message(context: str, cmd: str) -> LintError:
        return f"{context} is a {cmd} command with a working_dir parameter. Do not set working_dir, instead `cd` into the directory in the shell script."

    out: List[LintError] = []
    for context, command in iterate_commands(yaml):
        if "command" in command and command["command"] in SHELL_COMMANDS:
            if "params" in command and "working_dir" in command["params"]:
                out.append(_out_message(context, command["command"]))

    return out


FUNCTION_NAME = "^f_[a-z][A-Za-z0-9_]*"
FUNCTION_NAME_RE = re.compile(FUNCTION_NAME)


def invalid_function_name(yaml: dict) -> List[LintError]:
    """Enforce naming convention on functions."""

    def _out_message(context: str) -> LintError:
        return f"Function '{context}' must have a name matching '{FUNCTION_NAME}'"

    if "functions" not in yaml:
        return []

    out: List[LintError] = []
    for fname in yaml["functions"].keys():
        if not FUNCTION_NAME_RE.fullmatch(fname):
            out.append(_out_message(fname))

    return out


def no_shell_exec(yaml: dict) -> List[LintError]:
    """Do not allow shell.exec. Users should use subprocess.exec instead."""

    def _out_message(context: str) -> LintError:
        return (f"{context} is a shell.exec command, which is forbidden. "
                "Extract your shell script out of the YAML and into a .sh file "
                "in directory 'evergreen', and use subprocess.exec instead.")

    out: List[LintError] = []
    for context, command in iterate_commands(yaml):
        if "command" in command and command["command"] == "shell.exec":
            out.append(_out_message(context))
    return out


def no_multiline_expansions_update(yaml: dict) -> List[LintError]:
    """Forbid multi-line values in expansion.updates parameters."""

    def _out_message(context: str, idx: int) -> LintError:
        return (f"{context}, key-value pair {idx} is an expansions.update "
                "command a multi-line values, which is forbidden. For "
                "long-form values, prefer expansions.write.")

    out: List[LintError] = []
    for context, command in iterate_commands(yaml):
        if "command" in command and command["command"] == "expansions.update":
            if "params" in command and "updates" in command["params"]:
                for idx, item in enumerate(command["params"]["updates"]):
                    if "value" in item and "\n" in item["value"]:
                        out.append(_out_message(context, idx))
    return out


BUILD_PARAMETER = "[a-z][a-z0-9_]*"
BUILD_PARAMETER_RE = re.compile(BUILD_PARAMETER)


def invalid_build_parameter(yaml: dict) -> List[LintError]:
    """Require that parameters obey a naming convention and have a description."""

    def _out_message_key(idx: int) -> LintError:
        return f"Build parameter, pair {idx}, key must match '{BUILD_PARAMETER}'."

    def _out_message_description(idx: int) -> LintError:
        return f"Build parameter, pair {idx}, must have a description."

    if "parameters" not in yaml:
        return []

    out: List[LintError] = []
    for idx, param in enumerate(yaml["parameters"]):
        if "key" not in param or not BUILD_PARAMETER_RE.fullmatch(param["key"]):
            out.append(_out_message_key(idx))
        if "description" not in param or not param["description"]:
            out.append(_out_message_description(idx))
    return out


EVERGREEN_SCRIPT_RE = re.compile(r"\/evergreen\/.*\.sh")


def subprocess_exec_bootstraps_shell(yaml: dict) -> List[LintError]:
    """Require that subprocess.exec functions that consume evergreen scripts correctly bootstrap the prelude."""

    def _out_message(context: str, key: str) -> LintError:
        return f"{context} is a subprocess.exec command that calls an evergreen shell script without a correctly set environment. You must set 'params.env.{key}' to '${{{key}}}'."

    def _out_message_binary(context: str) -> LintError:
        return f"{context} is a subprocess.exec command that calls an evergreen shell script through a binary other than bash, which is unsupported."

    # we're looking for subprocess exec commands that look like this
    #- command: subprocess.exec
    #  params:
    #    args:
    #      - "src/evergreen/do_something.sh"
    # if we find one, we want to ensure that env on params is set to correctly
    # allow activate_venv to be bootstrapped, and the binary is set to bash

    out: List[LintError] = []
    for context, command in iterate_commands(yaml):
        if "command" in command and command["command"] != "subprocess.exec":
            continue
        if "params" not in command:
            continue

        params = command["params"]
        if "args" not in params or not EVERGREEN_SCRIPT_RE.search(params["args"][0]):
            continue
        if "binary" not in params or params["binary"] != "bash":
            out.append(_out_message_binary(context))

        if "env" in params:
            if "workdir" not in params["env"] or params["env"]["workdir"] != "${workdir}":
                out.append(_out_message(context, "workdir"))
            if "python" not in params["env"] or params["env"]["python"] != "${python}":
                out.append(_out_message(context, "python"))
        else:
            out.append(_out_message(context, "workdir"))
            out.append(_out_message(context, "python"))

    return out


RULES: Dict[str, LintRule] = {
    #"invalid-function-name": invalid_function_name,
    # TODO: after SERVER-54315
    #"no-keyval-inc": no_keyval_inc,
    #"no-working-dir-on-shell": no_working_dir_on_shell,
    "shell-exec-explicit-shell": shell_exec_explicit_shell,
    # this rule contradicts the above. When you turn it on, delete shell_exec_explicit_shell
    #"no-shell-exec": no_shell_exec
    #"no-multiline-expansions-update": no_multiline_expansions_update,
    "invalid-build-parameter": invalid_build_parameter,
    #"subprocess-exec-bootstraps-shell": subprocess_exec_bootstraps_shell
}
# Thoughts on Writing Rules
# - see .helpers for reliable iteration helpers
# - Do not assume a key exists, unless it's been mentioned here
# - Do not allow exceptions to percolate outside of the rule function
# - YAML anchors are not available. Unless you want to write your own yaml
#   parser, or fork adrienverge/yamllint, abandon all hope on that idea you have.
# - Anchors are basically copy and paste, so you might see "duplicate" errors
#   that originate from the same anchor, but are reported in multiple locations

# Evergreen YAML Root Structure Reference
# Unless otherwise mentioned, the key is optional. You can infer the
# substructure by reading etc/evergreen.yml

# Function blocks: are dicts with the key 'func', which maps to a string,
# the name of the function
# Command blocks: are dicts with the key 'command', which maps to a string,
# the Evergreen command to run

# variables: List[dict]. These can be any valid yaml and it's very difficult
#   to infer anything
# functions: Dict[str, Union[dict, List[dict]]]. The key is the name of the
#   function, the value is either a dict, or list of dicts, with each dict
#   representing a command
# pre, post, and timeout: List[dict] representing commands or functions to
#   be run before/after/on timeout condition respectively
# tasks: List[dict], each dict is a task definition, key is always present
# task_groups: List[dict]
# modules: List[dict]
# buildvariants: List[dict], key is always present
# parameters: List[dict]
