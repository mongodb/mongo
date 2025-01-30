#!/usr/bin/env python3
"""Script for validating CODEOWNERS file."""

import os
import subprocess
import sys
from typing import Annotated

import typer

from buildscripts.download_codeowners_validator import download_validator_binary


def get_validator_env() -> dict:
    """Prepare the environment for the codeowners-validator."""
    env = os.environ.copy()

    env.update(
        {
            "REPOSITORY_PATH": ".",
            "CHECKS": "duppatterns,syntax",
            "EXPERIMENTAL_CHECKS": "avoid-shadowing",
            "OWNER_CHECKER_REPOSITORY": "10gen/mongo",
        }
    )
    return env


def run_validator(validator_path: str) -> int:
    """Run the codeowners validation."""
    validator_path = os.path.join(os.path.expanduser(validator_path), "codeowners-validator")
    downloaded_by_current_script = False
    # If we are running in bazel, default the directory to the workspace
    workspace_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY", ".")
    if workspace_dir:
        validator_path = os.path.join(workspace_dir, validator_path)

    if not os.path.isfile(validator_path):
        print(f"Validator not found at {validator_path}, attempting to download...")
        try:
            download_validator_binary(os.path.dirname(validator_path))
            if not os.path.isfile(validator_path):
                print(
                    f"Error: Validator still not found at {validator_path} after download attempt",
                    file=sys.stderr,
                )
                return 1
            downloaded_by_current_script = True
        except Exception as exc:
            print(f"Failed to download validator: {str(exc)}", file=sys.stderr)
            return 1

    print(f"Using validator at: {validator_path}")
    env = get_validator_env()

    try:
        result = subprocess.run(
            [validator_path], env=env, check=True, capture_output=True, text=True
        )
        if result.stdout:
            print(result.stdout)
        return 0
    except subprocess.CalledProcessError as exc:
        if exc.stdout:
            print(exc.stdout, file=sys.stderr)
        if exc.stderr:
            print(exc.stderr, file=sys.stderr)
        return exc.returncode
    except FileNotFoundError:
        print("Error: Failed to run codeowners-validator after installation", file=sys.stderr)
        return 1
    finally:
        if downloaded_by_current_script and os.path.isfile(validator_path):
            os.remove(validator_path)


def main(
    validator_path: Annotated[
        str, typer.Option(help="Path to the codeowners-validator binary")
    ] = "./",
) -> int:
    """Validate CODEOWNERS file using codeowners-validator."""
    return run_validator(validator_path=validator_path)


if __name__ == "__main__":
    typer.run(main)
