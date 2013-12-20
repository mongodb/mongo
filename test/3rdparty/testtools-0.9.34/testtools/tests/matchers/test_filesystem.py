# Copyright (c) 2008-2012 testtools developers. See LICENSE for details.

import os
import shutil
import tarfile
import tempfile

from testtools import TestCase
from testtools.matchers import (
    Contains,
    DocTestMatches,
    Equals,
    )
from testtools.matchers._filesystem import (
    DirContains,
    DirExists,
    FileContains,
    FileExists,
    HasPermissions,
    PathExists,
    SamePath,
    TarballContains,
    )


class PathHelpers(object):

    def mkdtemp(self):
        directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, directory)
        return directory

    def create_file(self, filename, contents=''):
        fp = open(filename, 'w')
        try:
            fp.write(contents)
        finally:
            fp.close()

    def touch(self, filename):
        return self.create_file(filename)


class TestPathExists(TestCase, PathHelpers):

    def test_exists(self):
        tempdir = self.mkdtemp()
        self.assertThat(tempdir, PathExists())

    def test_not_exists(self):
        doesntexist = os.path.join(self.mkdtemp(), 'doesntexist')
        mismatch = PathExists().match(doesntexist)
        self.assertThat(
            "%s does not exist." % doesntexist, Equals(mismatch.describe()))


class TestDirExists(TestCase, PathHelpers):

    def test_exists(self):
        tempdir = self.mkdtemp()
        self.assertThat(tempdir, DirExists())

    def test_not_exists(self):
        doesntexist = os.path.join(self.mkdtemp(), 'doesntexist')
        mismatch = DirExists().match(doesntexist)
        self.assertThat(
            PathExists().match(doesntexist).describe(),
            Equals(mismatch.describe()))

    def test_not_a_directory(self):
        filename = os.path.join(self.mkdtemp(), 'foo')
        self.touch(filename)
        mismatch = DirExists().match(filename)
        self.assertThat(
            "%s is not a directory." % filename, Equals(mismatch.describe()))


class TestFileExists(TestCase, PathHelpers):

    def test_exists(self):
        tempdir = self.mkdtemp()
        filename = os.path.join(tempdir, 'filename')
        self.touch(filename)
        self.assertThat(filename, FileExists())

    def test_not_exists(self):
        doesntexist = os.path.join(self.mkdtemp(), 'doesntexist')
        mismatch = FileExists().match(doesntexist)
        self.assertThat(
            PathExists().match(doesntexist).describe(),
            Equals(mismatch.describe()))

    def test_not_a_file(self):
        tempdir = self.mkdtemp()
        mismatch = FileExists().match(tempdir)
        self.assertThat(
            "%s is not a file." % tempdir, Equals(mismatch.describe()))


class TestDirContains(TestCase, PathHelpers):

    def test_empty(self):
        tempdir = self.mkdtemp()
        self.assertThat(tempdir, DirContains([]))

    def test_not_exists(self):
        doesntexist = os.path.join(self.mkdtemp(), 'doesntexist')
        mismatch = DirContains([]).match(doesntexist)
        self.assertThat(
            PathExists().match(doesntexist).describe(),
            Equals(mismatch.describe()))

    def test_contains_files(self):
        tempdir = self.mkdtemp()
        self.touch(os.path.join(tempdir, 'foo'))
        self.touch(os.path.join(tempdir, 'bar'))
        self.assertThat(tempdir, DirContains(['bar', 'foo']))

    def test_matcher(self):
        tempdir = self.mkdtemp()
        self.touch(os.path.join(tempdir, 'foo'))
        self.touch(os.path.join(tempdir, 'bar'))
        self.assertThat(tempdir, DirContains(matcher=Contains('bar')))

    def test_neither_specified(self):
        self.assertRaises(AssertionError, DirContains)

    def test_both_specified(self):
        self.assertRaises(
            AssertionError, DirContains, filenames=[], matcher=Contains('a'))

    def test_does_not_contain_files(self):
        tempdir = self.mkdtemp()
        self.touch(os.path.join(tempdir, 'foo'))
        mismatch = DirContains(['bar', 'foo']).match(tempdir)
        self.assertThat(
            Equals(['bar', 'foo']).match(['foo']).describe(),
            Equals(mismatch.describe()))


