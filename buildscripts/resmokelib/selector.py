"""
Test selection utility.

Defines filtering rules for what tests to include in a suite depending
on whether they apply to C++ unit tests, dbtests, or JS tests.
"""

from __future__ import absolute_import

import errno
import fnmatch
import os.path
import subprocess
import sys

from . import config
from . import errors
from . import utils
from .utils import globstar
from .utils import jscomment


def _filter_cpp_tests(kind, root, include_files, exclude_files):
    """
    Generic filtering logic for C++ tests that are sourced from a list
    of test executables.
    """
    include_files = utils.default_if_none(include_files, [])
    exclude_files = utils.default_if_none(exclude_files, [])

    tests = []
    with open(root, "r") as fp:
        for test_path in fp:
            test_path = test_path.rstrip()
            tests.append(test_path)

    return list(_filter_by_filename(kind, tests, include_files, exclude_files))


def filter_cpp_unit_tests(root=config.DEFAULT_UNIT_TEST_LIST,
                          include_files=None,
                          exclude_files=None):
    """
    Filters out what C++ unit tests to run.
    """
    return _filter_cpp_tests("C++ unit test", root, include_files, exclude_files)


def filter_cpp_integration_tests(root=config.DEFAULT_INTEGRATION_TEST_LIST,
                                 include_files=None,
                                 exclude_files=None):
    """
    Filters out what C++ integration tests to run.
    """
    return _filter_cpp_tests("C++ integration test", root, include_files, exclude_files)


def filter_dbtests(binary=None, include_suites=None):
    """
    Filters out what dbtests to run.
    """

    # Command line option overrides the YAML configuration.
    binary = utils.default_if_none(config.DBTEST_EXECUTABLE, binary)
    # Use the default if nothing specified.
    binary = utils.default_if_none(binary, config.DEFAULT_DBTEST_EXECUTABLE)

    include_suites = utils.default_if_none(include_suites, [])

    if not utils.is_string_list(include_suites):
        raise TypeError("include_suites must be a list of strings")

    # Ensure that executable files on Windows have a ".exe" extension.
    if sys.platform == "win32" and os.path.splitext(binary)[1] != ".exe":
        binary += ".exe"

    if not os.path.isfile(binary):
        raise IOError(errno.ENOENT, "File not found", binary)

    program = subprocess.Popen([binary, "--list"], stdout=subprocess.PIPE)
    stdout = program.communicate()[0]

    if program.returncode != 0:
        raise errors.ResmokeError("Getting list of dbtest suites failed")

    dbtests = stdout.splitlines()

    if not include_suites:
        return dbtests

    dbtests = set(dbtests)

    (verbatim, globbed) = _partition(include_suites, normpath=False)
    included = _pop_all("dbtest suite", dbtests, verbatim)

    for suite_pattern in globbed:
        for suite_name in dbtests:
            if fnmatch.fnmatchcase(suite_name, suite_pattern):
                included.add(suite_name)

    return list(included)


