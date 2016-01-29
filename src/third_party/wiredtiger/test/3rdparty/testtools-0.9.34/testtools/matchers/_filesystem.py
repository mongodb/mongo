# Copyright (c) 2009-2012 testtools developers. See LICENSE for details.

"""Matchers for things related to the filesystem."""

__all__ = [
    'FileContains',
    'DirExists',
    'FileExists',
    'HasPermissions',
    'PathExists',
    'SamePath',
    'TarballContains',
    ]

import os
import tarfile

from ._basic import Equals
from ._higherorder import (
    MatchesAll,
    MatchesPredicate,
    )
from ._impl import (
    Matcher,
    )


def PathExists():
    """Matches if the given path exists.

    Use like this::

      assertThat('/some/path', PathExists())
    """
    return MatchesPredicate(os.path.exists, "%s does not exist.")


def DirExists():
    """Matches if the path exists and is a directory."""
    return MatchesAll(
        PathExists(),
        MatchesPredicate(os.path.isdir, "%s is not a directory."),
        first_only=True)


def FileExists():
    """Matches if the given path exists and is a file."""
    return MatchesAll(
        PathExists(),
        MatchesPredicate(os.path.isfile, "%s is not a file."),
        first_only=True)


class DirContains(Matcher):
    """Matches if the given directory contains files with the given names.

    That is, is the directory listing exactly equal to the given files?
    """

    def __init__(self, filenames=None, matcher=None):
        """Construct a ``DirContains`` matcher.

        Can be used in a basic mode where the whole directory listing is
        matched against an expected directory listing (by passing
        ``filenames``).  Can also be used in a more advanced way where the
        whole directory listing is matched against an arbitrary matcher (by
        passing ``matcher`` instead).

        :param filenames: If specified, match the sorted directory listing
            against this list of filenames, sorted.
        :param matcher: If specified, match the sorted directory listing
            against this matcher.
        """
        if filenames == matcher == None:
            raise AssertionError(
                "Must provide one of `filenames` or `matcher`.")
        if None not in (filenames, matcher):
            raise AssertionError(
                "Must provide either `filenames` or `matcher`, not both.")
        if filenames is None:
            self.matcher = matcher
        else:
            self.matcher = Equals(sorted(filenames))

    def match(self, path):
        mismatch = DirExists().match(path)
        if mismatch is not None:
            return mismatch
        return self.matcher.match(sorted(os.listdir(path)))


class FileContains(Matcher):
    """Matches if the given file has the specified contents."""

    def __init__(self, contents=None, matcher=None):
        """Construct a ``FileContains`` matcher.

        Can be used in a basic mode where the file contents are compared for
        equality against the expected file contents (by passing ``contents``).
        Can also be used in a more advanced way where the file contents are
        matched against an arbitrary matcher (by passing ``matcher`` instead).

        :param contents: If specified, match the contents of the file with
            these contents.
        :param matcher: If specified, match the contents of the file against
            this matcher.
        """
        if contents == matcher == None:
            raise AssertionError(
                "Must provide one of `contents` or `matcher`.")
        if None not in (contents, matcher):
            raise AssertionError(
                "Must provide either `contents` or `matcher`, not both.")
        if matcher is None:
            self.matcher = Equals(contents)
        else:
            self.matcher = matcher

    def match(self, path):
        mismatch = PathExists().match(path)
        if mismatch is not None:
            return mismatch
        f = open(path)
        try:
            actual_contents = f.read()
            return self.matcher.match(actual_contents)
        finally:
            f.close()

    def __str__(self):
        return "File at path exists and contains %s" % self.contents


class HasPermissions(Matcher):
    """Matches if a file has the given permissions.

    Permissions are specified and matched as a four-digit octal string.
    """

    def __init__(self, octal_permissions):
        """Construct a HasPermissions matcher.

        :param octal_permissions: A four digit octal string, representing the
            intended access permissions. e.g. '0775' for rwxrwxr-x.
        """
        super(HasPermissions, self).__init__()
        self.octal_permissions = octal_permissions

    def match(self, filename):
        permissions = oct(os.stat(filename).st_mode)[-4:]
        return Equals(self.octal_permissions).match(permissions)


class SamePath(Matcher):
    """Matches if two paths are the same.

    That is, the paths are equal, or they point to the same file but in
    different ways.  The paths do not have to exist.
    """

    def __init__(self, path):
        super(SamePath, self).__init__()
        self.path = path

    def match(self, other_path):
        f = lambda x: os.path.abspath(os.path.realpath(x))
        return Equals(f(self.path)).match(f(other_path))


class TarballContains(Matcher):
    """Matches if the given tarball contains the given paths.

    Uses TarFile.getnames() to get the paths out of the tarball.
    """

    def __init__(self, paths):
        super(TarballContains, self).__init__()
        self.paths = paths
        self.path_matcher = Equals(sorted(self.paths))

    def match(self, tarball_path):
        # Open underlying file first to ensure it's always closed:
        # <http://bugs.python.org/issue10233>
        f = open(tarball_path, "rb")
        try:
            tarball = tarfile.open(tarball_path, fileobj=f)
            try:
                return self.path_matcher.match(sorted(tarball.getnames()))
            finally:
                tarball.close()
        finally:
            f.close()
