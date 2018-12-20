"""Functions for working with resmoke test names."""

import os

HOOK_DELIMITER = ":"


def is_resmoke_hook(test_name):
    """Determine whether the given test name is for a resmoke hook."""
    return test_name.find(HOOK_DELIMITER) != -1


def split_test_hook_name(hook_name):
    """
    Split a hook name into the test name and the resmoke hook name.

    Note: This method uses ':' to separate the test name from the resmoke hook name. If the test
    name has a ':' in it (such as burn_in_test.py tests), it will not work correctly.
    """
    assert is_resmoke_hook(hook_name) is True

    hook_name_parts = hook_name.split(HOOK_DELIMITER)

    return hook_name_parts[0], hook_name_parts[1]


def get_short_name_from_test_file(test_file):
    """Determine the short name a test would use based on the given test_file."""

    return os.path.splitext(os.path.basename(test_file))[0]


def normalize_test_file(test_file):
    """
    Normalize the given test file.

    If 'test_file' represents a Windows-style path, then it is converted to a POSIX-style path
    with

    - backslashes (\\) as the path separator replaced with forward slashes (/) and
    - the ".exe" extension, if present, removed.

    If 'test_file' already represents a POSIX-style path, then it is returned unmodified.
    """

    if "\\" in test_file:
        posix_test_file = test_file.replace("\\", "/")
        (test_file_root, test_file_ext) = os.path.splitext(posix_test_file)
        if test_file_ext == ".exe":
            return test_file_root
        return posix_test_file

    return test_file


def denormalize_test_file(test_file):
    """Return a list containing 'test_file' as both a POSIX-style and a Windows-style path.

    The conversion process may involving replacing forward slashes (/) as the path separator
    with backslashes (\\), as well as adding a ".exe" extension if 'test_file' has no file
    extension.
    """

    test_file = normalize_test_file(test_file)

    if "/" in test_file:
        windows_test_file = test_file.replace("/", "\\")
        if not os.path.splitext(test_file)[1]:
            windows_test_file += ".exe"
        return [test_file, windows_test_file]

    return [test_file]
