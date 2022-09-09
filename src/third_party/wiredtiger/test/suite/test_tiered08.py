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
#
# [TEST_TAGS]
# tiered_storage:checkpoint
# tiered_storage:flush_tier
# [END_TAGS]
#

import os, threading, time, wttest
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources, get_conn_config
from wiredtiger import stat
from wtthread import checkpoint_thread, flush_tier_thread
from wtscenario import make_scenarios


# test_tiered08.py
#   Run background checkpoints and flush_tier operations while inserting
#   data into a table from another thread.
class test_tiered08(wttest.WiredTigerTestCase, TieredConfigMixin):

    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered08', tiered_only=True)

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(storage_sources)

    batch_size = 100000

    # Keep inserting keys until we've done this many flush and checkpoint ops.
    ckpt_target = 1000
    flush_target = 500

    uri = "table:test_tiered08"

    def conn_config(self):
        return get_conn_config(self) + '),statistics=(fast),timing_stress_for_test=(tiered_flush_finish)'
        
    # Load the storage store extension.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def key_gen(self, i):
        return 'KEY' + str(i)

    def value_gen(self, i):
        return 'VALUE_' + 'filler' * (i % 12) + str(i)

    # Populate the test table.  Keep adding keys until the desired number of flush and
    # checkpoint operations have happened.
    def populate(self):
        ckpt_count = 0
        flush_count = 0
        nkeys = 0

        self.pr('Populating tiered table')
        c = self.session.open_cursor(self.uri, None, None)
        while ckpt_count < self.ckpt_target or flush_count < self.flush_target:
            for i in range(nkeys, nkeys + self.batch_size):
                c[self.key_gen(i)] = self.value_gen(i)
            nkeys += self.batch_size
            ckpt_count = self.get_stat(stat.conn.txn_checkpoint)
            flush_count = self.get_stat(stat.conn.flush_tier)
            self.pr('Populating: ckpt {}, flush {}'.format(str(ckpt_count), str(flush_count)))
        c.close()
        return nkeys

    def verify(self, key_count):
        self.pr('Verifying tiered table: {}'.format(str(key_count)))
        c = self.session.open_cursor(self.uri, None, None)
        # Speed up the test by not looking at every key/value pair.
        for i in range(1, key_count, 237):
            self.assertEqual(c[self.key_gen(i)], self.value_gen(i))
        c.close()

    def test_tiered08(self):

        # FIXME-WT-9823, FIXME-WT-9837
        # This part of the test multi-threads checkpoint, flush, and insert
        # operations, creating races in tiered storage. It triggers several 
        # bugs that occur frequently in our testing. We will re-enable this
        # testing when the bugs have been addressed.
        self.skipTest('Concurrent flush_tier, checkpoint, and insert operations cause races.')

        cfg = self.conn_config()
        self.pr('Config is: ' + cfg)
        self.session.create(self.uri,
            'key_format=S,value_format=S,internal_page_max=4096,leaf_page_max=4096')

        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        flush = flush_tier_thread(self.conn, done)

        # Start background threads and give them a chance to start.
        ckpt.start()
        flush.start()
        time.sleep(0.5)

        key_count = self.populate()

        done.set()
        flush.join()
        ckpt.join()

        self.verify(key_count)

        self.close_conn()
        self.pr('Reopening tiered table')
        self.reopen_conn()

        self.verify(key_count)
        
if __name__ == '__main__':
    wttest.run()
