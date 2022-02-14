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

import os
from suite_subprocess import suite_subprocess
import wttest

# test_util17.py
#    Utilities: wt stat
class test_util17(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util17.a'

    def test_stat_process(self):
        """
        Test stat in a 'wt' process
        This test is just here to confirm that stat produces a correct looking
        output, it isn't here to do statistics validation.
        """
        params = 'key_format=S,value_format=S'
        outfile = "wt-stat.out"
        expected_string = "cursor: cursor create calls="
        self.session.create('table:' + self.tablename, params)
        self.assertTrue(os.path.exists(self.tablename + ".wt"))
        self.runWt(["stat"], outfilename=outfile)
        self.check_file_contains(outfile, expected_string)

        expected_string = "cache_walk: Entries in the root page=1"
        self.runWt(["stat", "table:" + self.tablename ], outfilename=outfile)
        self.check_file_contains(outfile, expected_string)

if __name__ == '__main__':
    wttest.run()
