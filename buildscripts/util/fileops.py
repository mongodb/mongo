"""Utility to support file operations."""
import os
from typing import Dict, Any

import yaml


def create_empty(path):
    """Create an empty file specified by 'path'."""
    with open(path, "w") as file_handle:
        file_handle.write("")


def getmtime(path):
    """Return the modified time of 'path', or 0 if is does not exist."""
    if not os.path.isfile(path):
        return 0
    return os.path.getmtime(path)


def is_empty(path):
    """Return True if 'path' has a zero size."""
    return os.stat(path).st_size == 0


def get_file_handle(path, append_file=False):
    """Open 'path', truncate it if 'append_file' is False, and return file handle."""
    mode = "a+" if append_file else "w"
    return open(path, mode)


def read_yaml_file(path: str) -> Dict[str, Any]:
    """
    Read the yaml file at the given path and return the contents.

    :param path: Path to file to read.
    :return: Contents of given file.
    """
    with open(path) as file_handle:
        return yaml.safe_load(file_handle)
