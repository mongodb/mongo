# -*- mode: python; -*-

# Try to keep this modules imports minimum and only
# import python standard modules, because this module
# should be used for finding such external modules or
# missing dependencies.
import sys
import subprocess
import re


class MissingRequirements(Exception):
    """Raised when when verify_requirements() detects missing requirements."""
    pass


def verify_requirements(silent: bool = False, executable=sys.executable):
    """Check if the modules in a pip requirements file are installed.
    This allows for a more friendly user message with guidance on how to
    resolve the missing dependencies.
    Args:
        requirements_file: path to a pip requirements file.
        silent: True if the function should print.
    Raises:
        MissingRequirements if any requirements are missing
    """

    def verbose(*args, **kwargs):
        if not silent:
            print(*args, **kwargs)

    def raiseSuggestion(ex, pip_pkg):
        raise MissingRequirements(f"{ex}\n"
                                  f"Try running:\n"
                                  f"    {executable} -m pip install {pip_pkg}") from ex

    # Import poetry. If this fails then we know the next function will fail.
    # This is so the user will have an easier time diagnosing the problem
    try:
        import poetry
    except ModuleNotFoundError as ex:
        raiseSuggestion(ex, "'poetry==1.5.1'")

    verbose("Checking required python packages...")

    try:
        poetry_dry_run_proc = subprocess.run(
            [executable, "-m", "poetry", "install", "--no-root", "--sync", "--dry-run"], check=True,
            text=True, capture_output=True, errors='backslashreplace')
    except subprocess.CalledProcessError as exc:
        print("ERROR: poetry packages verification failed.")
        print(exc.stdout)
        print(exc.stderr)
        raise MissingRequirements(
            f"Detected one or more packages are out of date. "
            f"Try running:\n"
            f"    export PYTHON_KEYRING_BACKEND=keyring.backends.null.Keyring\n"
            f"    python3 -m poetry install --no-root --sync")

    # String match should look like the following
    # Package operations: 2 installs, 3 updates, 0 removals, 165 skipped
    match = re.search(r"Package operations: (\d+) \w+, (\d+) \w+, (\d+) \w+, (\d+) \w+",
                      poetry_dry_run_proc.stdout)
    verbose("Requirements list:")
    verbose(poetry_dry_run_proc.stdout)
    installs = int(match[1])
    updates = int(match[2])
    removals = int(match[3])
    if updates == 1 and sys.platform == 'win32' and "Updating pywin32" in poetry_dry_run_proc.stdout:
        # We have no idea why pywin32 thinks it needs to be updated
        # We could use some more investigation into this
        verbose(
            "Windows detected a single update to pywin32 which is known to be buggy. Continuing.")
    elif installs + updates + removals > 0:
        raise MissingRequirements(
            f"Detected one or more packages are out of date. "
            f"Try running:\n"
            f"    export PYTHON_KEYRING_BACKEND=keyring.backends.null.Keyring\n"
            f"    {executable} -m poetry install --no-root --sync")
