#!/usr/bin/env python3
"""Script for validating CODEOWNERS file."""

import os
import subprocess
import sys


def get_validator_env() -> dict:
    """Prepare the environment for the codeowners-validator."""
    env = os.environ.copy()

    env.update(
        {
            "REPOSITORY_PATH": ".",
            "CHECKS": "duppatterns,syntax",
            "EXPERIMENTAL_CHECKS": "avoid-shadowing",
        }
    )
    return env


def run_validator(validator_path: str) -> int:
    """Run the codeowners validation."""

    if not os.path.isfile(validator_path):
        raise RuntimeError(f"Validator was not found at input path: {validator_path}")

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
    except Exception:
        raise