class TestFileContains(TestCase, PathHelpers):

    def test_not_exists(self):
        doesntexist = os.path.join(self.mkdtemp(), 'doesntexist')
        mismatch = FileContains('').match(doesntexist)
        self.assertThat(
            PathExists().match(doesntexist).describe(),
            Equals(mismatch.describe()))

    def test_contains(self):
        tempdir = self.mkdtemp()
        filename = os.path.join(tempdir, 'foo')
        self.create_file(filename, 'Hello World!')
        self.assertThat(filename, FileContains('Hello World!'))

    def test_matcher(self):
        tempdir = self.mkdtemp()
        filename = os.path.join(tempdir, 'foo')
        self.create_file(filename, 'Hello World!')
        self.assertThat(
            filename, FileContains(matcher=DocTestMatches('Hello World!')))

    def test_neither_specified(self):
        self.assertRaises(AssertionError, FileContains)

    def test_both_specified(self):
        self.assertRaises(
            AssertionError, FileContains, contents=[], matcher=Contains('a'))

    def test_does_not_contain(self):
        tempdir = self.mkdtemp()
        filename = os.path.join(tempdir, 'foo')
        self.create_file(filename, 'Goodbye Cruel World!')
        mismatch = FileContains('Hello World!').match(filename)
        self.assertThat(
            Equals('Hello World!').match('Goodbye Cruel World!').describe(),
            Equals(mismatch.describe()))
class TestTarballContains(TestCase, PathHelpers):

    def test_match(self):
        tempdir = self.mkdtemp()
        in_temp_dir = lambda x: os.path.join(tempdir, x)
        self.touch(in_temp_dir('a'))
        self.touch(in_temp_dir('b'))
        tarball = tarfile.open(in_temp_dir('foo.tar.gz'), 'w')
        tarball.add(in_temp_dir('a'), 'a')
        tarball.add(in_temp_dir('b'), 'b')
        tarball.close()
        self.assertThat(
            in_temp_dir('foo.tar.gz'), TarballContains(['b', 'a']))

    def test_mismatch(self):
        tempdir = self.mkdtemp()
        in_temp_dir = lambda x: os.path.join(tempdir, x)
        self.touch(in_temp_dir('a'))
        self.touch(in_temp_dir('b'))
        tarball = tarfile.open(in_temp_dir('foo.tar.gz'), 'w')
        tarball.add(in_temp_dir('a'), 'a')
        tarball.add(in_temp_dir('b'), 'b')
        tarball.close()
        mismatch = TarballContains(['d', 'c']).match(in_temp_dir('foo.tar.gz'))
        self.assertEqual(
            mismatch.describe(),
            Equals(['c', 'd']).match(['a', 'b']).describe())


class TestSamePath(TestCase, PathHelpers):

    def test_same_string(self):
        self.assertThat('foo', SamePath('foo'))

    def test_relative_and_absolute(self):
        path = 'foo'
        abspath = os.path.abspath(path)
        self.assertThat(path, SamePath(abspath))
        self.assertThat(abspath, SamePath(path))

    def test_real_path(self):
        tempdir = self.mkdtemp()
        source = os.path.join(tempdir, 'source')
        self.touch(source)
        target = os.path.join(tempdir, 'target')
        try:
            os.symlink(source, target)
        except (AttributeError, NotImplementedError):
            self.skip("No symlink support")
        self.assertThat(source, SamePath(target))
        self.assertThat(target, SamePath(source))


class TestHasPermissions(TestCase, PathHelpers):

    def test_match(self):
        tempdir = self.mkdtemp()
        filename = os.path.join(tempdir, 'filename')
        self.touch(filename)
        permissions = oct(os.stat(filename).st_mode)[-4:]
        self.assertThat(filename, HasPermissions(permissions))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
