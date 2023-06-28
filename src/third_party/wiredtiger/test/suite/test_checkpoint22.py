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

# test_checkpoint22.py
#
# Test that skipping trees in checkpoints doesn't cause us to use the wrong
# write generation.
#
# If an item contains txnids, they're hidden during unpack based on the write
# generation. The checkpoint cursor code contains logic to use the proper write
# generation for the checkpoint (which might not be the same as the current
# write generation) but there are opportunities for this to be wrong if the tree
# is not modified for a while and thus skipped during checkpointing.
#
# In this test we take an initial checkpoint while holding a read transaction
# open (to ensure txnids are written out), then without making more changes to
# the tree, shut down and restart and take another checkpoint. If we have messed
# up, reading the second checkpoint will use the write generation from before
# the restart and values we should see will disappear.
#
# To make this work we need to make sure the txnids after the restart are
# substantially smaller than the ones we need to see from before, so do 10000
# extra transactions up front before doing anything else.
#
# A more advanced variant of this test might try reading from _both_ checkpoints
# after restarting to make sure anything that should not be visible in the first
# checkpoint (because it wasn't done committing) is visible in the first
# checkpoint and not the second. However, to make that go we need to not just
# generate a torn transaction (which is enough of a nuisance already) but one
# where the entire transaction, or at least the entire transaction that applies
# to the tree we care about, gets written out in the checkpoint but that
# transaction's txnid is still not in the checkpoint's snapshot. Doing this
# reliably seems beyond the facilities we have available in Python -- writing a
# large amount of data to a second table and making the changes in the main table
# small might help but seems unlikely to be particularly reliable under load.
#
# If we gain a better way to generate torn transactions on purpose, it might be
# worth seeing if this more advanced variant can be set up. Note that for this
# case the first checkpoint must be named so it remains addressable.
#
# This test doesn't use timestamps; it is about transaction-level visibility.
# There doesn't seem any immediate reason to think timestamps would add anything.

class test_checkpoint(wttest.WiredTigerTestCase):

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    first_name_values = [
        ('named', dict(first_checkpoint='first_checkpoint')),
        ('unnamed', dict(first_checkpoint=None)),
    ]
    second_name_values = [
        ('named', dict(second_checkpoint='second_checkpoint')),
        ('unnamed', dict(second_checkpoint=None)),
    ]
    scenarios = make_scenarios(format_values, first_name_values, second_name_values)
        

    def do_checkpoint(self, ckpt_name):
        if ckpt_name is None:
            self.session.checkpoint()
        else:
            self.session.checkpoint('name=' + ckpt_name)

    def check(self, ds, ckpt, nrows, value):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        cfg = 'checkpoint=' + ckpt
        cursor = self.session.open_cursor(ds.uri, None, cfg)
        #self.session.begin_transaction()
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        #self.session.rollback_transaction()
        self.assertEqual(count, nrows)
        cursor.close()

    def test_checkpoint(self):
        uri = 'table:checkpoint22'
        uri2 = 'table:checkpoint22a'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        # Create a second table so the second checkpoint can avoid being entirely vacuous.
        ds2 = SimpleDataSet(
            self, uri2, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds2.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100

        # Write some initial data, and then write more, to crank up the txnid counter.
        cursor = self.session.open_cursor(ds.uri, None, None)
        for i in range(10000 // nrows):
            for k in range(1, nrows + 1):
                self.session.begin_transaction()
                cursor[ds.key(k)] = 40 + i if self.value_format == '8t' else str(i) + value_a
                self.session.commit_transaction()

        # Put some material in the second table too to keep things from being degenerate.
        self.session.begin_transaction()
        cursor2 = self.session.open_cursor(ds2.uri, None, None)
        for k in range(1, 10):
            cursor2[ds2.key(k)] = value_a
        self.session.commit_transaction()
        cursor2.close()

        # Create a reader transaction that will not be able to see what happens next, so its
        # txnids will end up on disk. We don't need to do anything with this; it just needs
        # to exist.
        session2 = self.conn.open_session()
        session2.begin_transaction()

        # Now write some more data that we'll expect to see below.
        self.session.begin_transaction()
        for k in range(1, nrows + 1):
            cursor[ds.key(k)] = value_b
        self.session.commit_transaction()

        # Checkpoint.
        self.do_checkpoint(self.first_checkpoint)

        # Tidy up.
        session2.rollback_transaction()
        session2.close()
        cursor.close()

        # Shut down and restart.
        self.reopen_conn()

        # Write some stuff in the second table.
        cursor2 = self.session.open_cursor(ds2.uri, None, None)
        self.session.begin_transaction()
        for k in range(1, 10):
            cursor2[ds2.key(k)] = value_c
        self.session.commit_transaction()
        cursor2.close()

        # Checkpoint.
        self.do_checkpoint(self.second_checkpoint)

        # Now read the first table in the second checkpoint. We should see value_b.
        self.check(ds, self.second_checkpoint, nrows, value_b)

if __name__ == '__main__':
    wttest.run()
