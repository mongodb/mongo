#!/usr/bin/env python
#
# Public Domain 2008-2013 WiredTiger, Inc.
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
import wiredtiger, wttest
from helper import complex_populate, simple_populate, key_populate
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios

# test_compact.py
#    session level compact operation
class test_compact(wttest.WiredTigerTestCase, suite_subprocess):
    name = 'test_compact'

    # Use a small page size because we want to create lots of pages.
    config = 'leaf_page_max=512,value_format=S,key_format=S'
    nentries = 40000

    types = [
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:'))
        ]
    compact = [
        ('method', dict(utility=0,reopen=0)),
        ('method_reopen', dict(utility=0,reopen=1)),
        ('utility', dict(utility=1,reopen=0)),
    ]
    scenarios = number_scenarios(multiply_scenarios('.', types, compact))

    # Test compaction.
    def test_compact(self):
        # Populate an object
        uri = self.uri + self.name
        if self.uri == "file:":
            simple_populate(self, uri, self.config, self.nentries - 1)
        else:
            complex_populate(self, uri, self.config, self.nentries - 1)

        # Reopen the connection to force the object to disk.
        self.reopen_conn()

        # Remove most of the object.
        c1 = self.session.open_cursor(uri, None)
        c1.set_key(key_populate(c1, 5))
        c2 = self.session.open_cursor(uri, None)
        c2.set_key(key_populate(c2, self.nentries - 5))
        self.session.truncate(None, c1, c2, None)
        c1.close()
        c2.close()

        # Compact it, using either the session method or the utility.
        if self.utility == 1:
            self.session.checkpoint(None)
            self.close_conn()
            self.runWt(["compact", uri])
        else:
            # Optionally reopen the connection so we do more on-disk tests.
            if self.reopen == 1:
                self.session.checkpoint(None)
                self.reopen_conn()

            self.session.compact(uri, None)

        # If it's a simple object, confirm it worked.
        if self.uri == "file:":
            self.assertLess(os.stat(self.name).st_size, 10 * 1024)


if __name__ == '__main__':
    wttest.run()
