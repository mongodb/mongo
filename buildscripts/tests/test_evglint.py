"""evglint tests."""
import unittest
from io import StringIO
from unittest.mock import MagicMock, patch
from typing import List

import yaml
from typing_extensions import TypedDict

from buildscripts.evglint.yamlhandler import load
from buildscripts.evglint import rules
from buildscripts.evglint.model import LintRule, LintError
import buildscripts.evglint.helpers as h


class TestRulebreaker(unittest.TestCase):
    """Attempt to raise exceptions in evglint rules."""

    # the Evergreen YAML freely allows for lists of dicts or just a single
    # dict for commands, which can painfully lead to exceptions.
    # Additionally, the rule author cannot safely assume that any parameters
    # are defined, so we generate an even larger list of parameter-less
    # commands that will raise exceptions in any rule.
    RULEBREAKER = """
    functions:
      "single command": &a1
        # this is, surprisingly, a valid evergreen command
        command: shell.exec

      "list of commands": &a2
        - command: shell.exec
          params:
            script: /bin/true
        - command: shell.exec
          params:
            script: /bin/true

      "deliberately empty kv pair":
      "inject here":
{inject_here}
      "anchor cheese":
        - *a1
        - *a1

    timeout:
      - *a1
    pre:
      - *a1
    post:
      - *a1
    tasks:
    - name: empty
    - name: clang_tidy
      setup_task:
        - *a1
      teardown_task:
        - *a1
      teardown_group:
        - *a1
      setup_group:
        - *a1
      timeout:
        - *a1

      commands:
        - func: "single command"
        - func: "anchor cheese"
        - command: shell.exec
    """

    @classmethod
    def _gen_rule_breaker(cls) -> dict:
        # List from https://github.com/evergreen-ci/evergreen/wiki/Project-Commands
        commands = [
            "keyval.inc",
            "archive.targz_extract",
            "archive.targz_pack",
            "attach.artifacts",
            "attach.results",
            "attach.xunit_results",
            "expansions.update",
            "expansions.write",
            "generate.tasks",
            "git.get_project",
            "gotest.parse_files",
            "host.create",
            "host.list",
            "json.send",
            "manifest.load",
            "perf.send",
            "s3.get",
            "s3.put",
            "s3.push",
            "s3.pull",
            "s3Copy.copy",
            "shell.exec",
            "subprocess.exec",
            "subprocess.scripting",
            "timeout.update",
        ]
        buf = StringIO()
        for cmd in commands:
            buf.write(f"        - command: {cmd}\n")

        gen_commands = TestRulebreaker.RULEBREAKER.format(inject_here=buf.getvalue())
        return load(gen_commands)

    def test_break_rules(self):
        """test that rules don't raise exceptions."""
        yaml_dict = self._gen_rule_breaker()
        for rule_name, rule in rules.RULES.items():
            try:
                rule(yaml_dict)
            except Exception as ex:  # pylint: disable=broad-except
                self.fail(f"{rule_name} raised an exception, but must not. "
                          "The rule is likely accessing a key without "
                          "verifying that it exists first. Write a more "
                          "thorough rule.\n"
                          f"Exception: {ex}")


