# -*- coding: utf-8 -*-
""" Unit tests for utils.rmtree. """

from __future__ import absolute_import
from __future__ import print_function

import os
import shutil
import sys
import tempfile
import unittest

from buildscripts.resmokelib import utils

# pylint: disable=missing-docstring,protected-access


def rmtree(dir_root):
    """Invoke utils.rmtree(dir_root) and return True if removed."""
    utils.rmtree(dir_root)
    return not os.path.exists(dir_root)


def create_file(path):
    """Create file named 'path'."""
    with open(path, "w") as fh:
        fh.write("")


def ascii_filesystemencoding():
    """Return True if the file system encoding is type ASCII.

    See https://www.iana.org/assignments/character-sets/character-sets.xhtml.
    """
    encoding = sys.getfilesystemencoding()
    return encoding.startswith("ANSI_X3.4") or encoding == "US-ASCII"


def string_for_ascii_filesystem_encoding(path):
    """Return encoded string type for unicode on ASCII file system encoding.

    Some file system encodings are set to ASCII if LANG=C or LC_ALL=C is specified.
    """
    if ascii_filesystemencoding() and isinstance(path, unicode):
        return path.encode("utf-8")
    return path


class RmtreeTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_dir_root = tempfile.mkdtemp()
        cls.cwd = os.getcwd()

    @classmethod
    def tearDownClass(cls):
        os.chdir(cls.cwd)
        shutil.rmtree(cls.temp_dir_root, ignore_errors=True)

    def do_test(self, name):
        pass

    def test_ascii(self):
        # ASCII name
        self.do_test("ascii")

    def test_unicode(self):
        # Unicode name
        self.do_test(u"unicode")

    def test_greek(self):
        # Name with Greek
        self.do_test(string_for_ascii_filesystem_encoding(u"ελληνικά"))

    def test_japanese(self):
        # Name with Japanese
        self.do_test(string_for_ascii_filesystem_encoding(u"会社案"))


class RmtreeFileTests(RmtreeTestCase):
    def do_test(self, file_name):  # pylint: disable=arguments-differ
        """Execute file test."""
        temp_dir = tempfile.mkdtemp(dir=self.temp_dir_root)
        os.chdir(temp_dir)
        create_file(file_name)
        os.chdir(self.temp_dir_root)
        self.assertTrue(rmtree(temp_dir))


class RmtreeDirectoryTests(RmtreeTestCase):
    def do_test(self, dir_name):  # pylint: disable=arguments-differ
        """Execute directory test."""
        os.chdir(self.temp_dir_root)
        os.mkdir(dir_name)
        self.assertTrue(rmtree(dir_name))


class RmtreeDirectoryWithNonAsciiTests(RmtreeTestCase):
    def do_test(self, name):
        """Execute directory with non-ASCII file test."""
        os.chdir(self.temp_dir_root)
        os.mkdir(name)
        os.chdir(name)
        create_file(name)
        os.chdir(self.temp_dir_root)
        self.assertTrue(rmtree(name))


class ShutilWindowsRmtreeFileTests(RmtreeFileTests):
    def do_test(self, file_name):
        """Execute file test that are known to fail in shutil.rmtree."""
        if not utils.is_windows():
            print("Skipping ShutilWindowsRmtreeFileTests on non-Windows platforms")
            return
        temp_dir = tempfile.mkdtemp(dir=self.temp_dir_root)
        os.chdir(temp_dir)
        create_file(file_name)
        os.chdir(self.temp_dir_root)
        with self.assertRaises(WindowsError):  # pylint: disable=undefined-variable
            shutil.rmtree(temp_dir)

    def test_ascii(self):
        pass

    def test_unicode(self):
        pass
