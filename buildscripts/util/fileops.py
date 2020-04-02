"""Utility to support file operations."""

import os


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


def write_file(path: str, contents: str) -> None:
    """
    Write the contents provided to the file in the specified path.

    :param path: Path of file to write.
    :param contents: Contents to write to file.
    """
    with open(path, "w") as file_handle:
        file_handle.write(contents)


def write_file_to_dir(directory: str, file: str, contents: str) -> None:
    """
    Write the contents provided to the file in the given directory.

    The directory will be created if it does not exist.

    :param directory: Directory to write to.
    :param file: Name of file to write.
    :param contents: Contents to write to file.
    """
    if not os.path.exists(directory):
        os.makedirs(directory)

    write_file(os.path.join(directory, file), contents)
