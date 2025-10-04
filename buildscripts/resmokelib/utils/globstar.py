"""Filename globbing utility."""

import functools
import glob as _glob
import os.path
import re

_CONTAINS_GLOB_PATTERN = re.compile("[*?[]")


def is_glob_pattern(string):
    """Return true if 'string' represents a glob pattern, and false otherwise."""

    # Copied from glob.has_magic().
    return _CONTAINS_GLOB_PATTERN.search(string) is not None


@functools.cache
def glob(globbed_pathname):
    """Return a list of pathnames matching the 'globbed_pathname' pattern.

    In addition to containing simple shell-style wildcards a la fnmatch,
    the pattern may also contain globstars ("**"), which is recursively
    expanded to match zero or more subdirectories.
    """

    return list(iglob(globbed_pathname))


def iglob(globbed_pathname):
    """Emit a list of pathnames matching the 'globbed_pathname' pattern.

    In addition to containing simple shell-style wildcards a la fnmatch,
    the pattern may also contain globstars ("**"), which is recursively
    expanded to match zero or more subdirectories.
    """

    for pathname in _glob.iglob(globbed_pathname, recursive=True):
        # Normalize 'pathname' so exact string comparison can be used later.
        yield os.path.normpath(pathname)
