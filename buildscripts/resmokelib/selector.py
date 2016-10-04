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

    (remaining, included, _) = _filter_by_filename(kind,
                                                   tests,
                                                   include_files,
                                                   exclude_files)

    if include_files:
        return list(included)
    elif exclude_files:
        return list(remaining)
    return tests

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

    # Command line options override the YAML options, and all should be defaulted to an empty list
    # if not specified.
    tags = {
        "exclude_with_all_tags": exclude_with_all_tags,
        "exclude_with_any_tags": exclude_with_any_tags,
        "include_with_all_tags": include_with_all_tags,
        "include_with_any_tags": include_with_any_tags,
    }
    cmd_line_values = (
        ("exclude_with_all_tags", config.EXCLUDE_WITH_ALL_TAGS),
        ("exclude_with_any_tags", config.EXCLUDE_WITH_ANY_TAGS),
        ("include_with_all_tags", config.INCLUDE_WITH_ALL_TAGS),
        ("include_with_any_tags", config.INCLUDE_WITH_ANY_TAGS),
    )
    for (tag_category, cmd_line_val) in cmd_line_values:
        if cmd_line_val is not None:
            # Ignore the empty string when it is used as a tag. Specifying an empty string on the
            # command line allows a user to unset the list of tags specified in the YAML
            # configuration.
            tags[tag_category] = set([tag for tag in cmd_line_val.split(",") if tag != ""])
        else:
            tags[tag_category] = set(utils.default_if_none(tags[tag_category], []))

    using_tags = 0
    for name in tags:
        if not utils.is_string_set(tags[name]):
            raise TypeError("%s must be a list of strings" % (name))
        if len(tags[name]) > 0:
            using_tags += 1

    if using_tags > 1:
        raise ValueError("Can only specify one of 'include_with_all_tags', 'include_with_any_tags',"
                         " 'exclude_with_all_tags', and 'exclude_with_any_tags'. If you wish to"
                         " unset one of these options, use --includeWithAllTags='' or similar")

    jstests = []
    for root in roots:
        jstests.extend(globstar.iglob(root))

    (remaining, included, _) = _filter_by_filename("jstest",
                                                   jstests,
                                                   include_files,
                                                   exclude_files)

    # Skip parsing comments if not using tags
    if not using_tags:
        if include_files:
            return list(included)
        elif exclude_files:
            return list(remaining)
        return jstests

    jstests = set(remaining)
    excluded = set()

    for filename in jstests:
        file_tags = set(jscomment.get_tags(filename))
        if tags["include_with_all_tags"] and not tags["include_with_all_tags"] - file_tags:
            included.add(filename)
        elif tags["include_with_any_tags"] and tags["include_with_any_tags"] & file_tags:
            included.add(filename)
        elif tags["exclude_with_all_tags"] and not tags["exclude_with_all_tags"] - file_tags:
            excluded.add(filename)
        elif tags["exclude_with_any_tags"] and tags["exclude_with_any_tags"] & file_tags:
            excluded.add(filename)

    if tags["include_with_all_tags"] or tags["include_with_any_tags"]:
        if exclude_files:
            return list((included & jstests) - excluded)
        return list(included)
    else:
        if include_files:
            return list(included | (jstests - excluded))
        return list(jstests - excluded)


def _filter_by_filename(kind, universe, include_files, exclude_files):
    """
    Filters out what tests to run solely by filename.

    Returns the triplet (remaining, included, excluded), where
    'remaining' is 'universe' after 'included' and 'excluded' were
    removed from it.
    """

    if not utils.is_string_list(include_files):
        raise TypeError("include_files must be a list of strings")
    elif not utils.is_string_list(exclude_files):
        raise TypeError("exclude_files must be a list of strings")
    elif include_files and exclude_files:
        raise ValueError("Cannot specify both include_files and exclude_files")

    universe = set(universe)
    if include_files:
        (verbatim, globbed) = _partition(include_files)
        # Remove all matching files of 'verbatim' from 'universe'.
        included_verbatim = _pop_all(kind, universe, verbatim)
        included_globbed = set()

        for file_pattern in globbed:
            included_globbed.update(globstar.iglob(file_pattern))

        # Remove all matching files of 'included_globbed' from 'universe' without checking whether
        # the same file is expanded to multiple times. This implicitly takes an intersection
        # between 'included_globbed' and 'universe'.
        included_globbed = _pop_all(kind, universe, included_globbed, validate=False)
        return (universe, included_verbatim | included_globbed, set())

    elif exclude_files:
        (verbatim, globbed) = _partition(exclude_files)

        # Remove all matching files of 'verbatim' from 'universe'.
        excluded_verbatim = _pop_all(kind, universe, verbatim)
        excluded_globbed = set()

        for file_pattern in globbed:
            excluded_globbed.update(globstar.iglob(file_pattern))

        # Remove all matching files of 'excluded_globbed' from 'universe' without checking whether
        # the same file is expanded to multiple times. This implicitly takes an intersection
        # between 'excluded_globbed' and 'universe'.
        excluded_globbed = _pop_all(kind, universe, excluded_globbed, validate=False)
        return (universe, set(), excluded_verbatim | excluded_globbed)

    return (universe, set(), set())


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
