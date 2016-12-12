"""
Filename globbing utility.
"""

from __future__ import absolute_import

import glob as _glob
import os
import os.path
import re


_GLOBSTAR = "**"
_CONTAINS_GLOB_PATTERN = re.compile("[*?[]")


def is_glob_pattern(s):
    """
    Returns true if 's' represents a glob pattern, and false otherwise.
    """

    # Copied from glob.has_magic().
    return _CONTAINS_GLOB_PATTERN.search(s) is not None


def glob(globbed_pathname):
    """
    Return a list of pathnames matching the 'globbed_pathname' pattern.

    In addition to containing simple shell-style wildcards a la fnmatch,
    the pattern may also contain globstars ("**"), which is recursively
    expanded to match zero or more subdirectories.
    """

    return list(iglob(globbed_pathname))


def iglob(globbed_pathname):
    """
    Emit a list of pathnames matching the 'globbed_pathname' pattern.

    In addition to containing simple shell-style wildcards a la fnmatch,
    the pattern may also contain globstars ("**"), which is recursively
    expanded to match zero or more subdirectories.
    """

    parts = _split_path(globbed_pathname)
    parts = _canonicalize(parts)

    index = _find_globstar(parts)
    if index == -1:
        for pathname in _glob.iglob(globbed_pathname):
            # Normalize 'pathname' so exact string comparison can be used later.
            yield os.path.normpath(pathname)
        return

    # **, **/, or **/a
    if index == 0:
        expand = _expand_curdir

    # a/** or a/**/ or a/**/b
    else:
        expand = _expand

    prefix_parts = parts[:index]
    suffix_parts = parts[index + 1:]

    prefix = os.path.join(*prefix_parts) if prefix_parts else os.curdir
    suffix = os.path.join(*suffix_parts) if suffix_parts else ""

    for (kind, path) in expand(prefix):
        if not suffix_parts:
            yield path

        # Avoid following symlinks to avoid an infinite loop
        elif suffix_parts and kind == "dir" and not os.path.islink(path):
            path = os.path.join(path, suffix)
            for pathname in iglob(path):
                yield pathname


def _split_path(pathname):
    """
    Return 'pathname' as a list of path components.
    """

    parts = []

    while True:
        (dirname, basename) = os.path.split(pathname)
        parts.append(basename)
        if pathname == dirname:
            parts.append(dirname)
            break
        if not dirname:
            break
        pathname = dirname

    parts.reverse()
    return parts


def _canonicalize(parts):
    """
    Return a copy of 'parts' with consecutive "**"s coalesced.
    Raise a ValueError for unsupported uses of "**".
    """

    res = []

    prev_was_globstar = False
    for p in parts:
        if p == _GLOBSTAR:
            # Skip consecutive **'s
            if not prev_was_globstar:
                prev_was_globstar = True
                res.append(p)
        elif _GLOBSTAR in p:  # a/b**/c or a/**b/c
            raise ValueError("Can only specify glob patterns of the form a/**/b")
        else:
            prev_was_globstar = False
            res.append(p)

    return res


def _find_globstar(parts):
    """
    Return the index of the first occurrence of "**" in 'parts'.
    Return -1 if "**" is not found in the list.
    """

    for (i, p) in enumerate(parts):
        if p == _GLOBSTAR:
            return i
    return -1


def _list_dir(pathname):
    """
    Return a pair of the subdirectory names and filenames immediately
    contained within the 'pathname' directory.

    If 'pathname' does not exist, then None is returned.
    """

    try:
        (_root, dirs, files) = os.walk(pathname).next()
        return (dirs, files)
    except StopIteration:
        return None  # 'pathname' directory does not exist


def _expand(pathname):
    """
    Emit tuples of the form ("dir", dirname) and ("file", filename)
    of all directories and files contained within the 'pathname' directory.
    """

    res = _list_dir(pathname)
    if res is None:
        return

    (dirs, files) = res

    # Zero expansion
    if os.path.basename(pathname):
        yield ("dir", os.path.join(pathname, ""))

    for f in files:
        path = os.path.join(pathname, f)
        yield ("file", path)

    for d in dirs:
        path = os.path.join(pathname, d)
        for x in _expand(path):
            yield x


def _expand_curdir(pathname):
    """
    Emit tuples of the form ("dir", dirname) and ("file", filename)
    of all directories and files contained within the 'pathname' directory.

    The returned pathnames omit a "./" prefix.
    """

    res = _list_dir(pathname)
    if res is None:
        return

    (dirs, files) = res

    # Zero expansion
    yield ("dir", "")

    for f in files:
        yield ("file", f)

    for d in dirs:
        for x in _expand(d):
            yield x
