import pathlib
import sys

REPO_ROOT = str(pathlib.Path(__file__).parent.parent.parent)
sys.path.append(REPO_ROOT)

from bazel.wrapper_hook.wrapper_debug import wrapper_debug
from bazel.wrapper_hook.wrapper_util import memory_info


def check_resource():
    """Check if user machine is using optimal resources."""
    wrapper_debug(f"Check if user machine is using optimal resources {REPO_ROOT}")
    mem_available = memory_info("MemAvailable")
    wrapper_debug(f"Available memory: {mem_available} GB")

    if mem_available == "Unknown":
        print("Warning: Unable to determine available memory.")
        return

    if float(mem_available) < 6.0:
        print(
            f"Warning: Available memory is low ({mem_available} GB). "
            "For optimal performance, it is recommended to have at least 8 GB of available memory."
        )
