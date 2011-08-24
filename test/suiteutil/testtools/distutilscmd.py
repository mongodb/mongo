# Copyright (c) 2010-2011 testtools developers . See LICENSE for details.

"""Extensions to the standard Python unittest library."""

import sys

from distutils.core import Command
from distutils.errors import DistutilsOptionError

from testtools.run import TestProgram, TestToolsTestRunner


class TestCommand(Command):
    """Command to run unit tests with testtools"""

    description = "run unit tests with testtools"

    user_options = [
        ('catch', 'c', "Catch ctrl-C and display results so far"),
        ('buffer', 'b', "Buffer stdout and stderr during tests"),
        ('failfast', 'f', "Stop on first fail or error"),
        ('test-module=','m', "Run 'test_suite' in specified module"),
        ('test-suite=','s',
         "Test suite to run (e.g. 'some_module.test_suite')")
    ]

    def __init__(self, dist):
        Command.__init__(self, dist)
        self.runner = TestToolsTestRunner(sys.stdout)


    def initialize_options(self):
        self.test_suite = None
        self.test_module = None
        self.catch = None
        self.buffer = None
        self.failfast = None

    def finalize_options(self):
        if self.test_suite is None:
            if self.test_module is None:
                raise DistutilsOptionError(
                    "You must specify a module or a suite to run tests from")
            else:
                self.test_suite = self.test_module+".test_suite"
        elif self.test_module:
            raise DistutilsOptionError(
                "You may specify a module or a suite, but not both")
        self.test_args = [self.test_suite]
        if self.verbose:
            self.test_args.insert(0, '--verbose')
        if self.buffer:
            self.test_args.insert(0, '--buffer')
        if self.catch:
            self.test_args.insert(0, '--catch')
        if self.failfast:
            self.test_args.insert(0, '--failfast')

    def run(self):
        self.program = TestProgram(
            argv=self.test_args, testRunner=self.runner, stdout=sys.stdout,
            exit=False)
