# -*- mode: python; -*-

# Try to keep this module's imports minimal and only
# import python standard modules, because this module
# should be used for finding such external modules or
# missing dependencies.
import os
import pathlib
import subprocess
import sys

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class MissingRequirements(Exception):
    """Raised when verify_requirements() detects missing requirements."""

    pass


def verify_requirements(silent: bool = False, executable=sys.executable):
    """Check if the locked Python deps from `uv.lock` are installed in the
    interpreter pointed at by `executable`. Provides a friendly remediation
    hint when something is missing or stale.

    Raises:
        MissingRequirements if any requirements are missing or out of date.
    """

    def verbose(*args, **kwargs):
        if not silent:
            print(*args, **kwargs)

    fix_hint = (
        f"Detected one or more packages are out of date.\n"
        f"Try running:\n"
        f"    buildscripts/uv_sync.sh -p '{executable}'\n"
    )

    # uv ships its own CLI; if it's not importable from this interpreter, the
    # sync script hasn't been run yet.
    try:
        import uv  # noqa: F401
    except ModuleNotFoundError:
        raise MissingRequirements(fix_hint)

    verbose("Checking required python packages...")

    # `uv sync --check` validates uv's *project environment*, which defaults
    # to `<repo>/.venv` — NOT the environment `executable` lives in (the
    # interpreter merely hosts the uv module). Point uv at `executable`'s
    # own environment via UV_PROJECT_ENVIRONMENT, and anchor project
    # discovery at the repo root so this works regardless of caller cwd.
    if executable == sys.executable:
        env_prefix = sys.prefix
    else:
        env_prefix = subprocess.run(
            [executable, "-c", "import sys; print(sys.prefix)"],
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()

    # `uv sync --check` exits non-zero when that environment doesn't match
    # uv.lock. It does not modify the venv. --all-groups + --no-install-
    # project mirror the install-time invocation in buildscripts/uv_sync.sh.
    try:
        proc = subprocess.run(
            [
                executable,
                "-m",
                "uv",
                "sync",
                "--locked",
                "--all-groups",
                "--no-install-project",
                "--check",
            ],
            check=True,
            text=True,
            capture_output=True,
            errors="backslashreplace",
            cwd=_REPO_ROOT,
            env=dict(os.environ, UV_PROJECT_ENVIRONMENT=env_prefix),
        )
    except subprocess.CalledProcessError as exc:
        print("ERROR: uv package verification failed.")
        print(exc.stdout)
        print(exc.stderr)
        raise MissingRequirements(fix_hint)

    verbose(proc.stdout)
