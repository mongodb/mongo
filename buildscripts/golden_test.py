#!/usr/bin/env python3
"""
Utility to interact with golden data test outputs, produced by golden data test framework.

For details on the golden data test framework see: docs/golden_data_test_framework.md.
"""

import json
import os
import pathlib
import re
import sys
import shutil

from subprocess import call, CalledProcessError, check_output, STDOUT, DEVNULL
import click

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.util.fileops import read_yaml_file
# pylint: enable=wrong-import-position

assert sys.version_info >= (3, 8)


class AppError(Exception):
    """Application execution error."""

    pass


class GoldenTestConfig(object):
    """Represents the golden test configuration.

    See: docs/golden_data_test_framework.md#appendix---config-file-reference
    """

    def __init__(self, iterable=(), **kwargs):
        """Initialize the fields."""
        self.__dict__.update(iterable, **kwargs)

    outputRootPattern: str
    diffCmd: str

    @classmethod
    def from_yaml_file(cls, path: str) -> "GoldenTestConfig":
        """Read the golden test configuration from the given file."""
        return cls(**read_yaml_file(path))


class OutputPaths(object):
    """Represents actual and expected output paths."""

    def __init__(self, actual, expected):
        """Initialize the fields."""
        self.actual = actual
        self.expected = expected

    actual: None
    expected: None


def replace_variables(pattern: str, variables: dict) -> str:
    """Replace the mustache-style variables."""
    return re.sub(r"\{\{(\w+)\}\}", lambda match: variables[match.group(1)], pattern)


def get_path_name_regex(pattern: str) -> str:
    """Return the regex pattern for output names."""
    return '[0-9a-f]'.join([re.escape(part) for part in pattern.split('%')])


@click.group()
@click.option('-n', '--dry-run', is_flag=True)
@click.option('-v', '--verbose', is_flag=True)
@click.option('--config', envvar='GOLDEN_TEST_CONFIG_PATH',
              help='Config file path. Also GOLDEN_TEST_CONFIG_PATH environment variable.')
@click.pass_context
def cli(ctx, dry_run, verbose, config):
    """Manage test results from golden data test framework.

    Allows for querying, diffing and accepting the golden data test results.

    \b
    For advanced setup guide see: https://wiki.corp.mongodb.com/display/KERNEL/Golden+Data+test+framework+and+workstation+setup
    """

    ctx.obj = GoldenTestApp(dry_run, verbose, config)


