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

    results = _glob.iglob(globbed_pathname, recursive=True)

    # Python 3.13 changed glob behavior for recursive patterns.
    # In Python 3.10, when pattern is "dir/**", _glob2() always yields empty string
    # which becomes "dir" when joined. Python 3.13's _glob2() only yields if dir exists.
    # Peek at first result to check if glob returned anything without consuming the iterator.
    first_result = next(results, None)

    if first_result is None:
        # No results from glob - simulate Python 3.10 behavior for recursive patterns
        dirname, basename = os.path.split(globbed_pathname)
        # Check if basename is exactly "**" (recursive wildcard)
        is_recursive = basename == "**" or (isinstance(basename, bytes) and basename == b"**")
        if _glob.has_magic(basename) and is_recursive:
            # Mimic Python 3.10: _glob2 yields empty string, joined with dirname = dirname
            if dirname:
                yield os.path.normpath(dirname)
        return

    # Yield the first result we peeked at
    yield os.path.normpath(first_result)

    # Then yield the rest
    for pathname in results:
        # Normalize 'pathname' so exact string comparison can be used later.
        yield os.path.normpath(pathname)
