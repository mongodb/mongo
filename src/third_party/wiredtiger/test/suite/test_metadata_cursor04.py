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

# test_metadata04.py
#    Check metadata create cursors with complex tables.
class test_metadata04(wttest.WiredTigerTestCase):
    # Turn logging on for the database but we will turn logging off for all tables.
    conn_config = 'log=(enabled)'
    uri = 'table:metadata04'

    def check_meta(self, uri, check_log):
        cur = self.session.open_cursor('metadata:create', None, None)
        cur.set_key(uri)
        cur.search()
        meta = cur.get_value()
        cur.close()
        self.pr("======== " +  uri)
        self.pr(meta)
        if not check_log:
            return

        log_false = meta.find('log=(enabled=false)')
        if log_false == -1:
            self.pr(uri + " FAIL: " + meta)
        else:
            self.pr(uri + " SUCCESS")
        self.assertTrue(log_false != -1)

    # Test a complex table with column groups and an index.
    def test_metadata04_complex(self):
        self.session.create(self.uri,
                            "log=(enabled=false),key_format=S,value_format=SS," +
                            "columns=(key,s0,s1),colgroups=(c1)")

        self.session.create("index:metadata04:s0", "log=(enabled=false),columns=(s0)")
        self.session.create("colgroup:metadata04:c1", "log=(enabled=false),columns=(s0,s1)")

        # Check that we can see that logging on the index and colgroup tables is false.
        self.check_meta("colgroup:metadata04:c1", True)
        self.check_meta("index:metadata04:s0", True)
        self.check_meta(self.uri, False)

    # Test a simple table.
    def test_metadata04_table(self):
        self.session.create(self.uri, 'log=(enabled=false),key_format=S,value_format=S,')
        self.check_meta(self.uri, True)

if __name__ == '__main__':
    wttest.run()
