"""Helper functions."""

import contextlib
import errno
import os.path
import re
import shutil
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


def remove_if_exists(path):
    """Remove path if it exists."""
    try:
        os.remove(path)
    except OSError:
        pass


def is_string_list(lst):
    """Return true if 'lst' is a list of strings, and false otherwise."""
    return isinstance(lst, list) and all(isinstance(x, str) for x in lst)


def is_string_set(value):
    """Return true if 'value' is a set of strings, and false otherwise."""
    return isinstance(value, set) and all(isinstance(x, str) for x in value)


def is_js_file(filename):
    """Return true if 'filename' ends in .js, and false otherwise."""
    return os.path.splitext(filename)[1] == ".js"


def is_yaml_file(filename):
    """Return true if 'filename' ends in .yml or .yaml, and false otherwise."""
    return os.path.splitext(filename)[1] in (".yaml", ".yml")


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


def mkdir_p(path):
    """
    Make the directory and all missing parents (like mkdir -p).

    :type path: string the directory path
    """
    try:
        os.makedirs(path)
    except OSError as exc:  # Python >2.5
        if exc.errno == errno.EEXIST and os.path.isdir(path):
            pass
        else:
            raise


def get_task_name_without_suffix(task_name, variant_name):
    """Return evergreen task name without suffix added to the generated task.

    Remove evergreen variant name, numerical suffix and underscores between them from evergreen task name.
    Example: "noPassthrough_0_enterprise-rhel-80-64-bit-dynamic-required" -> "noPassthrough"
    """
    task_name = task_name if task_name else ""
    return re.sub(fr"(_[0-9]+)?(_{variant_name})?$", "", task_name)
