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

import os, wttest
from helper import simulate_crash_restart
from wtdataset import SimpleDataSet

# test_rollback_to_stable45.py
#    Make sure RTS does nothing in a disaggregated storage context.
class test_rollback_to_stable45(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(page_log=palm),' \
        + 'disaggregated=(role="leader")'

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RTS')

    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('page_log', 'palm')

    # Custom test case setup
    def early_setup(self):
        os.mkdir('follower')
        # Create the home directory for the PALM k/v store, and share it with the follower.
        os.mkdir('kv_home')
        os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

    def test_rollback_to_stable45(self):
        uri = "table:rollback_to_stable45"
        ds = SimpleDataSet(self, uri, 500, key_format='S', value_format='S')
        ds.populate()

        c = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        c[ds.key(10)] = ds.value(100)
        c[ds.key(11)] = ds.value(101)
        c[ds.key(12)] = ds.value(102)
        self.session.commit_transaction('commit_timestamp=30')
        c.close()

        # Set stable to 20 and crash.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()
        simulate_crash_restart(self, ".", "RESTART")

        # Check that recovery didn't roll us back.
        c = self.session.open_cursor(uri, None)
        self.assertEquals(c[ds.key(10)], ds.value(100))
        self.assertEquals(c[ds.key(11)], ds.value(101))
        self.assertEquals(c[ds.key(12)], ds.value(102))
        c.close()

        # Runtime RTS should still work.
        self.conn.rollback_to_stable()
        c = self.session.open_cursor(uri, None)
        self.assertEquals(c[ds.key(10)], ds.value(10))
        self.assertEquals(c[ds.key(11)], ds.value(11))
        self.assertEquals(c[ds.key(12)], ds.value(12))
        c.close()
