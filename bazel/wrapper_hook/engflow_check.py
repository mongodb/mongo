import os
import pathlib
import platform
import sys
import time

REPO_ROOT = str(pathlib.Path(__file__).parent.parent.parent)
sys.path.append(REPO_ROOT)

from bazel.wrapper_hook.wrapper_debug import wrapper_debug


def engflow_auth(args):
    start = time.time()
    from buildscripts.bazel_rules_mongo.engflow_auth.engflow_auth import setup_auth

    args_str = " ".join(args)
    if (
        "--config=local" not in args_str
        and "--config=public-release" not in args_str
        and "--config local" not in args_str
        and "--config public-release" not in args_str
    ):
        if os.environ.get("CI") is None and platform.machine().lower() not in {"ppc64le", "s390x"}:
            setup_auth(verbose=False)
    wrapper_debug(f"engflow auth time: {time.time() - start}")
