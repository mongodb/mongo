#!/usr/bin/env python2
# Copyright (C) 2017 MongoDB Inc.
#
# This program is free software: you can redistribute it and/or  modify
# it under the terms of the GNU Affero General Public License, version 3,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
"""
Test cases for IDL Generator.

This file exists to verify code coverage for the generator.py file. To run code coverage, run in the
idl base directory:

$ coverage run run_tests.py && coverage html
"""

from __future__ import absolute_import, print_function, unicode_literals

import os
import unittest

# import package so that it works regardless of whether we run as a module or file
if __package__ is None:
    import sys
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    from context import idl
    import testcase
else:
    from .context import idl
    from . import testcase


class TestGenerator(testcase.IDLTestcase):
    """Test the IDL Generator."""

    def test_compile(self):
        # type: () -> None
        """Exercise the code generator so code coverage can be measured."""
        base_dir = os.path.dirname(
            os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
        src_dir = os.path.join(
            base_dir,
            'src', )
        idl_dir = os.path.join(src_dir, 'mongo', 'idl')

        args = idl.compiler.CompilerArgs()
        args.output_suffix = "_codecoverage_gen"
        args.import_directories = [src_dir]

        unittest_idl_file = os.path.join(idl_dir, 'unittest.idl')
        if not os.path.exists(unittest_idl_file):
            unittest.skip("Skipping IDL Generator testing since %s could not be found." %
                          (unittest_idl_file))
            return

        args.input_file = os.path.join(idl_dir, 'unittest_import.idl')
        self.assertTrue(idl.compiler.compile_idl(args))

        args.input_file = unittest_idl_file
        self.assertTrue(idl.compiler.compile_idl(args))


if __name__ == '__main__':

    unittest.main()