class GoldenTestApp(object):
    """Represents the golden application."""

    verbose: False
    dry_run: False
    config: None
    output_parent_path = None
    output_name_pattern = None
    output_name_regex = None

    def __init__(self, dry_run, verbose, config_path):
        """Initialize the app."""
        self.verbose = verbose
        self.dry_run = dry_run

        self.config = self.load_config(config_path)

        self.output_parent_path = pathlib.Path(self.config.outputRootPattern).parent
        self.output_name_pattern = str(pathlib.Path(self.config.outputRootPattern).name)
        self.output_name_regex = get_path_name_regex(self.output_name_pattern)

    def vprint(self, *args, **kwargs):
        """Verbose print, if enabled."""
        if self.verbose:
            print(*args, file=sys.stderr, **kwargs)

    def call_shell(self, cmd):
        """Call shell command."""
        if not self.dry_run:
            call(cmd, shell=True)
        else:
            print(cmd)

    def get_git_root(self):
        """Return the root for git repo."""
        self.vprint("Querying git repo root")
        repo_root = check_output("git rev-parse --show-toplevel", shell=True, text=True).strip()
        self.vprint(f"Found git repo root: '{repo_root}'")
        return repo_root

    def load_config(self, config_path):
        """Load configuration file."""
        if config_path is None:
            raise "Can't load config. GOLDEN_TEST_CONFIG_PATH envrionment variable is not set"

        self.vprint(f"Loading config from path: '{config_path}'")
        config = GoldenTestConfig.from_yaml_file(config_path)

        if config.outputRootPattern is None:
            raise "Invalid config. outputRootPattern config parameter is not set"

        return config

    def get_output_path(self, output_name):
        """Return the path for given output name."""
        if not re.match(self.output_name_regex, output_name):
            raise AppError(f"Invalid name: '{output_name}'. " +
                           f"Does not match configured pattern: {self.output_name_pattern}")
        output_path = os.path.join(self.output_parent_path, output_name)
        if not os.path.isdir(output_path):
            raise AppError(f"No such directory: '{output_path}'")
        return output_path

    def list_outputs(self):
        """Return names of all available outputs."""
        self.vprint(f"Listing outputs in path: '{self.output_parent_path}' " +
                    f"matching '{self.output_name_pattern}'")

        if not os.path.isdir(self.output_parent_path):
            return []
        return [
            o for o in os.listdir(self.output_parent_path) if re.match(self.output_name_regex, o)
            and os.path.isdir(os.path.join(self.output_parent_path, o))
        ]

    def get_latest_output(self):
        """Return the output name wit most recent created timestamp."""
        latest_name = None
        latest_ctime = None
        self.vprint("Searching for output with latest creation time")
        for output_name in self.list_outputs():
            stat = os.stat(self.get_output_path(output_name))
            if (latest_ctime is None) or (stat.st_ctime > latest_ctime):
                latest_name = output_name
                latest_ctime = stat.st_ctime

        if latest_name is None:
            raise AppError("No outputs found")

        self.vprint(f"Found output with latest creation time: {latest_name} " +
                    f"created at {latest_ctime}")

        return latest_name

    def get_paths(self, output_name):
        """Return actual and expected paths for given output name."""
        output_path = self.get_output_path(output_name)
        return OutputPaths(
            actual=os.path.join(output_path, "actual"), expected=os.path.join(
                output_path, "expected"))

    @cli.command('diff', help='Diff the expected and actual folders of the test output')
    @click.argument('output_name', required=False)
    @click.pass_obj
    def command_diff(self, output_name):
        """Diff the expected and actual folders of the test output."""
        if output_name is None:
            output_name = self.get_latest_output()

        self.vprint(f"Diffing results from output '{output_name}'")

        paths = self.get_paths(output_name)
        diff_cmd = replace_variables(self.config.diffCmd,
                                     {'actual': paths.actual, 'expected': paths.expected})
        self.vprint(f"Running command: '{diff_cmd}'")
        self.call_shell(diff_cmd)

    @cli.command('get-path', help='Get the root folder path of the test output.')
    @click.argument('output_name', required=False)
    @click.pass_obj
    def command_get_path(self, output_name):
        """Get the root folder path of the test output."""
        if output_name is None:
            output_name = self.get_latest_output()

        print(self.get_output_path(output_name))

    @cli.command(
        'accept',
        help='Accept the actual test output and copy it as new golden data to the source repo.')
    @click.argument('output_name', required=False)
    @click.pass_obj
    def command_accept(self, output_name):
        """Accept the actual test output and copy it as new golden data to the source repo."""
        if output_name is None:
            output_name = self.get_latest_output()

        self.vprint(f"Accepting actual results from output '{output_name}'")

        repo_root = self.get_git_root()
        paths = self.get_paths(output_name)

        self.vprint(f"Copying files recursively from '{paths.actual}' to '{repo_root}'")
        if not self.dry_run:
            shutil.copytree(paths.actual, repo_root, dirs_exist_ok=True)

    @cli.command('clean', help='Remove all test outputs')
    @click.pass_obj
    def command_clean(self):
        """Remove all test outputs."""
        outputs = self.list_outputs()
        self.vprint(f"Deleting {len(outputs)} outputs")
        for output_name in outputs:
            output_path = self.get_output_path(output_name)
            self.vprint(f"Deleting folder: '{output_path}'")
            if not self.dry_run:
                shutil.rmtree(output_path)

    @cli.command('latest', help='Get the name of the most recent test output')
    @click.pass_obj
    def command_latest(self):
        """Get the name of the most recent test output."""
        output_name = self.get_latest_output()
        print(output_name)

    @cli.command('list', help='List all names of the available test outputs')
    @click.pass_obj
    def command_list(self):
        """List all names of the available test outputs."""
        for output_name in self.list_outputs():
            print(output_name)


def main():
    """Execute main."""
    try:
        cli()  # pylint: disable=no-value-for-parameter
    except AppError as err:
        print(err)
        sys.exit(1)


if __name__ == "__main__":
    main()
