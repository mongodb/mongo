"""Helper functions."""

from __future__ import absolute_import
from __future__ import print_function

import contextlib
import os.path
import shutil
import sys

import yaml

from . import archival


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


def default_if_none(value, default):
    """Set default if value is 'None'."""
    return value if value is not None else default


def rmtree(path, **kwargs):
    """Wrap shutil.rmtreee.

    Use a UTF-8 unicode path if Windows.
    See https://bugs.python.org/issue24672, where shutil.rmtree can fail with UTF-8.
    Use a bytes path to rmtree, otherwise.
    See https://github.com/pypa/setuptools/issues/706.
    """
    if is_windows():
        if not isinstance(path, unicode):
            path = unicode(path, "utf-8")
    else:
        if isinstance(path, unicode):
            path = path.encode("utf-8")
    shutil.rmtree(path, **kwargs)


def is_windows():
    """Return True if Windows."""
    return sys.platform.startswith("win32") or sys.platform.startswith("cygwin")


def remove_if_exists(path):
    """Remove path if it exists."""
    if path is not None and os.path.exists(path):
        try:
            os.remove(path)
        except OSError:
            pass


def is_string_list(lst):
    """Return true if 'lst' is a list of strings, and false otherwise."""
    return isinstance(lst, list) and all(isinstance(x, basestring) for x in lst)


def is_string_set(value):
    """Return true if 'value' is a set of strings, and false otherwise."""
    return isinstance(value, set) and all(isinstance(x, basestring) for x in value)


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
