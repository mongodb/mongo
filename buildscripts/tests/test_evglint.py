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
from buildscripts.evglint.helpers import iterate_commands


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
        gen = iterate_commands(yaml_dict)
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
        gen = iterate_commands(yaml_dict)
        count = 0
        for _ in gen:
            count = count + 1
        self.assertEqual(count, 0)


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
                      "command 0, key-value pair 0 is an expansions.update command a multi-line "
                      "values, which is forbidden. For long-form values, prefer expansions.write.")]
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


class TestShellExecBootstrapsShell(_BaseTestClasses.RuleTest):
    """Test shell-exec-bootstraps-shell."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.func = rules.subprocess_exec_bootstraps_shell
        self.table = [
            {
                'raw_yaml':
                    """
functions:
    "test":
        command: subprocess.exec
        params:
          binary: bash
          args:
            - "src/evergreen/do_something.sh"
          env:
            python: ${python}
            workdir: ${workdir}
tasks:
- name: test
            """, "errors": []
            },
            {
                "raw_yaml":
                    """
functions:
    "test":
        command: subprocess.exec
        params:
          binary: sh
          args:
            - "src/evergreen/do_something.sh"
tasks:
- name: test
            """,
                "errors": [
                    "Function 'test', command is a subprocess.exec command that calls an evergreen shell script through a binary other than bash, which is unsupported.",
                    "Function 'test', command is a subprocess.exec command that calls an evergreen shell script without a correctly set environment. You must set 'params.env.workdir' to '${workdir}'.",
                    "Function 'test', command is a subprocess.exec command that calls an evergreen shell script without a correctly set environment. You must set 'params.env.python' to '${python}'.",
                ]
            },
        ]