class TestHelpers(unittest.TestCase):
    """Test .helpers module."""

    def test_iterate_commands(self):
        """test iterate_commands."""
        yaml_dict = load(TestRulebreaker.RULEBREAKER.format(inject_here=""))
        gen = h.iterate_commands(yaml_dict)
        count = 0
        for _ in gen:
            count = count + 1
        self.assertEqual(count, 14)

    I_CANT_BELIEVE_THAT_VALIDATES = """
tasks:
- name: test
    """

    def test_iterate_commands_no_commands(self):
        """Test iterate_commands when the yaml has no commands."""
        yaml_dict = load(TestHelpers.I_CANT_BELIEVE_THAT_VALIDATES)
        gen = h.iterate_commands(yaml_dict)
        count = 0
        for _ in gen:
            count = count + 1
        self.assertEqual(count, 0)

    def test_iterate_command_lists(self):
        """test iterate_command_lists."""
        yaml_dict = load(TestRulebreaker.RULEBREAKER.format(inject_here=""))
        gen = h.iterate_command_lists(yaml_dict)
        count = 0
        for _ in gen:
            count = count + 1
        self.assertEqual(count, 12)

    def test_iterate_command_lists_no_commands(self):
        """Test iterate_command_lists when the yaml has no commands."""
        yaml_dict = load(TestHelpers.I_CANT_BELIEVE_THAT_VALIDATES)
        gen = h.iterate_command_lists(yaml_dict)
        count = 0
        for _ in gen:
            count = count + 1
        self.assertEqual(count, 0)

    def test_match_expansions_write(self):
        """Test match_expansions_write."""
        cmd = {}
        self.assertFalse(h.match_expansions_write(cmd))
        cmd = {
            "command": "expansions.write", "params": {"file": "expansions.yml", "redacted": True}
        }
        self.assertTrue(h.match_expansions_write(cmd))

    def test_iterate_fn_calls_context(self):
        """Test iterate_fn_calls_context."""
        yaml_dict = load(TestRulebreaker.RULEBREAKER.format(inject_here=""))
        gen = h.iterate_fn_calls_context(yaml_dict)
        count = 0
        for _ in gen:
            count = count + 1
        self.assertEqual(count, 2)

    def test_match_subprocess_exec(self):
        """Test match_subprocess_exec."""
        cmd = {}
        self.assertFalse(h.match_subprocess_exec(cmd))
        cmd = {
            "command": "subprocess.exec",
            "params": {"binary": "bash", "args": ["./src/evergreen/something.sh"]}
        }
        self.assertTrue(h.match_subprocess_exec(cmd))


class _RuleExpect(TypedDict):
    raw_yaml: str
    errors: List[LintError]


class _BaseTestClasses:
    # this extra class prevents unittest from running the base class as a test
    # suite

    class RuleTest(unittest.TestCase):
        """Test a rule."""

        @staticmethod
        def _whine(_: dict) -> LintRule:
            raise RuntimeError("Programmer error: func was not set")

        def __init__(self, *args, **kwargs):
            self.table: List[_RuleExpect] = []
            self.func: LintRule = self._whine
            super().__init__(*args, **kwargs)
            self.maxDiff = None  # pylint: disable=invalid-name

        def test_rule(self):
            """Test self.func with the yamls listed in self.table, and compare results."""

            for expectation in self.table:
                yaml_dict = load(expectation["raw_yaml"])
                errors = self.func(yaml_dict)
                # a discrepancy on this assert means that your rule isn't working
                # as expected
                self.assertListEqual(errors, expectation["errors"])


