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
from helper_disagg import DisaggConfigMixin, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

# test_layered19.py
# Test adjustable consecutive deltas

class test_layered19(wttest.WiredTigerTestCase, DisaggConfigMixin):
    uris = [
        ('layered', dict(uri='layered:test_layered19')),
        ('btree', dict(uri='file:test_layered19')),
    ]

    conn_base_config = 'transaction_sync=(enabled,method=fsync),statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'page_delta=(max_consecutive_delta=1),disaggregated=(page_log=palm),'
    disagg_storages = gen_disagg_storages('test_layered19', disagg_only = True)

    nitems = 1000

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(disagg_storages, uris)

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RTS')

    def session_create_config(self):
        # The delta percentage of 200 is an arbitrary large value, intended to produce
        # deltas a lot of the time.
        cfg = 'key_format=S,value_format=S'
        if self.uri.startswith('file'):
            cfg += ',block_manager=disagg'
        return cfg

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader"),'

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        DisaggConfigMixin.conn_extensions(self, extlist)

    def test_layered_read_write(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        value1 = "aaaa"
        value2 = "bbbb"

        for i in range(self.nitems):
            cursor[str(i)] = value1

        # XXX
        # Inserted timing delays before checkpoint, apparently needed because of the
        # layered table watcher implementation
        import time
        time.sleep(1.0)
        self.session.checkpoint()

        for i in range(self.nitems):
            if i % 10 == 0:
                cursor[str(i)] = value2

        # XXX
        # Inserted timing delays around reopen, apparently needed because of the
        # layered table watcher implementation
        import time
        time.sleep(1.0)
        self.session.checkpoint()

        for i in range(self.nitems):
            if i % 10 == 0:
                cursor[str(i)] = value2

        # XXX
        # Inserted timing delays around reopen, apparently needed because of the
        # layered table watcher implementation
        time.sleep(1.0)
        self.session.checkpoint()

        follower_config = self.conn_base_config + 'disaggregated=(role="follower",' +\
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        self.reopen_conn(config = follower_config)
        time.sleep(1.0)

        cursor = self.session.open_cursor(self.uri, None, None)

        for i in range(self.nitems):
            if i % 10 == 0:
                self.assertEqual(cursor[str(i)], value2)
            else:
                self.assertEqual(cursor[str(i)], value1)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        read_delta = stat_cursor[stat.conn.cache_read_leaf_delta][2]
        self.assertEqual(read_delta, 0)
        stat_cursor.close()
