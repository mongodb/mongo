#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import wttest
from suite_subprocess import suite_subprocess
from helper import compare_files


# test_util22.py
# Check that wt correctly processes the help option and that it handles invalid options correctly.
class test_util22(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config = ''

    # Skip 'copyright', which does not process options
    commands = ['alter', 'backup', 'compact', 'create', 'downgrade', 'drop', 'dump', 'list', 'load',
                'loadtext', 'printlog', 'read', 'rename', 'salvage', 'stat', 'truncate', 'upgrade',
                'verify', 'write']

    def test_help_option(self):
        errfilename = "errfile.txt"
        self.runWt(['-?'], errfilename=errfilename)
        self.check_file_contains(errfilename, 'global_options:')

        for cmd in self.commands:
            self.runWt([cmd, '-?'], errfilename=errfilename)
            self.check_file_contains(errfilename, 'options:')

    def test_no_argument(self):
        errfilename = "errfile.txt"
        self.runWt(['-h'], errfilename=errfilename, failure=True)
        self.check_file_contains(errfilename, 'wt: option requires an argument -- h')
        self.check_file_contains(errfilename, 'global_options:')

    def test_unsupported_command(self):
        errfilename = "errfile.txt"
        self.runWt(['unsupported'], errfilename=errfilename, failure=True)
        self.check_file_contains(errfilename, 'global_options:')

    def test_unsupported_option(self):
        errfilename = "errfile.txt"
        self.runWt(['-^'], errfilename=errfilename, failure=True)
        self.check_file_contains(errfilename, 'wt: illegal option -- ^')
        self.check_file_contains(errfilename, 'global_options:')

        for cmd in self.commands:
            self.runWt([cmd, '-^'], errfilename=errfilename, failure=True)
            self.check_file_contains(errfilename, 'wt: illegal option -- ^')
            self.check_file_contains(errfilename, 'options:')

if __name__ == '__main__':
    wttest.run()
