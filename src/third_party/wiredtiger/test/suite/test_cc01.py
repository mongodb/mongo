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

import time, wiredtiger, wttest
from wiredtiger import stat

# Shared base class used by cc tests.
class test_cc_base(wttest.WiredTigerTestCase):

    def get_stat(self, stat, uri = ""):
        stat_cursor = self.session.open_cursor(f'statistics:{uri}')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def large_updates(self, uri, value, ds, nrows, commit_ts):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(0, nrows):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def large_modifies(self, uri, value, ds, location, nbytes, nrows, commit_ts):
        # Load a slight modification.
        session = self.session
        cursor = session.open_cursor(uri)
        session.begin_transaction()
        for i in range(0, nrows):
            cursor.set_key(i)
            mods = [wiredtiger.Modify(value, location, nbytes)]
            self.assertEqual(cursor.modify(mods), 0)
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def check(self, check_value, uri, nrows, read_ts):
        session = self.session
        session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1
        session.rollback_transaction()
        self.assertEqual(count, nrows)

    def populate(self, uri, start_key, num_keys, value, ts = None):
        c = self.session.open_cursor(uri, None)
        for k in range(start_key, num_keys):
            self.session.begin_transaction()
            c[k] = value
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(ts if ts else k + 1))
        c.close()

    # Trigger checkpoint cleanup. The function waits for checkpoint cleanup to make progress before
    # exiting.
    def wait_for_cc_to_run(self, ckpt_name = ""):
        c = self.session.open_cursor('statistics:')
        cc_success = prev_cc_success = c[stat.conn.checkpoint_cleanup_success][2]
        c.close()
        ckpt_config = "debug=(checkpoint_cleanup=true)"
        if ckpt_name:
            ckpt_config += f",name={ckpt_name}"
        self.session.checkpoint(ckpt_config)
        while cc_success - prev_cc_success == 0:
            time.sleep(0.1)
            c = self.session.open_cursor('statistics:')
            cc_success = c[stat.conn.checkpoint_cleanup_success][2]
            c.close()

    # Trigger checkpoint clean up and check it has visited and removed pages.
    def check_cc_stats(self, ckpt_name = ""):
        self.wait_for_cc_to_run(ckpt_name=ckpt_name)
        c = self.session.open_cursor('statistics:')
        self.assertGreater(c[stat.conn.checkpoint_cleanup_pages_visited][2], 0)
        self.assertGreater(c[stat.conn.checkpoint_cleanup_pages_removed][2], 0)
        c.close()
