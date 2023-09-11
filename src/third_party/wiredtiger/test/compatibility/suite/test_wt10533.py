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

import compatibility_test, os, shutil, sys, wiredtiger

class test_wt10533(compatibility_test.CompatibilityTestCase):
    '''
    Test handling checkpoints during database downgrade.
    '''

    conn_config = 'compatibility=(release="3.3"),' + \
        'statistics=(fast),statistics_log=(json,on_close,wait=5)'
    create_config = 'key_format=i,value_format=S'
    uri = 'table:test_wt10533'

    # Initial pass
    num_initial_rows = 100
    num_checkpoints = 100

    # Second pass
    num_second_rows_per_checkpoint = 10000
    num_second_checkpoints = 10

    def test_checkpoint_downgrade(self):
        '''
        Test downgrading after creating a large number of checkpoints, which forces the
        monotonic checkpoint time to race far ahead the wallclock time.
        '''
        self.run_method_on_branch(self.newer_branch, 'on_newer_branch_test_checkpoint_downgrade')
        self.run_method_on_branch(self.older_branch, 'on_older_branch_test_checkpoint_downgrade')

    def on_newer_branch_test_checkpoint_downgrade(self):
        '''
        The first part of the test, which runs on the newer branch.
        '''

        conn = wiredtiger.wiredtiger_open('.', 'create,' + self.conn_config)
        session = conn.open_session()

        self.pr(f'Running on {wiredtiger.wiredtiger_version()[0]}')

        # Create and populate a table.
        session.create(self.uri, self.create_config)

        c = session.open_cursor(self.uri)
        for i in range(1, self.num_initial_rows):
            c[i] = 'i' + str(i)
        c.close()

        # Create a lot of checkpoints to ensure that the checkpoint's "time" advances far.
        # If there is already a checkpoint with a given timestamp, WiredTiger adds another second,
        # and will keep adding more seconds, until it finds a unique timestamp that is larger than
        # any existing timestamp.
        for i in range(1, self.num_checkpoints):
            c = session.open_cursor(self.uri)
            c[self.num_initial_rows + i] = 'c' + str(i)
            c.close()
            session.checkpoint()

        # Close
        session.close()
        conn.close()

    def on_older_branch_test_checkpoint_downgrade(self):
        '''
        The second part of the test, which runs on the older branch.
        '''

        self.pr(f'Running on {wiredtiger.wiredtiger_version()[0]}')

        conn = wiredtiger.wiredtiger_open('.', self.conn_config)
        session = conn.open_session()

        # Start creating a backup.
        backup_path = 'backup'
        if os.path.exists(backup_path):
            shutil.rmtree(backup_path)
        os.mkdir(backup_path)
        backup_cursor = session.open_cursor('backup:')

        # Create a few checkpoints, each holding quite a bit of data.
        for i in range(0, self.num_second_checkpoints):
            c = session.open_cursor(self.uri)
            for j in range(1, self.num_second_rows_per_checkpoint):
                c[i * 1000000 + j] = 'x' + str(i) + '-' + str(j)
            c.close()
            session.checkpoint()

        # Now actually create the backup.
        while backup_cursor.next() == 0:
            file_name = backup_cursor.get_key()
            shutil.copy(file_name, os.path.join(backup_path, file_name))
        backup_cursor.close()

        session.close()
        conn.close()

        # And let's see if we can open the backup and verify that it has the right data.
        conn = wiredtiger.wiredtiger_open(backup_path, self.conn_config)
        session = conn.open_session()

        c = session.open_cursor(self.uri)
        for i in range(1, self.num_initial_rows):
            v = 'i' + str(i)
            if c[i] != v:
                sys.stderr.write(f'Data mismatch on key {i}: expected {v}, got {c[i]}\n')
                self.assertEqual(c[i], v)
        c.close()

        session.close()
        conn.close()

if __name__ == '__main__':
    compatibility_test.run()
