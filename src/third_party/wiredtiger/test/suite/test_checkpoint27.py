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

import threading, time
import wttest
import wiredtiger
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_checkpoint27.py
#
# Check that nothing bad happens if we read in metadata pages while in the
# middle of reading a checkpoint.

class test_checkpoint(wttest.WiredTigerTestCase):

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    name_values = [
        ('named', dict(first_checkpoint='first_checkpoint')),
        ('unnamed', dict(first_checkpoint=None)),
    ]
    scenarios = make_scenarios(format_values, name_values)


    def large_updates(self, uri, ds, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value
            if i % 101 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def evict_metadata(self):
        metadata_uri = "file:WiredTiger.wt"
        evict_cursor = self.session.open_cursor(metadata_uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        # Because the evict cursor hook does evictions on reset, it's not possible to just
        # iterate through and evict every Nth key. In most test uses this is ok because we're
        # iterating a table we control and we know what the keys are; but for the metadata it's
        # a bit of a nuisance. So, since the metadata table isn't large, iterate it once and
        # save the eviction keys in a Python list, then iterate those keys and evict them.
        #
        # FUTURE: would be nice to evict each page exactly once, but that's even harder here
        # than in the usual eviction cursor case.
        keys = []
        n = 0
        for k, v in evict_cursor:
            keys.append(k)
            n += 1
        for k in keys:
            v = evict_cursor[k]
            self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

    def do_checkpoint(self, ckpt_name):
        if ckpt_name is None:
            self.session.checkpoint()
        else:
            self.session.checkpoint('name=' + ckpt_name)

    def check(self, ds, ckpt, nrows, value, ts):
        self.evict_metadata()
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        cfg = 'checkpoint=' + ckpt
        if ts is not None:
            cfg += ',debug=(checkpoint_read_timestamp=' + self.timestamp_str(ts) + ')'
        cursor = self.session.open_cursor(ds.uri, None, cfg)
        count = 0
        firstread = True
        self.evict_metadata()
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
            if firstread:
                firstread = False
                self.evict_metadata()
        self.assertEqual(count, nrows)
        cursor.close()

    def test_checkpoint(self):
        uri = 'table:checkpoint27'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100

        # Pin oldest and stable timestamps to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Write some data at time 10.
        self.large_updates(uri, ds, nrows, value_a, 10)

        # Write some more data at time 20.
        self.large_updates(uri, ds, nrows, value_b, 20)

        # Move stable up to 30.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # Checkpoint everything.
        self.do_checkpoint(self.first_checkpoint)

        # Now read the checkpoint.
        # It seems to be necessary to do one or two full scans of the checkpoint before the
        # metadata eviction works. Don't understand why, see WT-10236.
        self.check(ds, self.first_checkpoint, nrows, value_a, 15)
        self.check(ds, self.first_checkpoint, nrows, value_b, 25)

        # Now check for real.
        # The scan at 15 (the one that goes to history) is the one that is most likely to
        # have a problem, so do it last.
        self.check(ds, self.first_checkpoint, nrows, value_b, None) # default read ts
        self.check(ds, self.first_checkpoint, nrows, value_b, 0) # no read ts
        self.check(ds, self.first_checkpoint, nrows, value_b, 25)
        self.check(ds, self.first_checkpoint, nrows, value_a, 15)

if __name__ == '__main__':
    wttest.run()
