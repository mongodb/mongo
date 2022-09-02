"""Unit tests for the resmokelib.testing.fixtures.interface module."""
import logging
import unittest
import ast
import os

DISALLOWED_ROOT = "buildscripts"
ALLOWED_IMPORTS = [
    "buildscripts.resmokelib.testing.fixtures.external",
    "buildscripts.resmokelib.testing.fixtures.interface",
    "buildscripts.resmokelib.testing.fixtures.fixturelib",
    "buildscripts.resmokelib.multiversionconstants",
    "buildscripts.resmokelib.utils.registry",
]
FIXTURE_PATH = os.path.normpath("buildscripts/resmokelib/testing/fixtures")

# These files are not part of the fixure API.
IGNORED_FILES = ["__init__.py", "fixturelib.py", "_builder.py"]


class AdherenceChecker(ast.NodeVisitor):
    """Class that counts up all the breaks from API adherence."""

    def __init__(self, disallowed_root, allowed_imports):
        """Initialize AdherenceChecker."""
        self.breakages = []
        self.disallowed_root = disallowed_root
        self.allowed_imports = allowed_imports

    def check_breakage(self, module):
        if module.split('.')[0] == self.disallowed_root and module not in self.allowed_imports:
            self.breakages.append(module)

    def visit_Import(self, node):  # pylint: disable=invalid-name
        for alias in node.names:
            self.check_breakage(alias.name)

    def visit_ImportFrom(self, node):  # pylint: disable=invalid-name
        # This checks the x in "from x import y", but we can't tell if y is a file or
        # a class in x, so we err on the side of strictness.
        self.check_breakage(node.module)


class TestFixtureAPIAdherence(unittest.TestCase):
    def test_api_adherence(self):
        (_, _, filenames) = next(os.walk(FIXTURE_PATH))
        pathnames = [
            os.path.join(FIXTURE_PATH, file) for file in filenames if file not in IGNORED_FILES
        ]
        for path in pathnames:
            self._check_file(path)

    def _check_file(self, pathname):
        checker = AdherenceChecker(DISALLOWED_ROOT, ALLOWED_IMPORTS)
        with open(pathname) as file:
            checker.visit(ast.parse(file.read()))
        msg = (
            f"File {pathname} imports the following modules that possibly break the fixture API: {checker.breakages}. "
            f"Only files from {ALLOWED_IMPORTS} may be imported. If making an API-breaking change, please add to "
            "fixturelib.py and increment the API version. For statements of form \"from x import y\", please ensure "
            "that x is the pathname of the module being imported and that y is not a file.")
        self.assertFalse(len(checker.breakages), msg)
