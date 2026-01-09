#!/usr/bin/env python3
"""YAML linters wrapper script for Bazel."""

import os
import subprocess
import sys


def run_command(cmd, **kwargs):
    """Run a command and exit on failure."""
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        sys.exit(result.returncode)


def main():
    # Change to workspace root if running under Bazel
    if "BUILD_WORKING_DIRECTORY" in os.environ:
        os.chdir(os.environ["BUILD_WORKING_DIRECTORY"])
    else:
        # Change to repo root (one level up from buildscripts)
        script_dir = os.path.dirname(os.path.abspath(__file__))
        os.chdir(os.path.join(script_dir, ".."))

    # Run yamllint as a Python module
    run_command(
        [
            sys.executable,
            "-m",
            "yamllint",
            "-c",
            "etc/yamllint_config.yml",
            "buildscripts",
            "etc",
            "jstests",
        ],
        shell=False,
    )

    # Evaluate evergreen configs
    # Set up environment with extended PATH for evergreen command
    env = os.environ.copy()
    home_dir = env.get("HOME", "")

    evergreen_cmd_base = [f"{home_dir}/evergreen", "evaluate"]

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
    run_command(
        [sys.executable, "-m", "evergreen_lint", "-c", "./etc/evergreen_lint.yml", "lint"],
        shell=False,
    )

    print("YAML linting completed successfully!")


if __name__ == "__main__":
    main()
