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
# checkpoint:metadata
# [END_TAGS]

import wttest
from wtdataset import SimpleDataSet

# Test WT_CURSOR::checkpoint_id(). It returns an identifier for the checkpoint a
# checkpoint cursor is reading, or 0 for a cursor that isn't on a checkpoint.
# Every object reading the same checkpoint reports the same identifier, which
# lets applications confirm that checkpoint cursors opened on different objects
# are looking at the same database checkpoint.
# FIXME-WT-15357: Checkpoint cursors on layered tables are not supported.
@wttest.skip_for_hook('disagg', 'checkpoint cursors on layered tables are not supported')
class test_checkpoint_id(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri1 = 'table:' + test_name + '_1'
    uri2 = 'table:' + test_name + '_2'
    nrows = 100

    def setUp(self):
        super(test_checkpoint_id, self).setUp()
        self.ds1 = SimpleDataSet(self, self.uri1, self.nrows)
        self.ds2 = SimpleDataSet(self, self.uri2, self.nrows)
        self.ds1.populate()
        self.ds2.populate()

    def checkpoint(self, name=None):
        if name is None:
            self.session.checkpoint()
        else:
            self.session.checkpoint('name=' + name)

    def ckpt_id(self, uri, name='WiredTigerCheckpoint'):
        cursor = self.session.open_cursor(uri, None, 'checkpoint=' + name)
        try:
            return cursor.checkpoint_id()
        finally:
            cursor.close()

    # Make a table dirty so the next checkpoint actually writes something.
    def add_record(self, ds):
        cursor = self.session.open_cursor(ds.uri, None)
        cursor[ds.key(self.nrows + 1)] = ds.value(self.nrows + 1)
        cursor.close()

    # A cursor that isn't reading a checkpoint returns 0.
    def test_not_a_checkpoint(self):
        cursor = self.session.open_cursor(self.uri1, None)
        self.assertEqual(cursor.checkpoint_id(), 0)
        cursor.close()

    # Scenario 1: checkpointing the same table twice gives cursors opened on
    # each checkpoint different identifiers.
    def test_two_checkpoints_differ(self):
        self.checkpoint()
        id1 = self.ckpt_id(self.uri1)
        self.assertNotEqual(id1, 0)
        self.add_record(self.ds1)
        self.checkpoint()
        self.assertNotEqual(self.ckpt_id(self.uri1), id1)

    # Scenario 2: tables checkpointed together share an identifier.
    def test_checkpointed_together(self):
        self.checkpoint()
        self.assertEqual(self.ckpt_id(self.uri1), self.ckpt_id(self.uri2))

    # Scenario 3: tables checkpointed separately still share an identifier when
    # the cursors are opened after both checkpoints, as they both read the most
    # recent database checkpoint.
    def test_checkpointed_separately(self):
        self.checkpoint()
        self.add_record(self.ds2)
        self.checkpoint()
        self.assertEqual(self.ckpt_id(self.uri1), self.ckpt_id(self.uri2))

    # Scenario 4: a cursor opened before a later checkpoint keeps the older
    # identifier.
    def test_cursor_keeps_identifier(self):
        self.checkpoint()
        cursor = self.session.open_cursor(self.uri1, None, 'checkpoint=WiredTigerCheckpoint')
        id1 = cursor.checkpoint_id()
        self.assertNotEqual(id1, 0)
        self.add_record(self.ds2)
        self.checkpoint()
        self.assertNotEqual(self.ckpt_id(self.uri2), id1)
        cursor.close()

    # A named checkpoint has its own identifier, stable across later checkpoints.
    def test_named_checkpoint(self):
        self.checkpoint('cp1')
        id1 = self.ckpt_id(self.uri1, 'cp1')
        self.assertNotEqual(id1, 0)
        self.add_record(self.ds2)
        self.checkpoint('cp2')
        self.assertEqual(self.ckpt_id(self.uri1, 'cp1'), id1)
        self.assertNotEqual(self.ckpt_id(self.uri2, 'cp2'), id1)
