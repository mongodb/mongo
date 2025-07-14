import os
import pathlib
import sys
import time
from typing import Dict

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

from bazel.wrapper_hook.wrapper_debug import wrapper_debug
from tools.flag_sync.util import get_flags

# Allowed .bazelrc lines. Attempt to remove flag setting as attack vector.
ALLOW_LINES = [
    "common --config=local",
    "--experimental_throttle_remote_action_building",
    "--noexperimental_throttle_remote_action_building",
]


def update_bazelrc(flags: Dict[str, Dict], verbose: bool):
    bazelrc_path = f"{REPO_ROOT}/.bazelrc.sync"
    if verbose:
        print(f"Updating {bazelrc_path}")

    enabled = set()
    for flag in flags.values():
        if flag["enabled"]:
            enabled.add(flag["value"])

    changed = True
    if os.path.exists(bazelrc_path):
        with open(bazelrc_path, "r") as bazelrc:
            lines = bazelrc.readlines()
            bazelrc = set([l.strip() for l in lines])
            changed = bazelrc != enabled

    if not changed:
        return

    with open(bazelrc_path, "w+") as bazelrc:
        for line in enabled:
            if line in ALLOW_LINES:
                bazelrc.write(line + "\n")
            else:
                print("Tried to write unallowed line. Skipping...")


def sync_and_update(namespace: str):
    flags = get_flags(namespace)
    update_bazelrc(flags, False)


def sync_flags(namespace: str) -> bool:
    start = time.time()
    try:
        sync_and_update(namespace)
    except Exception:
        print("Failed to sync bazel flags. Skipping...")
        return False
    wrapper_debug(f"flag sync time: {time.time() - start}")
    return True
