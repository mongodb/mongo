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

# test_util23.py
# Test that wt verify properly handles scratch buffers on usage path.
class test_util23(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util23.wt'
    uri = 'file:' + tablename

    commands = ["-r", "verify", "-d", "dump_offsets", uri]

    def test_verify_scratch_buffer(self):
        create_params = 'key_format=S,value_format=S'
        self.session.create(self.uri, create_params)

        self.runWt(self.commands,
                   errfilename="errfile.txt",
                   failure=True)

        self.check_file_contains("errfile.txt", 'usage:')
        with open("errfile.txt", 'r') as f:
            content = f.read()
            # Ensure that we no longer see the following error.
            self.assertNotIn('scratch buffer allocated and never discarded', content)

