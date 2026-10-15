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

import os
import re
import threading
import time
import wiredtiger, wttest
from wiredtiger import stat
from wtbackup import backup_base

# A dropped table's file is gone when drop returns and the name can be reused immediately.
class test_drop05(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'
    uri = 'table:test_drop05'
    filename = 'test_drop05.wt'
    nrows = 1000

    def setUp(self):
        if self.runningHook('disagg') or self.runningHook('tiered'):
            self.skipTest('asserts on the physical .wt file of a table')
        super().setUp()

    def populate(self, value):
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = value
        cursor.close()

    def create_and_checkpoint(self, value):
        self.session.create(self.uri, 'key_format=Q,value_format=S')
        self.populate(value)
        self.session.checkpoint()
        self.assertTrue(os.path.exists(os.path.join(self.home, self.filename)))

    def leftover_files(self):
        return [f for f in os.listdir(self.home) if '.wtdrop' in f]

    def btree_id(self):
        cursor = self.session.open_cursor('metadata:')
        config = cursor['file:' + self.filename]
        cursor.close()
        return int(re.search(r'(?<![a-z_])id=(\d+)', config).group(1))

    def test_drop_then_create(self):
        self.create_and_checkpoint('old' * 20)
        applied = self.get_stat(stat.conn.session_table_drop_deferred_applied)
        self.session.drop(self.uri)
        self.assertGreater(self.get_stat(stat.conn.session_table_drop_deferred_applied), applied)
        self.create_and_checkpoint('new' * 20)
        # No file with the table's name prefix remains other than the new table's file.
        self.assertEqual([f for f in os.listdir(self.home) if f.startswith(self.filename)],
                         [self.filename])
        self.reopen_conn()
        cursor = self.session.open_cursor(self.uri)
        count = 0
        for _, value in cursor:
            self.assertEqual(value, 'new' * 20)
            count += 1
        cursor.close()
        self.assertEqual(count, self.nrows)

    def test_drop_removes_file(self):
        self.create_and_checkpoint('a' * 50)
        applied = self.get_stat(stat.conn.session_table_drop_deferred_applied)
        self.session.drop(self.uri)
        self.assertFalse(os.path.exists(os.path.join(self.home, self.filename)))
        self.assertEqual(self.leftover_files(), [])
        self.assertGreater(self.get_stat(stat.conn.session_table_drop_deferred_applied), applied)

    # A drop never replaces a file already holding one of its rename targets.
    def test_drop_around_existing_files(self):
        self.create_and_checkpoint('a' * 50)
        btree_id = self.btree_id()
        existing = ['%s.%d.wtdrop' % (self.filename, btree_id),
                    '%s.%d.wtdrop.1' % (self.filename, btree_id)]
        for name in existing:
            with open(os.path.join(self.home, name), 'w') as f:
                f.write(name)
        self.session.drop(self.uri)
        self.assertFalse(os.path.exists(os.path.join(self.home, self.filename)))
        self.assertEqual(sorted(self.leftover_files()), sorted(existing))
        for name in existing:
            with open(os.path.join(self.home, name)) as f:
                self.assertEqual(f.read(), name)

    def test_drop_table_with_indexes(self):
        nindexes = 3
        self.session.create(self.uri, 'key_format=Q,value_format=S,columns=(k,v)')
        for i in range(nindexes):
            self.session.create('index:test_drop05:i%d' % i, 'columns=(v)')
        self.populate('a' * 50)
        self.session.checkpoint()
        files = [self.filename] + ['test_drop05_i%d.wti' % i for i in range(nindexes)]
        for name in files:
            self.assertTrue(os.path.exists(os.path.join(self.home, name)))
        applied = self.get_stat(stat.conn.session_table_drop_deferred_applied)
        self.session.drop(self.uri)
        for name in files:
            self.assertFalse(os.path.exists(os.path.join(self.home, name)))
        self.assertEqual(self.leftover_files(), [])
        # Every file of the table is renamed away and then removed.
        self.assertEqual(
            self.get_stat(stat.conn.session_table_drop_deferred_applied) - applied, len(files))

    def test_drop_keeps_file(self):
        self.create_and_checkpoint('a' * 50)
        applied = self.get_stat(stat.conn.session_table_drop_deferred_applied)
        self.session.drop(self.uri, 'remove_files=false')
        self.assertTrue(os.path.exists(os.path.join(self.home, self.filename)))
        self.assertEqual(self.leftover_files(), [])
        self.assertEqual(self.get_stat(stat.conn.session_table_drop_deferred_applied), applied)

class test_drop05_inmem(wttest.WiredTigerTestCase):
    conn_config = 'in_memory=true,statistics=(all)'
    uri = 'table:test_drop05_inmem'

    def test_drop_then_create(self):
        self.session.create(self.uri, 'key_format=Q,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'old'
        cursor.close()
        self.session.drop(self.uri)
        self.session.create(self.uri, 'key_format=Q,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'new'
        self.assertEqual(cursor[1], 'new')
        cursor.close()
        self.assertEqual(self.get_stat(stat.conn.session_table_drop_deferred_applied), 0)

class test_drop05_live_restore(backup_base):
    conn_config = 'statistics=(all)'
    uri = 'table:test_drop05'
    filename = 'test_drop05.wt'

    def test_drop_live_restore(self):
        if os.name == 'nt':
            self.skipTest('Live restore is not supported on Windows')
        self.session.create(self.uri, 'key_format=Q,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        for i in range(1000):
            cursor[i] = 'a' * 50
        cursor.close()
        self.session.checkpoint()
        os.mkdir('SOURCE')
        self.take_full_backup('SOURCE')
        self.close_conn()

        os.mkdir('DEST')
        self.open_conn('DEST',
            config='statistics=(all),live_restore=(enabled=true,path=SOURCE,threads_max=0)')
        self.session.drop(self.uri)
        for home in ('DEST', 'SOURCE'):
            self.assertEqual([f for f in os.listdir(home) if '.wtdrop' in f], [])
        self.assertFalse(os.path.exists(os.path.join('DEST', self.filename)))
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(self.uri))

# A schema operation on another table completes while a drop's deferred file removal is still
# running.
class test_drop05_lock_release(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all),timing_stress_for_test=[drop_deferred_hold]'

    def setUp(self):
        if self.runningHook('disagg') or self.runningHook('tiered'):
            self.skipTest('asserts on the physical .wt file of a table')
        super().setUp()

    def test_removal_runs_outside_locks(self):
        drop_uri = 'table:test_drop05_drop'
        drop_file = 'test_drop05_drop.wt'
        other_uri = 'table:test_drop05_other'
        self.session.create(drop_uri, 'key_format=Q,value_format=S')
        c = self.session.open_cursor(drop_uri)
        for i in range(100):
            c[i] = 'a' * 50
        c.close()
        self.session.checkpoint()

        order = []
        errors = []
        order_lock = threading.Lock()

        def deferred(s):
            c = s.open_cursor('statistics:')
            value = c[stat.conn.session_table_drop_deferred][2]
            c.close()
            return value

        def dropper():
            try:
                s = self.conn.open_session()
                s.drop(drop_uri)
                with order_lock:
                    order.append('drop')
                s.close()
            except Exception as e:
                errors.append(e)

        def creator():
            try:
                s = self.conn.open_session()
                deadline = time.time() + 30
                while deferred(s) == 0:
                    self.assertLess(time.time(), deadline, 'the drop never deferred a removal')
                    time.sleep(0.01)
                s.create(other_uri, 'key_format=Q,value_format=S')
                c = s.open_cursor(other_uri)
                c[1] = 'b'
                c.close()
                with order_lock:
                    order.append('create')
                s.close()
            except Exception as e:
                errors.append(e)

        td = threading.Thread(target=dropper)
        tc = threading.Thread(target=creator)
        td.start()
        tc.start()
        try:
            tc.join(60)
            self.assertFalse(tc.is_alive(), 'the concurrent create did not finish during the drop')
        finally:
            self.conn.reconfigure('timing_stress_for_test=[]')
            td.join(60)
        self.assertFalse(td.is_alive(), 'the drop did not finish')
        if errors:
            raise errors[0]

        self.assertFalse(os.path.exists(os.path.join(self.home, drop_file)))
        self.assertEqual(order, ['create', 'drop'])

if __name__ == '__main__':
    wttest.run()
