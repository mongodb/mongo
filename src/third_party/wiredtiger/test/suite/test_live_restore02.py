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

import os, glob, time, wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wtbackup import backup_base

# test_live_restore02.py
# Enable background thread migration and loop until it completes
class test_live_restore02(backup_base):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    read_sizes = [
        ('512B', dict(read_size='512B')),
        ('4KB', dict(read_size='4KB')),
        ('1MB', dict(read_size='1MB'))
    ]

    scenarios = make_scenarios(format_values, read_sizes)
    nrows = 10000

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        # Ensure we have logged the live restore messages.
        live_restore_patterns = [
            r"Starting \d+ threads to restore \d+ files",
            r"Live restore background migration finished restoring \d+ files in \d+ seconds",
            r"Live restore has finished"
        ]
        self.ignoreStdoutPattern("|".join(live_restore_patterns))

    def get_stat(self, statistic):
        stat_cursor = self.session.open_cursor("statistics:")
        val = stat_cursor[statistic][2]
        stat_cursor.close()
        return val

    def test_live_restore02(self):
        # Live restore is not supported on Windows.
        if os.name == 'nt':
            return

        uris = ['file:foo', 'file:bar', 'file:cat']
        # Create a data set with a 3 collections to restore on restart.
        # Populate 3 collections
        ds0 = SimpleDataSet(self, uris[0], self.nrows,
          key_format=self.key_format, value_format=self.value_format)
        ds0.populate()
        ds1 = SimpleDataSet(self, uris[1], self.nrows,
          key_format=self.key_format, value_format=self.value_format)
        ds1.populate()
        ds2 = SimpleDataSet(self, uris[2], self.nrows,
          key_format=self.key_format, value_format=self.value_format)
        ds2.populate()

        self.session.checkpoint()

        # Close the default connection.
        os.mkdir("SOURCE")
        self.take_full_backup("SOURCE")
        self.close_conn()

        # Remove everything but SOURCE / stderr / stdout.
        for f in glob.glob("*"):
            if not f == "SOURCE" and not f == "stderr.txt" and not f == "stdout.txt":
                os.remove(f)

        os.mkdir("DEST")
        self.open_conn("DEST", config="statistics=(all),verbose=[live_restore_progress:1],live_restore=(enabled=true,path=\"SOURCE\",threads_max=1,read_size=" + self.read_size + ")")

        state = 0
        timeout = 120
        iteration_count = 0
        # Build in a 2 minute timeout. Once we see the complete state exit the loop.
        while (iteration_count < timeout):
            state = self.get_stat(stat.conn.live_restore_state)
            # Stress the file create path in the meantime, this checks some assert conditions.
            self.session.create(f'file:abc{iteration_count}', f'key_format={self.key_format},value_format={self.value_format}')
            self.pr(f'Looping until finish, live restore state is: {state}, \
                      Current iteration: is {iteration_count}')
            # State 2 means the live restore has completed.
            if (state == wiredtiger.WT_LIVE_RESTORE_COMPLETE):
                break
            time.sleep(1)
            iteration_count += 1
        self.assertEqual(state, wiredtiger.WT_LIVE_RESTORE_COMPLETE)

        conn2 = self.setUpConnectionOpen('SOURCE/')
        session2 = self.setUpSessionOpen(conn2)
        session = self.setUpSessionOpen(self.conn)
        # Validate that the collections in source match ours in the destination.
        for uri in uris:
            cursor = session.open_cursor(uri)
            cursor2 = session2.open_cursor(uri)
            while True:
                ret = cursor.next()
                ret2 = cursor2.next()
                if ret != 0:
                    self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
                    self.assertEqual(ret2, wiredtiger.WT_NOTFOUND)
                    break
                self.assertEqual(cursor.get_key(), cursor2.get_key())
                self.assertEqual(cursor.get_value(), cursor2.get_value())

        assert(len(glob.glob('WT_DEST/*.stop')) == 0)
