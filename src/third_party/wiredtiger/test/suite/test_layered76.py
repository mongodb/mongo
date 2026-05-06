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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered76.py
# Checkpoint size verification

@disagg_test_class
class test_layered76(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'

    create_session_config = 'key_format=i,value_format=S'

    uri = "layered:test_layered76"

    disagg_storages = gen_disagg_storages('test_layered66', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    def test_ckpt_size_verify_simple(self):
        self.session.create(self.uri, self.create_session_config)

        # Insert a key.
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'value1'
        cursor.close()

        # Do a checkpoint.
        self.session.checkpoint()

        self.verifyUntilSuccess()

    def test_ckpt_size_verify_multi_insert(self):
        self.session.create(self.uri, self.create_session_config)

        # Insert data.
        cursor = self.session.open_cursor(self.uri)
        for i in range(10):
            cursor[i] = 'a' * 100
        cursor.close()

        # Do a checkpoint.
        self.session.checkpoint()

        self.verifyUntilSuccess()

    def test_ckpt_size_verify_large_dataset(self):
        self.session.create(self.uri, self.create_session_config)

        # Insert data.
        cursor = self.session.open_cursor(self.uri)
        for i in range(100000):
            cursor[i] = 'a' * 100
        cursor.close()

        # Do a checkpoint.
        self.session.checkpoint()

        self.verifyUntilSuccess()

    def test_ckpt_size_verify_many_ckpt(self):
        session_config = 'key_format=S,value_format=S'
        nitems = 10000

        self.session.create(self.uri, session_config)

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(nitems):
            cursor["Key " + str(i)] = str(i)
        cursor.close()

        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(nitems):
            if i % 2 == 0:
                cursor["Key " + str(i)] = str(i) + "_even"
        cursor.close()

        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(nitems):
            if i % 100 == 0:
                cursor["Key " + str(i)] = str(i) + "_hundred"
        cursor.close()

        self.session.checkpoint()

        self.verifyUntilSuccess()

    def test_verify_db_size(self):
        self.session.create(self.uri, self.create_session_config)

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value' + str(i)
        cursor.close()

        self.session.checkpoint()

        # Verify the disaggregated database size by reopening with verify_metadata=true.
        # This triggers the database size check via the verify_metadata startup path, after
        # disaggregated storage has been fully initialized. We cannot call session.verify on
        # the metadata URI directly on a live connection because the metadata dhandle is
        # permanently held open and will always return EBUSY.
        self.reopen_conn(config=self.conn_config + ',verify_metadata=true')

    def test_verify_db_size_no_checkpoint(self):
        self.session.create(self.uri, self.create_session_config)

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value' + str(i)
        cursor.close()

        # Step down to follower before closing so WiredTiger skips the implicit shutdown
        # checkpoint. This leaves database_size=0 in the shared metadata and no btree
        # checkpoint sizes in the local metadata. The database size verify should detect
        # that both are zero and skip the comparison entirely.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.close_conn()

        # Open fresh connection, no checkpoint_meta since no checkpoint was ever taken.
        self.open_conn(config=self.conn_config + ',verify_metadata=true')

    def test_verify_db_size_deferred_checkpoint(self):
        self.session.create(self.uri, self.create_session_config)

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value' + str(i)
        cursor.close()

        self.conn.reconfigure('disaggregated=(role="follower")')
        self.close_conn()

        # Open fresh connection, database_size is 0 and no btree checkpoints exist, so the database
        # size verify skips the comparison entirely.
        self.open_conn(config=self.conn_config + ',verify_metadata=true')

        # The table was never checkpointed to shared metadata so it did not survive the
        # non-checkpointed close. Recreate it and take the first real checkpoint. The
        # checkpoint will initialize database_size to the fixed overhead buffer and then
        # apply the btree size delta on top.
        self.session.create(self.uri, self.create_session_config)
        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value' + str(i)
        cursor.close()
        self.session.checkpoint()

        # Verify that the stored database_size now correctly reflects the fixed overhead
        # plus the sum of btree checkpoint sizes.
        self.reopen_conn(config=self.conn_config + ',verify_metadata=true')

    def test_verify_db_size_follower_no_pickup(self):
        self.session.create(self.uri, self.create_session_config)

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value' + str(i)
        cursor.close()
        self.session.checkpoint()
        self.close_conn()

        # Reopen as follower without supplying checkpoint meta. No pickup happens during
        # open, so the in-memory database size stays 0 while the local metadata still
        # records the checkpoint just taken. The verify call must be skipped until a
        # pickup populates database size, otherwise it reports a spurious size mismatch.
        self.open_conn(
          config=self.conn_config + ',disaggregated=(role="follower"),verify_metadata=true')

    def test_verify_db_size_multi_table(self):
        uris = [
            'layered:test_layered76_a',
            'layered:test_layered76_b',
            'layered:test_layered76_c',
        ]
        for uri in uris:
            self.session.create(uri, self.create_session_config)

        # Checkpoint 1: write different amounts to each table.
        cursor_a = self.session.open_cursor(uris[0])
        cursor_b = self.session.open_cursor(uris[1])
        cursor_c = self.session.open_cursor(uris[2])
        for i in range(500):
            cursor_a[i] = 'a' * 50
        for i in range(1000):
            cursor_b[i] = 'b' * 200
        for i in range(200):
            cursor_c[i] = 'c' * 10
        cursor_a.close()
        cursor_b.close()
        cursor_c.close()
        self.session.checkpoint()

        # Checkpoint 2: update some rows in table_a, delete rows from table_b, leave table_c
        # untouched. This exercises negative deltas in the database_size accounting.
        cursor_a = self.session.open_cursor(uris[0])
        for i in range(0, 500, 2):
            cursor_a[i] = 'a' * 150
        cursor_a.close()

        cursor_b = self.session.open_cursor(uris[1])
        for i in range(0, 500):
            cursor_b.set_key(i)
            cursor_b.remove()
        cursor_b.close()
        self.session.checkpoint()

        # Checkpoint 3: add a large batch to table_c and overwrite all of table_a.
        cursor_a = self.session.open_cursor(uris[0])
        for i in range(500):
            cursor_a[i] = 'x' * 300
        cursor_a.close()

        cursor_c = self.session.open_cursor(uris[2])
        for i in range(200, 1000):
            cursor_c[i] = 'c' * 100
        cursor_c.close()
        self.session.checkpoint()

        # Reopen with verify_metadata=true. The stored database_size must equal the sum of
        # the most recent checkpoint size across all disaggregated btrees plus the fixed
        # overhead buffer.
        self.reopen_conn(config=self.conn_config + ',verify_metadata=true')