def filter_jstests(roots,
                   include_files=None,
                   include_with_all_tags=None,
                   include_with_any_tags=None,
                   exclude_files=None,
                   exclude_with_all_tags=None,
                   exclude_with_any_tags=None):
    """
    Filters out what jstests to run.
    """
    include_files = utils.default_if_none(include_files, [])
    exclude_files = utils.default_if_none(exclude_files, [])

    # Command line options are merged with YAML options.
    tags = {
        # The constructor for an empty set does not accept "None".
        "exclude_with_all_tags": set(utils.default_if_none(exclude_with_all_tags, [])),
        "exclude_with_any_tags": set(utils.default_if_none(exclude_with_any_tags, [])),
        "include_with_all_tags": set(utils.default_if_none(include_with_all_tags, [])),
        "include_with_any_tags": set(utils.default_if_none(include_with_any_tags, [])),
    }

    for name in tags:
        if not utils.is_string_set(tags[name]):
            raise TypeError("%s must be a YAML list of strings" % (name))

    cmd_line_lists = (
        ("exclude_with_all_tags", config.EXCLUDE_WITH_ALL_TAGS),
        ("exclude_with_any_tags", config.EXCLUDE_WITH_ANY_TAGS),
        ("include_with_all_tags", config.INCLUDE_WITH_ALL_TAGS),
        ("include_with_any_tags", config.INCLUDE_WITH_ANY_TAGS),
    )

    # Merge command line options with YAML options.
    for (tag_category, cmd_line_list) in cmd_line_lists:
        if cmd_line_list is not None:
            # Ignore the empty string when it is used as a tag. Specifying an empty string on the
            # command line has no effect and allows a user to more easily synthesize a resmoke.py
            # invocation in their Evergreen project configuration.
            for cmd_line_tags in cmd_line_list:
                tags[tag_category] |= set(tag for tag in cmd_line_tags.split(",") if tag != "")

    jstests_list = []
    for root in roots:
        jstests_list.extend(globstar.iglob(root))

    using_tags = False
    for tag in tags.values():
        if tag:
            using_tags = True

    # Skip converting 'jstests_list' to a set when it isn't being filtered in order to retain its
    # ordering.
    if not include_files and not exclude_files and not using_tags:
        return jstests_list

    jstests_set = _filter_by_filename("jstest", jstests_list, include_files, exclude_files)

    # Skip parsing comments if not using tags.
    if not using_tags:
        return list(jstests_set)

    excluded = set()

    for filename in jstests_set:
        file_tags = set(jscomment.get_tags(filename))
        if tags["include_with_all_tags"] and tags["include_with_all_tags"] - file_tags:
            excluded.add(filename)
        if tags["include_with_any_tags"] and not tags["include_with_any_tags"] & file_tags:
            excluded.add(filename)
        if tags["exclude_with_all_tags"] and not tags["exclude_with_all_tags"] - file_tags:
            excluded.add(filename)
        if tags["exclude_with_any_tags"] and tags["exclude_with_any_tags"] & file_tags:
            excluded.add(filename)

    # Specifying include_files overrides tags.
    return list((jstests_set - excluded) | set(include_files))


def _filter_by_filename(kind, universe, include_files, exclude_files):
    """
    Filters out what tests to run solely by filename.

    Returns either the set of files from 'universe' that are present in 'include_files', or the
    set of files from 'universe' that aren't present in 'exclude_files', depending on which of
    'include_files' and 'exclude_files' is non-empty.

    An error is raised if both 'include_files' and 'exclude_files' are specified.
    """

    if not utils.is_string_list(include_files):
        raise TypeError("include_files must be a list of strings")
    elif not utils.is_string_list(exclude_files):
        raise TypeError("exclude_files must be a list of strings")
    elif include_files and exclude_files:
        raise ValueError("Cannot specify both include_files and exclude_files")

    universe = set(universe)
    files = include_files if include_files else exclude_files

    (verbatim, globbed) = _partition(files)
    # Remove all matching files of 'verbatim' from 'universe'.
    files_verbatim = _pop_all(kind, universe, verbatim)
    files_globbed = set()

    for file_pattern in globbed:
        files_globbed.update(globstar.iglob(file_pattern))

    # Remove all matching files of 'files_globbed' from 'universe' without checking whether
    # the same file is expanded to multiple times. This implicitly takes an intersection
    # between 'files_globbed' and 'universe'.
    files_globbed = _pop_all(kind, universe, files_globbed, validate=False)

    if include_files:
        return files_verbatim | files_globbed

    return universe


def _partition(pathnames, normpath=True):
    """
    Splits 'pathnames' into two separate lists based on whether they
    use a glob pattern.

    Returns the pair (non-globbed pathnames, globbed pathnames).
    """

    verbatim = []
    globbed = []

    for pathname in pathnames:
        if globstar.is_glob_pattern(pathname):
            globbed.append(pathname)
            continue

        # Normalize 'pathname' so exact string comparison can be used later.
        if normpath:
            pathname = os.path.normpath(pathname)
        verbatim.append(pathname)

    return (verbatim, globbed)


def _pop_all(kind, universe, iterable, validate=True):
    """
    Removes all elements of 'iterable' from 'universe' and returns them.

    If 'validate' is true, then a ValueError is raised if a element
    would be removed multiple times, or if an element of 'iterable' does
    not appear in 'universe' at all.
    """

    members = set()

    for elem in iterable:
        if validate and elem in members:
            raise ValueError("%s '%s' specified multiple times" % (kind, elem))

        if elem in universe:
            universe.remove(elem)
            members.add(elem)
        elif validate:
            raise ValueError("Unrecognized %s '%s'" % (kind, elem))

    return members
