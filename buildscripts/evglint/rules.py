"""Lint rules."""
import re
from typing import Dict, List, Set, Optional

import buildscripts.evglint.helpers as helpers
from buildscripts.evglint.model import LintRule, LintError
from buildscripts.evglint.helpers import iterate_commands, iterate_commands_context, iterate_fn_calls_context, iterate_command_lists


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


SHELL_COMMANDS = ["subprocess.exec", "subprocess.scripting", "shell.exec"]


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
                "command with multi-line values embedded in the yaml, which is"
                " forbidden. For long-form values, use the files parameter of "
                "expansions.update.")

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


# pylint: disable=too-many-branches,too-many-locals,too-many-statements
def required_expansions_write(yaml: dict) -> List[LintError]:
    """Require that subprocess.exec functions that consume evergreen scripts correctly bootstrap the prelude."""

    # This logic is well and truly awful.
    # Here's the problem:
    # 1. Evergreen functions can have a dict or list definition. Functions
    # with a dict definition, that is:
    #   "f_my_fn": &f_my_fn
    #     command: shell.exec
    # can called either as "- func: f_my_fn" or as *f_my_fn. The former syntax
    # can have arguments passed into it. The latter syntax,
    # i.e. the use of the YAML anchor, is used to workaround Evergreen not
    # allowing functions to call other functions. Arguments cannot be passed in
    # when the YAML anchor syntax is used.

    # This poses a problem: if the user calls a function with a dict definition,
    # and that function has arguments passed in, and that function calls
    # subprocess.exec, then the function will not have its expansions populated
    # in the external script as expected. It must be defined as a list, and
    # incorporate an expansions.write call, or not be called with args.
    # See Resolution 1.

    # Functions with a list definition, that is:
    #   "f_my_fn2": &f_my_fn2
    #     - command: shell.exec
    #     - command: shell.exec
    # cannot use the YAML anchor syntax (this is an evergreen validation
    # error). If these functions call subprocess.exec, then they will only work
    # if expansions.write is called before subprocess.exec. See Resolution 2.
    #
    # 2. Expansions can be defined/changed at any step of the way by Evergreen,
    # function arguments, build variants, Evergreen projects, tasks. Expansions
    # written to disk MUST be up to date. See Resolution 2
    # 3. Expansions be updated after expansions.update, but before
    # subprocess.exec. See Resolution 3.

    # Resolutions:
    # Given the above, here are the checks we need to perform
    # 1. Any function defined with a dict definition that calls subprocess.exec
    # MUST NOT be called with arguments.
    # 2. expansions.write MUST be called prior to invoking subprocess.exec for
    # the first time in any command list.
    # (i.e. in tasks: commands, setup_task, setup_group, teardown_task,
    # teardown_group, timeout,, globally: pre, post, timeout, functions with
    # list definitions)
    # When paired with Resolution 3, this works all the time (but does
    # sometimes require redundant expansions.write calls)
    # 3. Every invocation of expansions.update MUST be immediately followed by
    # expansions.write

    # 4. It makes sense to place your expansions.write call in a dict
    # definition function, that way you can call it with either syntax. So, in
    # the above passage where I've mentioned "expansions.write", we must not
    # only check for a properly formed expansions.write call, we must also
    # check for a function that calls expansions.write. For reference, a
    # properly formed expansions.write call looks like this:
    #   command: expansions.write
    #   params:
    #     file: expansions.yml
    #     redacted: true
    # A function containing the above as a dict definition is treated as
    # equivalent to calling expansions.write
    #
    # 5. Furthermore, above mentions of subprocess.exec MUST be applied only to
    # subprocess.exec invocations that call scripts in the evergreen directory.
    #
    # 6. And because that's not complicated enough, any functions that are
    # dict-defined with subprocess.exec must be treated as equivalent to
    # subprocess.exec on its own.
    # 7. Functions that call expansions.update, and are dict-defined MUST
    # require an expansions.update call after they are called, regardless of
    # syntax
    # 8. timeout.update can also affect expansion values, and all of the rules
    # above need to applied equally to timeout.update

    # These are functions that invoke expansions.write in a dict defintion,
    # as described in Resolution 4.
    expansions_write_fns: Set[str] = set()
    # These are functions whose invocations must be checked for Resolution 1.
    subprocess_exec_fns: Set[str] = set()
    # And these are for Resolution 7
    expansions_update_fns: Set[str] = set()
    # and Resolution 8
    timeout_update_fns: Set[str] = set()

    def _out_message_dangerous_function(context: str) -> LintError:
        return f"{context} cannot safely take arguments. Call expansions.write with params: file: expansions.yml; redacted: true, (or use one of these functions: {list(expansions_write_fns)}) in the function, or do not pass arguments to it."

    def _out_message_expansions_update(context: str, fname: str = "expansions.update") -> LintError:
        return f"{context} is an {fname} command that is not immediately followed by an expansions.write call. Always call expansions.write with params: file: expansions.yml; redacted: true, (or use one of these functions: {list(expansions_write_fns)}) after calling {fname}."

    def _out_message_subprocess(context: str) -> LintError:
        return f"{context} calls an evergreen shell script without a preceding expansions.write call. Always call expansions.write with params: file: expansions.yml; redacted: true, (or use one of these functions: {list(expansions_write_fns)}) before calling an evergreen shell script via subprocess.exec."

    def _is_expansions_write_or_fn(command: dict) -> bool:
        return helpers.match_expansions_write(command) or ("func" in command and
                                                           command["func"] in expansions_write_fns)

    def _is_subprocess_exec_or_fn(command: dict) -> bool:
        return helpers.match_subprocess_exec(command) or ("func" in command and
                                                          command["func"] in subprocess_exec_fns)

    def _is_expansions_update_or_fn(command: dict) -> bool:
        return helpers.match_expansions_update(command) or (
            "func" in command and command["func"] in expansions_update_fns)

    def _is_timeout_update_or_fn(command: dict) -> bool:
        return helpers.match_timeout_update(command) or ("func" in command
                                                         and command["func"] in timeout_update_fns)

    def _context_add_fn(context: str, command: Optional[dict]) -> str:
        if command and "func" in command:
            return f'{context}, (function call: {command["func"]})'

        return context

    def _check_command_list(context: str, commands: List[dict]) -> List[LintError]:
        out: List[LintError] = []
        first_subprocess: int = None
        first_subprocess_cmd: dict = None
        first_exp_write: int = None
        warned_subprocess = False
        for idx, command in enumerate(commands):
            if first_subprocess is None and _is_subprocess_exec_or_fn(command):
                first_subprocess = idx
                first_subprocess_cmd = command

            elif first_exp_write is None and _is_expansions_write_or_fn(command):
                first_exp_write = idx

            if not warned_subprocess and _is_subprocess_exec_or_fn(
                    command) and first_exp_write is None:
                out.append(
                    _out_message_subprocess(_context_add_fn(f"{context}, command {idx}", command)))
                # only warn for the first instance of this per command list.
                # Once you resolve the first instance, the Resolution 3 and
                # Resolution 8 checks below handle the rest of the errors.
                warned_subprocess = True

            if (_is_expansions_update_or_fn(command) or _is_timeout_update_or_fn(command)):
                # Resolution 3 and Resolution 8
                if len(commands) <= idx + 1 or not _is_expansions_write_or_fn(commands[idx + 1]):
                    if _is_expansions_update_or_fn(command):
                        out.append(
                            _out_message_expansions_update(
                                _context_add_fn(f"{context}, command {idx}", command)))
                    elif _is_timeout_update_or_fn(command):
                        out.append(
                            _out_message_expansions_update(
                                _context_add_fn(f"{context}, command {idx}", command),
                                "timeout.update"))

        if not warned_subprocess and first_subprocess is not None and first_exp_write is not None and first_subprocess < first_exp_write:
            out.append(
                _out_message_subprocess(
                    _context_add_fn(f"{context}, command {first_subprocess}",
                                    first_subprocess_cmd)))

        return out

    out: List[LintError] = []
    if "functions" in yaml:
        for fname, body in yaml["functions"].items():
            if isinstance(body, dict):
                # assemble the list of functions whose bodies are the expected
                # expansions.write call. (Resolution 4)
                if helpers.match_expansions_write(body):
                    expansions_write_fns.add(fname)
                # assemble the list of functions that must never be called
                # with arguments (Resolution 1)
                elif helpers.match_subprocess_exec(body):
                    subprocess_exec_fns.add(fname)
                # Resolution 7
                elif helpers.match_expansions_update(body):
                    expansions_update_fns.add(fname)
                # Resolution 8
                elif helpers.match_timeout_update(body):
                    timeout_update_fns.add(fname)

    for context, commands in iterate_command_lists(yaml):
        if isinstance(commands, dict):
            continue
        out += _check_command_list(context, commands)

    for context, command, _ in iterate_fn_calls_context(yaml):
        if command["func"] in subprocess_exec_fns and "vars" in command and command["vars"]:
            out.append(_out_message_dangerous_function(context))

    return out


RULES: Dict[str, LintRule] = {
    #"invalid-function-name": invalid_function_name,
    # TODO: after SERVER-54315
    #"no-keyval-inc": no_keyval_inc,
    "no-working-dir-on-shell": no_working_dir_on_shell,
    "no-shell-exec": no_shell_exec,
    "no-multiline-expansions-update": no_multiline_expansions_update,
    "invalid-build-parameter": invalid_build_parameter,
    "required-expansions-write": required_expansions_write,
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
