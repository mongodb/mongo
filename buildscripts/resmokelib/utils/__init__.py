"""Helper functions."""

import contextlib
import random
import re
import sys

import yaml

from buildscripts.resmokelib.utils import archival


@contextlib.contextmanager
def open_or_use_stdout(filename):
    """Open the specified file for writing, or returns sys.stdout if filename is "-"."""

    if filename == "-":
        yield sys.stdout
        return

    line_buffered = 1
    try:
        fp = open(filename, "w", line_buffered)
    except IOError:
        print("Could not open file {}".format(filename), file=sys.stderr)
        sys.exit(1)

    try:
        yield fp
    finally:
        fp.close()


def default_if_none(*values):
    """Return the first argument that is not 'None'."""
    for value in values:
        if value is not None:
            return value

    return None


def is_windows():
    """Return True if Windows."""
    return sys.platform.startswith("win32") or sys.platform.startswith("cygwin")


def is_string_list(lst):
    """Return true if 'lst' is a list of strings, and false otherwise."""
    return isinstance(lst, list) and all(isinstance(x, str) for x in lst)


def load_yaml_file(filename):
    """Attempt to read 'filename' as YAML."""
    try:
        with open(filename, "r") as fp:
            return yaml.safe_load(fp)
    except yaml.YAMLError as err:
        raise ValueError("File '%s' contained invalid YAML: %s" % (filename, err))


def dump_yaml_file(value, filename):
    """Attempt to write YAML object to filename."""
    try:
        with open(filename, "w") as fp:
            return yaml.safe_dump(value, fp)
    except yaml.YAMLError as err:
        raise ValueError("Could not write YAML to file '%s': %s" % (filename, err))


def dump_yaml(value):
    """Return 'value' formatted as YAML."""
    # Use block (indented) style for formatting YAML.
    return yaml.safe_dump(value, default_flow_style=False).rstrip()


def load_yaml(value):
    """Attempt to parse 'value' as YAML."""
    try:
        return yaml.safe_load(value)
    except yaml.YAMLError as err:
        raise ValueError("Attempted to parse invalid YAML value '%s': %s" % (value, err))


def get_task_name_without_suffix(task_name, variant_name):
    """Return evergreen task name without suffix added to the generated task.

    Remove evergreen variant name, numerical suffix and underscores between them from evergreen task name.
    Example: "noPassthrough_0_enterprise-rhel-80-64-bit-dynamic-required" -> "noPassthrough"
    """
    task_name = task_name if task_name else ""
    return re.sub(fr"(_[0-9]+)?(_{variant_name})?$", "", task_name)


def pick_catalog_shard_node(config_shard, num_shards):
    """Get config_shard node index or None if no config_shard."""
    if config_shard is None:
        return None

    if config_shard == "any":
        if num_shards is None or num_shards == 0:
            return 0
        return random.randint(0, num_shards - 1)

    config_shard_index = int(config_shard)
    if config_shard_index < 0 or config_shard_index >= num_shards:
        raise ValueError("Config shard value must be in range 0..num_shards-1 or \"any\"")

    return config_shard_index
