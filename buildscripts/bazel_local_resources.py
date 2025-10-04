"""
A utility for acquiring exclusive access to named local resources from within a
locally run bazel action.

The resource should be defined in the bazel configuration:
  # 4 possible port blocks, each representing the ports that can be used up until the next block.
  --local_resources=port_block=4
  --define=LOCAL_RESOURCES="port_block=20000,20250,20500,20750"

The target that needs the resource should add the tag:
  tags = ["resources:port_block:1"] # This action needs one port range
And include LOCAL_RESOURCES in the action environment:
  env = {
    "LOCAL_RESOURCES": "$(LOCAL_RESOURCES)",
  }

The action should use `acquire_local_resource` to get a lock on the local resource:
  lock, port_block = acquire_local_resource("port_block")
    # Use port_block, no other action will be using the same one.
  lock.release()
"""

import os
import pathlib
import tempfile
from functools import cache

from filelock import FileLock, Timeout


@cache
def _parse_local_resources() -> dict:
    resources = {}
    for resource in os.environ.get("LOCAL_RESOURCES").split(";"):
        name, values = resource.split("=", 1)
        resources[name] = values.split(",")
    return resources


def acquire_local_resource(resource_name: str) -> (FileLock, str):
    local_resources = _parse_local_resources()
    if resource_name not in local_resources:
        raise Exception(
            f"Resource {resource_name} not found in LOCAL_RESOURCES. LOCAL_RESOURCES are: {local_resources}"
        )

    lock_dir = pathlib.Path(tempfile.gettempdir(), "bazel_local_resources", resource_name)
    lock_dir.mkdir(parents=True, exist_ok=True)

    for resource in local_resources[resource_name]:
        try:
            lock = FileLock(lock_dir / f"{resource}.lock")
            lock.acquire(timeout=0)
            return lock, resource
        except Timeout:
            continue

    raise Exception(f"Could not acquire a lock for resource {resource_name}")
