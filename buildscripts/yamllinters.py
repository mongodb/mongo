#!/usr/bin/env python3
"""YAML linters wrapper script for Bazel."""

import os
import runpy
import shutil
import subprocess
import sys


def run_command(cmd, **kwargs):
    """Run a command and exit on failure."""
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        sys.exit(result.returncode)


def run_module(module_name, args):
    old_argv = sys.argv
    old_cwd = os.getcwd()
    try:
        sys.argv = [module_name] + args
        runpy.run_module(module_name, run_name="__main__")
    except SystemExit as e:
        if e.code not in (None, 0):
            sys.exit(e.code)
    finally:
        sys.argv = old_argv
        os.chdir(old_cwd)


def main():
    # Change to workspace root if running under Bazel
    if "BUILD_WORKING_DIRECTORY" in os.environ:
        os.chdir(os.environ["BUILD_WORKING_DIRECTORY"])
    else:
        # Change to repo root (one level up from buildscripts)
        script_dir = os.path.dirname(os.path.abspath(__file__))
        os.chdir(os.path.join(script_dir, ".."))

    run_module(
        "yamllint",
        ["-c", "etc/yamllint_config.yml", "buildscripts", "etc", "jstests"],
    )

    # Evaluate evergreen configs
    # Set up environment with extended PATH for evergreen command
    env = os.environ.copy()

    evergreen_bin = shutil.which("evergreen") or os.path.join(env.get("HOME", ""), "evergreen")
    evergreen_cmd_base = [evergreen_bin, "evaluate"]

    run_command(
        evergreen_cmd_base + ["etc/evergreen.yml"],
        stdout=open("etc/evaluated_evergreen.yml", "w"),
        env=env,
    )

    run_command(
        evergreen_cmd_base + ["etc/evergreen_nightly.yml"],
        stdout=open("etc/evaluated_evergreen_nightly.yml", "w"),
        env=env,
    )

    # Process system_perf.yml
    # Remove references to the DSI repo before evergreen evaluate.
    # The DSI module references break 'evaluate', the system_perf config should
    # parse without them, and we don't want changes to the DSI repository to
    # break checking that the rest of the imports etc. work.
    with (
        open("etc/system_perf.yml", "r") as infile,
        open("etc/trimmed_system_perf.yml", "w") as outfile,
    ):
        drop = False
        for line in infile:
            if "lint_yaml trim start" in line:
                drop = True
            if "lint_yaml trim end" in line:
                drop = False
            if not drop:
                outfile.write(line)

    run_command(
        evergreen_cmd_base + ["etc/trimmed_system_perf.yml"],
        stdout=open("etc/evaluated_system_perf.yml", "w"),
        env=env,
    )

    # Run evergreen-lint using module invocation
    run_module("evergreen_lint", ["-c", "./etc/evergreen_lint.yml", "lint"])

    print("YAML linting completed successfully!")


if __name__ == "__main__":
    main()