class TestNoKeyvalInc(_BaseTestClasses.RuleTest):
    """Test no-keyval-inc."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.func = rules.no_keyval_inc
        self.table = [
            {
                "raw_yaml":
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: shell.exec
tasks:
- name: test
        """, "errors": []
            },
            {
                'raw_yaml':
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: keyval.inc
tasks:
- name: test
            """,
                "errors": [
                    "Function 'cat i'm a kitty cat, and i test test test and i test test test', command 0 includes keyval.inc, which is not permitted. Do not use keyval.inc."
                ]
            },
        ]


class TestShellExecExplicitShell(_BaseTestClasses.RuleTest):
    """Test shell-exec-explicit-shell."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.func = rules.shell_exec_explicit_shell
        self.table = [
            {
                "raw_yaml":
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: shell.exec
        params:
          shell: bash
tasks:
- name: test
            """, "errors": []
            },
            {
                'raw_yaml':
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: shell.exec
tasks:
- name: test
            """,
                "errors": [
                    "Function 'cat i'm a kitty cat, and i test test test and i test test test', command 0 is a shell.exec command without an explicitly declared shell. You almost certainly want to add 'shell: bash' to the parameters list."
                ]
            },
        ]


class TestNoWorkingDirOnShell(_BaseTestClasses.RuleTest):
    """Test no-working-dir-on-shell."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.func = rules.no_working_dir_on_shell
        self.table = [
            {
                "raw_yaml":
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: subprocess.exec
tasks:
- name: test
            """, "errors": []
            },
            {
                'raw_yaml':
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: shell.exec
        params:
            working_dir: somewhere
tasks:
- name: test
            """,
                "errors": [(
                    "Function 'cat i'm a kitty cat, and i test test test and i test test test', command 0 is a shell.exec command with a working_dir parameter. Do not set working_dir, instead `cd` into the directory in the shell script."
                )]
            },
        ]


class TestInvalidFunctionName(_BaseTestClasses.RuleTest):
    """Test invalid-function-name."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.func = rules.invalid_function_name
        self.table = [
            {
                'raw_yaml':
                    """
functions:
    "f_cat_im_a_kitty_cat_and_i_test_test_test_and_i_test_test_test":
      - command: shell.exec
tasks:
- name: test
            """, "errors": []
            },
            {
                "raw_yaml":
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
        - command: subprocess.exec
tasks:
- name: test
            """,
                "errors": [(
                    "Function 'cat i'm a kitty cat, and i test test test and i test test test' must have a name matching '^f_[a-z][A-Za-z0-9_]*'"
                )]
            },
        ]


class TestNoShellExec(_BaseTestClasses.RuleTest):
    """Test no-shell-exec."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.func = rules.no_shell_exec
        self.table = [
            {
                'raw_yaml':
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: subprocess.exec
tasks:
- name: test
            """, "errors": []
            },
            {
                "raw_yaml":
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: shell.exec
tasks:
- name: test
            """,
                "errors": [(
                    "Function 'cat i'm a kitty cat, and i test test test and i test test test', command 0 is a shell.exec command, which is forbidden. Extract your shell script out of the YAML and into a .sh file in directory 'evergreen', and use subprocess.exec instead."
                )]
            },
        ]


class TestNoMultilineExpansionsUpdate(_BaseTestClasses.RuleTest):
    """Test no-multiline-expansions-update."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.func = rules.no_multiline_expansions_update
        self.table = [
            {
                'raw_yaml':
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: expansions.update
        params:
          updates:
          - key: test
            value: a single line value \n
tasks:
- name: test
            """, "errors": []
            },
            {
                "raw_yaml":
                    """
functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: expansions.update
        params:
          updates:
          - key: test
            value: |
              a
              multiline
              value

tasks:
- name: test
            """,
                "errors":
                    [("Function 'cat i'm a kitty cat, and i test test test and i test test test', "
                      "command 0, key-value pair 0 is an expansions.update command with multi-line "
                      "values embedded in the yaml, which is forbidden. For long-form values, use "
                      "the files parameter of expansions.update.")]
            },
        ]


class TestInvalidBuildParameter(_BaseTestClasses.RuleTest):
    """Test invalid-build-parameter."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.func = rules.invalid_build_parameter
        self.table = [
            {
                'raw_yaml':
                    """
parameters:
  - key: num_kitties
    description: "number of kitties"

functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: shell.exec
tasks:
- name: test
            """, "errors": []
            },
            {
                "raw_yaml":
                    """
parameters:
  - key: numberOfKitties
    description: "number of kitties"
  - key: number_of_kitties
  - key: number_of_kitties2
    description: ""

functions:
    "cat i'm a kitty cat, and i test test test and i test test test":
      - command: shell.exec
tasks:
- name: test
            """, "errors": [
                        "Build parameter, pair 0, key must match '[a-z][a-z0-9_]*'.",
                        "Build parameter, pair 1, must have a description.",
                        "Build parameter, pair 2, must have a description."
                    ]
            },
        ]


class TestRequiredExpansionsWrite(_BaseTestClasses.RuleTest):
    """Test required-expansions-write."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.func = rules.required_expansions_write
        self.table = [
            {
                'raw_yaml':
                    """
functions:
    # this function can serve in lieu of an expansions.write call
    "f_expansions_write": &f_expansions_write
        command: expansions.write
        params:
          file: expansions.yml
          redacted: true

    # this function cannot, because redacted is not True
    "f_expansions_write2": &f_expansions_write2
        command: expansions.write
        params:
          file: expansions.yml

    "dangerous_fn": &dangerous_fn
        # will not generate errors because this is a dict defintion. Errors
        # will be generated if this function is called with arguments
        command: subprocess.exec
        params:
          binary: bash
          args:
            - "src/evergreen/do_something.sh"

    "dangerous_fn2": &dangerous_fn2
        # will not generate errors because this is a dict defintion.
        command: expansions.update

    "test1":
        # needs expansions.write
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "src/evergreen/do_something.sh"
        # only ONE of these should generate an error
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "src/evergreen/do_something.sh"
    "test2a":
        # correct
        - func: "f_expansions_write"
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "src/evergreen/do_something.sh"
    "test2b":
        # correct
        - *f_expansions_write
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "src/evergreen/do_something.sh"
    "test2c":
        # function isn't a compatible substitution
        - *f_expansions_write2
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "src/evergreen/do_something.sh"
    "test3":
        # correct because the subprocess.exec call is a script outside
        # of evergreen
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "somewhere/else/do_something.sh"
    "test3a":
        # needs expansions.write
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "somewhere/else/do_something.sh"
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "src/evergreen/do_something.sh"
    "test4a":
        # need an expansions.write call after expansions.update
        - command: expansions.update
    "test4b":
        # need an expansions.write call after expansions.update
        - command: expansions.update
        - command: shell.exec
        - *f_expansions_write
    "test4c":
        # no errors
        - command: expansions.update
        - *f_expansions_write
    "test4d":
        # errors, because an incompatible function is called
        - *f_expansions_write2
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "src/evergreen/do_something.sh"
    "test5":
        # errors, because an incompatible function is called
        - command: expansions.update
        - func: f_expansions_write2

    "test6":
        # error because this needs an expansions write call after dangerous_fn2
        - *f_expansions_write
        - func: "dangerous_fn2"
          vars:
            test: test

    "test7":
        # error, needs an expansion.write at the end
        - command: expansions.update
        - *f_expansions_write
        - command: timeout.update

    "test8":
        # no errors
        - command: expansions.update
        - *f_expansions_write
        - command: timeout.update
        - *f_expansions_write
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "somewhere/else/do_something.sh"
        - command: timeout.update
        - *f_expansions_write
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "src/evergreen/do_something.sh"

    "test8b":
        - command: expansions.update
        - command: timeout.update
        - *f_expansions_write
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "somewhere/else/do_something.sh"
        - command: timeout.update
        - command: subprocess.exec
          params:
            binary: bash
            args:
              - "src/evergreen/do_something.sh"

tasks:
- name: test
  commands:
    # need an expansions.write call here
    - command: shell.exec
    - command: subprocess.exec
      params:
        binary: bash
        args:
          - "src/evergreen/do_something.sh"
- name: test1
  commands:
    - func: "dangerous_fn"
      vars:
        test: true

- name: test2
  commands:
  # need an expansions.write call after expansions.update
    - command: expansions.update
    - command: shell.exec
    - command: subprocess.exec
      params:
        binary: bash
        args:
          - "src/evergreen/do_something.sh"
- name: test3
  commands:
    # an expansions.write call is required here.
    - func: dangerous_fn
        """,
                "errors": [
                    "Function 'test1', command 0 calls an evergreen shell script without a "
                    'preceding expansions.write call. Always call expansions.write with params: '
                    'file: expansions.yml; redacted: true, (or use one of these functions: '
                    "['f_expansions_write']) before calling an evergreen shell script via "
                    'subprocess.exec.',
                    "Function 'test2c', command 1 calls an evergreen shell script without a "
                    'preceding expansions.write call. Always call expansions.write with params: '
                    'file: expansions.yml; redacted: true, (or use one of these functions: '
                    "['f_expansions_write']) before calling an evergreen shell script via "
                    'subprocess.exec.',
                    "Function 'test3a', command 1 calls an evergreen shell script without a "
                    'preceding expansions.write call. Always call expansions.write with params: '
                    'file: expansions.yml; redacted: true, (or use one of these functions: '
                    "['f_expansions_write']) before calling an evergreen shell script via "
                    'subprocess.exec.',
                    "Function 'test4a', command 0 is an expansions.update command that is not "
                    'immediately followed by an expansions.write call. Always call '
                    'expansions.write with params: file: expansions.yml; redacted: true, (or use '
                    "one of these functions: ['f_expansions_write']) after calling "
                    'expansions.update.',
                    "Function 'test4b', command 0 is an expansions.update command that is not "
                    'immediately followed by an expansions.write call. Always call '
                    'expansions.write with params: file: expansions.yml; redacted: true, (or use '
                    "one of these functions: ['f_expansions_write']) after calling "
                    'expansions.update.',
                    "Function 'test4d', command 1 calls an evergreen shell script without a "
                    'preceding expansions.write call. Always call expansions.write with params: '
                    'file: expansions.yml; redacted: true, (or use one of these functions: '
                    "['f_expansions_write']) before calling an evergreen shell script via "
                    'subprocess.exec.',
                    "Function 'test5', command 0 is an expansions.update command that is not "
                    'immediately followed by an expansions.write call. Always call '
                    'expansions.write with params: file: expansions.yml; redacted: true, (or use '
                    "one of these functions: ['f_expansions_write']) after calling "
                    'expansions.update.',
                    "Function 'test6', command 1, (function call: dangerous_fn2) is an "
                    'expansions.update command that is not immediately followed by an '
                    'expansions.write call. Always call expansions.write with params: file: '
                    'expansions.yml; redacted: true, (or use one of these functions: '
                    "['f_expansions_write']) after calling expansions.update.",
                    "Function 'test7', command 2 is an timeout.update command that is not "
                    'immediately followed by an expansions.write call. Always call '
                    'expansions.write with params: file: expansions.yml; redacted: true, (or use '
                    "one of these functions: ['f_expansions_write']) after calling "
                    'timeout.update.',
                    "Function 'test8b', command 0 is an expansions.update command that is not "
                    'immediately followed by an expansions.write call. Always call '
                    'expansions.write with params: file: expansions.yml; redacted: true, (or use '
                    "one of these functions: ['f_expansions_write']) after calling "
                    'expansions.update.',
                    "Function 'test8b', command 4 is an timeout.update command that is not "
                    'immediately followed by an expansions.write call. Always call '
                    'expansions.write with params: file: expansions.yml; redacted: true, (or use '
                    "one of these functions: ['f_expansions_write']) after calling "
                    'timeout.update.',
                    "Task 'test', command 1 calls an evergreen shell script without a preceding "
                    'expansions.write call. Always call expansions.write with params: file: '
                    'expansions.yml; redacted: true, (or use one of these functions: '
                    "['f_expansions_write']) before calling an evergreen shell script via "
                    'subprocess.exec.',
                    "Task 'test1', command 0, (function call: dangerous_fn) calls an evergreen "
                    'shell script without a preceding expansions.write call. Always call '
                    'expansions.write with params: file: expansions.yml; redacted: true, (or use '
                    "one of these functions: ['f_expansions_write']) before calling an evergreen "
                    'shell script via subprocess.exec.',
                    "Task 'test2', command 0 is an expansions.update command that is not "
                    'immediately followed by an expansions.write call. Always call '
                    'expansions.write with params: file: expansions.yml; redacted: true, (or use '
                    "one of these functions: ['f_expansions_write']) after calling "
                    'expansions.update.',
                    "Task 'test2', command 2 calls an evergreen shell script without a preceding "
                    'expansions.write call. Always call expansions.write with params: file: '
                    'expansions.yml; redacted: true, (or use one of these functions: '
                    "['f_expansions_write']) before calling an evergreen shell script via "
                    'subprocess.exec.',
                    "Task 'test3', command 0, (function call: dangerous_fn) calls an evergreen "
                    'shell script without a preceding expansions.write call. Always call '
                    'expansions.write with params: file: expansions.yml; redacted: true, (or use '
                    "one of these functions: ['f_expansions_write']) before calling an evergreen "
                    'shell script via subprocess.exec.',
                    "Task 'test1', command 0 (function call: 'dangerous_fn') cannot safely take "
                    'arguments. Call expansions.write with params: file: expansions.yml; '
                    "redacted: true, (or use one of these functions: ['f_expansions_write']) in "
                    'the function, or do not pass arguments to it.'
                ]
            },
        ]
