"""
Test selection utility.

Defines filtering rules for what tests to include in a suite depending
on whether they apply to C++ unit tests, dbtests, or JS tests.
"""

from __future__ import absolute_import

import fnmatch
import os.path
import subprocess
import sys

from . import config
from . import errors
from . import utils
from .utils import globstar
from .utils import jscomment


def filter_cpp_unit_tests(root="build/unittests.txt", include_files=None, exclude_files=None):
    """
    Filters out what C++ unit tests to run.
    """

    include_files = utils.default_if_none(include_files, [])
    exclude_files = utils.default_if_none(exclude_files, [])

    unit_tests = []
    with open(root, "r") as fp:
        for unit_test_path in fp:
            unit_test_path = unit_test_path.rstrip()
            unit_tests.append(unit_test_path)

    (remaining, included, _) = _filter_by_filename("C++ unit test",
                                                   unit_tests,
                                                   include_files,
                                                   exclude_files)

    if include_files:
        return list(included)
    elif exclude_files:
        return list(remaining)
    return unit_tests


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

    include_with_all_tags = set(utils.default_if_none(include_with_all_tags, []))
    include_with_any_tags = set(utils.default_if_none(include_with_any_tags, []))
    exclude_with_all_tags = set(utils.default_if_none(exclude_with_all_tags, []))
    exclude_with_any_tags = set(utils.default_if_none(exclude_with_any_tags, []))

    using_tags = 0
    for (name, value) in (("include_with_all_tags", include_with_all_tags),
                          ("include_with_any_tags", include_with_any_tags),
                          ("exclude_with_all_tags", exclude_with_all_tags),
                          ("exclude_with_any_tags", exclude_with_any_tags)):
        if not utils.is_string_set(value):
            raise TypeError("%s must be a list of strings" % (name))
        if len(value) > 0:
            using_tags += 1

    if using_tags > 1:
        raise ValueError("Can only specify one of 'include_with_all_tags', 'include_with_any_tags',"
                         " 'exclude_with_all_tags', and 'exclude_with_any_tags'")

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
        if include_with_all_tags and not include_with_all_tags - file_tags:
            included.add(filename)
        elif include_with_any_tags and include_with_any_tags & file_tags:
            included.add(filename)
        elif exclude_with_all_tags and not exclude_with_all_tags - file_tags:
            excluded.add(filename)
        elif exclude_with_any_tags and exclude_with_any_tags & file_tags:
            excluded.add(filename)

    if include_with_all_tags or include_with_any_tags:
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
