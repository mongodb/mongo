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
from wtdataset import ComplexDataSet
import os

# test_bug025.py
# WT-7208: After a missing index is accessed, we return an error.
# After a later access, we crash (null pointer access).
#
# In the ticket, the application reports the problem after the index is corrupted (not removed).
# In our tests, we get a panic right away in case of corruption - presumably the index needs to
# be of the proper size and the corruption is just the right place.  Anyway, having a missing
# index gives exactly the same symptom (second access crashes), and the crash appears in the same
# lines of code.

class test_bug025(wttest.WiredTigerTestCase):
    uri = "table:test_bug025"
    nrows = 10

    def test_bug025(self):
        ds = ComplexDataSet(self, self.uri, self.nrows, key_format="S", value_format='S')
        ds.populate()
        iname = ds.index_name(0)
        iname_suffix = iname[iname.rindex(':') + 1:]
        filename = 'test_bug025_' + iname_suffix + '.wti'
        self.close_conn()
        pos = os.path.getsize(filename) - 1024
        os.remove(filename)

        # We will get error output, but not always in the same API calls from run to run,
        # in particular the open connection doesn't always report the missing file, as
        # index files are usually lazily loaded. As long as the missing file is reported
        # at least once in the following code, it's good.
        with self.expectedStderrPattern('.*No such file or directory.*'):
            self.open_conn()

            newkey = ds.key(self.nrows)
            newval = ds.value(self.nrows)
            cursor = self.session.open_cursor(self.uri)

            # The insert fails, and the cursor remains open.
            try:
                cursor[newkey] = newval
            except Exception as e:
                self.pr('Exception in first access: ' + str(e))

            # The insert fails again.  Before the associated fix was made, the insert crashed.
            try:
                cursor[newkey] = newval    # point of crash
            except Exception as e:
                self.pr('Exception in second access: ' + str(e))

            cursor.close()

if __name__ == '__main__':
    wttest.run()
