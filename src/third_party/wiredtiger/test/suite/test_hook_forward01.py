#!/usr/bin/env python3
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

import os
import wttest
from suite_subprocess import suite_subprocess

# test_hook_forward01.py
#    A subprocess started by run_subprocess_function runs under the same hooks as its parent.
# Without that, a test running under a hook silently exercises none of it in the subprocess, and a
# hook that alters the database layout leaves the parent unable to reopen what the subprocess wrote.
# Forwarding also means a hook can skip the subprocess, which has to become a skip here.
class test_hook_forward01(wttest.WiredTigerTestCase, suite_subprocess):
    test_name = __qualname__
    hooks_file = 'subprocess_hooks.txt'

    # Subprocess body: report the hooks this process was started with.
    def subprocess_report_hooks(self):
        with open(self.hooks_file, 'w') as f:
            f.write('\n'.join(self.hook_specs))

    # Subprocess body: skipped, as a hook skips it once it is forwarded there.
    def subprocess_skip(self):
        self.skipTest('the subprocess has nothing to do')

    def test_hook_forward(self):
        [returncode, home] = self.run_subprocess_function('SUBPROCESS_report_hooks',
            f'{self.test_name}.{self.test_name}.subprocess_report_hooks')
        self.assertEqual(returncode, 0)

        with open(os.path.join(home, self.hooks_file), 'r') as f:
            subprocess_specs = f.read().split()
        self.assertEqual(sorted(subprocess_specs), sorted(self.hook_specs))

    def test_hook_forward_subprocess_skip(self):
        self.run_subprocess_function('SUBPROCESS_skip',
            f'{self.test_name}.{self.test_name}.subprocess_skip')
        self.fail('a subprocess that skipped should have skipped this test')
