import os
import pathlib
import platform
import sys
import time

REPO_ROOT = str(pathlib.Path(__file__).parent.parent.parent)
sys.path.append(REPO_ROOT)

from bazel.wrapper_hook.wrapper_debug import wrapper_debug
from bazel.wrapper_hook.wrapper_util import get_terminal_stream


def setup_auth_wrapper():
    from buildscripts.bazel_rules_mongo.engflow_auth.engflow_auth import setup_auth

    term_out = get_terminal_stream("MONGO_WRAPPER_STDOUT_FD")
    term_err = get_terminal_stream("MONGO_WRAPPER_STDERR_FD")

    # Save current stdout/stderr
    old_stdout = sys.stdout
    old_stderr = sys.stderr

    try:
        if term_out:
            sys.stdout = term_out
        if term_err:
            sys.stderr = term_err

        setup_auth(verbose=False)

    finally:
        # Restore original stdout/stderr to whatever wrapper has
        sys.stdout = old_stdout
        sys.stderr = old_stderr


def engflow_auth(args):
    start = time.time()

    args_str = " ".join(args)
    if (
        "--config=local" not in args_str
        and "--config=public-release" not in args_str
        and "--config local" not in args_str
        and "--config public-release" not in args_str
    ):
        if os.environ.get("CI") is None and platform.machine().lower() not in {"ppc64le", "s390x"}:
            setup_auth_wrapper()
    wrapper_debug(f"engflow auth time: {time.time() - start}")
